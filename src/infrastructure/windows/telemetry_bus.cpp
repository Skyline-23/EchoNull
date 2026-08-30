#include "infrastructure/windows/telemetry_bus.hpp"

#include <Sddl.h>

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace echonull {
namespace {

constexpr wchar_t kGlobalMappingName[] = L"Global\\EchoNull.Telemetry.v2";
constexpr wchar_t kLocalMappingName[] = L"Local\\EchoNull.Telemetry.v2";
constexpr std::uint32_t kMagic = 0x454E544DU;  // ENTM
constexpr std::uint32_t kVersion = 1;

struct SharedTelemetry {
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  volatile LONG sequence = 0;
  std::uint32_t reserved = 0;
  TelemetrySnapshot snapshot;
};

SharedTelemetry* state(void* view) {
  return static_cast<SharedTelemetry*>(view);
}

std::int64_t qpc_hns() {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<std::int64_t>(
      static_cast<long double>(counter.QuadPart) * 10'000'000.0L /
      static_cast<long double>(frequency.QuadPart));
}

void close_mapping(HANDLE& mapping, void*& view) noexcept {
  if (view != nullptr) {
    UnmapViewOfFile(view);
    view = nullptr;
  }
  if (mapping != nullptr) {
    CloseHandle(mapping);
    mapping = nullptr;
  }
}

}  // namespace

bool is_windows_audio_engine_process() {
  std::wstring path(32'768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                          static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return false;
  path.resize(length);
  auto name = std::filesystem::path(path).filename().wstring();
  std::transform(name.begin(), name.end(), name.begin(),
                 [](const wchar_t value) {
                   return static_cast<wchar_t>(std::towlower(value));
                 });
  return name == L"audiodg.exe";
}

TelemetryBusWriter::~TelemetryBusWriter() { close(); }

void TelemetryBusWriter::open() {
  close();
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GR;;;AU)", SDDL_REVISION_1,
          &descriptor, nullptr)) {
    throw std::runtime_error("could not create telemetry security descriptor");
  }
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  mapping_ = CreateFileMappingW(
      INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
      static_cast<DWORD>(sizeof(SharedTelemetry)), kGlobalMappingName);
  if (mapping_ == nullptr && GetLastError() == ERROR_ACCESS_DENIED) {
    mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(SharedTelemetry)), kLocalMappingName);
  }
  LocalFree(descriptor);
  if (mapping_ == nullptr) {
    throw std::runtime_error("could not create EchoNull telemetry mapping");
  }
  const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
  view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                        sizeof(SharedTelemetry));
  if (view_ == nullptr) {
    close();
    throw std::runtime_error("could not map EchoNull telemetry");
  }
  auto* shared = state(view_);
  if (created) {
    std::memset(shared, 0, sizeof(*shared));
    shared->version = kVersion;
    MemoryBarrier();
    shared->magic = kMagic;
  } else if (shared->magic != kMagic || shared->version != kVersion) {
    close();
    throw std::runtime_error("EchoNull telemetry layout is incompatible");
  }
}

void TelemetryBusWriter::close() noexcept { close_mapping(mapping_, view_); }

void TelemetryBusWriter::publish(const TelemetrySnapshot& snapshot) noexcept {
  if (view_ == nullptr) return;
  auto* shared = state(view_);
  InterlockedIncrement(&shared->sequence);
  MemoryBarrier();
  shared->snapshot = snapshot;
  MemoryBarrier();
  InterlockedIncrement(&shared->sequence);
}

TelemetryBusReader::~TelemetryBusReader() { close(); }

bool TelemetryBusReader::open() {
  close();
  mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kLocalMappingName);
  if (mapping_ == nullptr) {
    mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kGlobalMappingName);
  }
  if (mapping_ == nullptr) return false;
  view_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(SharedTelemetry));
  if (view_ == nullptr) {
    close();
    return false;
  }
  const auto* shared = state(view_);
  if (shared->magic != kMagic || shared->version != kVersion) {
    close();
    return false;
  }
  return true;
}

void TelemetryBusReader::close() noexcept { close_mapping(mapping_, view_); }

std::optional<TelemetrySnapshot> TelemetryBusReader::read_latest() {
  if (view_ == nullptr && !open()) return std::nullopt;
  const auto* shared = state(view_);
  for (int attempt = 0; attempt < 4; ++attempt) {
    const LONG before = shared->sequence;
    if ((before & 1) != 0) continue;
    MemoryBarrier();
    const TelemetrySnapshot snapshot = shared->snapshot;
    MemoryBarrier();
    const LONG after = shared->sequence;
    if (before == after && snapshot.timestamp_hns != 0 &&
        qpc_hns() - snapshot.timestamp_hns <= 20'000'000) {
      return snapshot;
    }
  }
  // audiodg.exe is recreated whenever Equalizer APO applies a configuration.
  // Drop the old mapping so the editor reconnects to the new audio-engine
  // instance on its next timer tick instead of displaying a frozen snapshot.
  close();
  return std::nullopt;
}

}  // namespace echonull
