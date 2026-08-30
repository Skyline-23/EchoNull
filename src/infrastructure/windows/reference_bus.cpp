#include "infrastructure/windows/reference_bus.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace echonull {
namespace {

constexpr wchar_t kMappingName[] = L"Local\\EchoNull.Reference.v1";
constexpr std::uint32_t kMagic = 0x454E5246U;  // ENRF
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kBlockCount = 256;
constexpr std::size_t kSamplesPerBlock = 4096;

struct alignas(64) SharedBlock {
  volatile LONG64 sequence = 0;
  LONG64 timestamp_hns = 0;
  std::uint32_t sample_count = 0;
  std::uint32_t reserved = 0;
  float samples[kSamplesPerBlock]{};
};

struct alignas(64) SharedState {
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t block_count = 0;
  std::uint32_t samples_per_block = 0;
  std::uint32_t reserved[11]{};
  volatile LONG64 next_sequence = 0;
  volatile LONG64 published_sequence = 0;
  SharedBlock blocks[kBlockCount];
};

SharedState* state(void* view) {
  return static_cast<SharedState*>(view);
}

void validate(const SharedState& shared) {
  if (shared.magic != kMagic || shared.version != kVersion ||
      shared.sample_rate != 48'000 || shared.block_count != kBlockCount ||
      shared.samples_per_block != kSamplesPerBlock) {
    throw std::runtime_error("EchoNull reference bus has an incompatible layout");
  }
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

ReferenceBusWriter::ReferenceBusWriter() = default;
ReferenceBusWriter::~ReferenceBusWriter() { close(); }

void ReferenceBusWriter::open() {
  close();
  mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                static_cast<DWORD>(sizeof(SharedState)), kMappingName);
  if (mapping_ == nullptr) {
    throw std::runtime_error("CreateFileMappingW failed for the EchoNull reference bus");
  }
  const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
  view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
  if (view_ == nullptr) {
    close();
    throw std::runtime_error("MapViewOfFile failed for the EchoNull reference bus");
  }
  auto* shared = state(view_);
  if (created) {
    std::memset(shared, 0, sizeof(SharedState));
    shared->version = kVersion;
    shared->sample_rate = 48'000;
    shared->block_count = static_cast<std::uint32_t>(kBlockCount);
    shared->samples_per_block = static_cast<std::uint32_t>(kSamplesPerBlock);
    MemoryBarrier();
    shared->magic = kMagic;
  } else {
    validate(*shared);
  }
}

void ReferenceBusWriter::close() noexcept {
  close_mapping(mapping_, view_);
}

void ReferenceBusWriter::publish(const std::int64_t timestamp_hns,
                                 const std::span<const float> samples) {
  if (view_ == nullptr || samples.empty()) return;
  auto* shared = state(view_);
  std::size_t offset = 0;
  while (offset < samples.size()) {
    const auto count = std::min(kSamplesPerBlock, samples.size() - offset);
    const LONG64 sequence = InterlockedIncrement64(&shared->next_sequence);
    auto& block = shared->blocks[static_cast<std::size_t>(sequence) % kBlockCount];
    block.timestamp_hns = timestamp_hns + static_cast<std::int64_t>(
        offset * 10'000'000ULL / 48'000ULL);
    block.sample_count = static_cast<std::uint32_t>(count);
    std::memcpy(block.samples, samples.data() + offset, count * sizeof(float));
    MemoryBarrier();
    InterlockedExchange64(&block.sequence, sequence);
    InterlockedExchange64(&shared->published_sequence, sequence);
    offset += count;
  }
}

ReferenceBusReader::ReferenceBusReader() = default;
ReferenceBusReader::~ReferenceBusReader() { close(); }

bool ReferenceBusReader::open() {
  close();
  mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kMappingName);
  if (mapping_ == nullptr) return false;
  view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
  if (view_ == nullptr) {
    close();
    return false;
  }
  try {
    validate(*state(view_));
  } catch (...) {
    close();
    throw;
  }
  return true;
}

void ReferenceBusReader::close() noexcept {
  close_mapping(mapping_, view_);
  last_sequence_ = 0;
}

std::vector<ReferenceBusBlock> ReferenceBusReader::read_available() {
  std::vector<ReferenceBusBlock> result;
  if (view_ == nullptr && !open()) return result;

  const auto* shared = state(view_);
  const LONG64 latest = InterlockedCompareExchange64(
      const_cast<volatile LONG64*>(&shared->published_sequence), 0, 0);
  if (latest <= last_sequence_) return result;

  const LONG64 first = std::max(last_sequence_ + 1,
                                latest - static_cast<LONG64>(kBlockCount) + 1);
  result.reserve(static_cast<std::size_t>(latest - first + 1));
  for (LONG64 sequence = first; sequence <= latest; ++sequence) {
    const auto& block = shared->blocks[static_cast<std::size_t>(sequence) % kBlockCount];
    const LONG64 before = InterlockedCompareExchange64(
        const_cast<volatile LONG64*>(&block.sequence), 0, 0);
    if (before != sequence || block.sample_count > kSamplesPerBlock) continue;

    ReferenceBusBlock copy;
    copy.timestamp_hns = block.timestamp_hns;
    copy.samples.assign(block.samples, block.samples + block.sample_count);
    MemoryBarrier();
    const LONG64 after = InterlockedCompareExchange64(
        const_cast<volatile LONG64*>(&block.sequence), 0, 0);
    if (after == sequence) result.push_back(std::move(copy));
  }
  last_sequence_ = latest;
  return result;
}

}  // namespace echonull
