#include "infrastructure/nvidia/packaged_runtime.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "infrastructure/bundle_format.hpp"
#if ECHONULL_HAS_NVAFX
#include "infrastructure/nvidia/nvafx_api.hpp"
#endif

namespace echonull {
namespace {

struct Asset {
  std::string name;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

constexpr std::size_t kCopyBufferSize = 4U * 1024U * 1024U;
std::mutex g_extract_mutex;

std::wstring utf8_name(const std::string& value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0);
  if (length <= 0) throw std::runtime_error("bundle contains an invalid UTF-8 name");
  std::wstring output(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), output.data(),
                          length) != length) {
    throw std::runtime_error("bundle asset name conversion failed");
  }
  return output;
}

bool valid_name(const std::string& name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string::npos &&
         name.find('\\') == std::string::npos &&
         name.find(':') == std::string::npos;
}

std::filesystem::path cache_root(const std::uint64_t bundle_id) {
  std::wstring temporary(32'768, L'\0');
  const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()),
                                    temporary.data());
  if (length == 0 || length >= temporary.size()) {
    throw std::runtime_error("Windows temporary directory is unavailable");
  }
  temporary.resize(length);
  std::wostringstream identifier;
  identifier << std::hex << std::setfill(L'0') << std::setw(16) << bundle_id;
  return std::filesystem::path(temporary) / L"EchoNull" / identifier.str();
}

std::vector<Asset> read_index(std::ifstream& input,
                              const std::uint64_t file_size,
                              echonull::bundle::Footer& footer) {
  if (file_size < sizeof(footer)) {
    throw std::runtime_error("EchoNullPlugin.dll has no embedded runtime bundle");
  }
  input.seekg(static_cast<std::streamoff>(file_size - sizeof(footer)));
  input.read(reinterpret_cast<char*>(&footer), sizeof(footer));
  if (!input || footer.magic != bundle::kMagic ||
      footer.version != bundle::kVersion) {
    throw std::runtime_error("EchoNullPlugin.dll has no embedded runtime bundle");
  }
  const auto footer_offset = file_size - sizeof(footer);
  if (footer.index_offset < footer.original_size ||
      footer.index_size > footer_offset ||
      footer.index_offset > footer_offset - footer.index_size ||
      footer.index_offset + footer.index_size != footer_offset ||
      footer.entry_count == 0 || footer.entry_count > 128) {
    throw std::runtime_error("EchoNull runtime bundle index is corrupt");
  }

  input.seekg(static_cast<std::streamoff>(footer.index_offset));
  std::vector<Asset> assets;
  assets.reserve(footer.entry_count);
  std::uint64_t consumed = 0;
  for (std::uint32_t index = 0; index < footer.entry_count; ++index) {
    bundle::Entry entry{};
    input.read(reinterpret_cast<char*>(&entry), sizeof(entry));
    consumed += sizeof(entry);
    if (!input || entry.name_size == 0 || entry.name_size > 255 ||
        entry.offset < footer.original_size || entry.offset > footer.index_offset ||
        entry.size > footer.index_offset - entry.offset ||
        consumed + entry.name_size > footer.index_size) {
      throw std::runtime_error("EchoNull runtime bundle entry is corrupt");
    }
    std::string name(entry.name_size, '\0');
    input.read(name.data(), static_cast<std::streamsize>(name.size()));
    consumed += entry.name_size;
    if (!input || !valid_name(name)) {
      throw std::runtime_error("EchoNull runtime bundle contains an unsafe filename");
    }
    assets.push_back(Asset{std::move(name), entry.offset, entry.size});
  }
  if (consumed != footer.index_size) {
    throw std::runtime_error("EchoNull runtime bundle index length is invalid");
  }
  return assets;
}

bool correct_size(const std::filesystem::path& path, const std::uint64_t size) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error &&
         std::filesystem::file_size(path, error) == size && !error;
}

std::optional<std::string> preferred_gpu_architecture() {
  const HMODULE cuda = LoadLibraryW(L"nvcuda.dll");
  if (cuda == nullptr) return std::nullopt;
  using CuInit = int(WINAPI*)(unsigned int);
  using CuDeviceGetCount = int(WINAPI*)(int*);
  using CuDeviceGet = int(WINAPI*)(int*, int);
  using CuDeviceGetAttribute = int(WINAPI*)(int*, int, int);
  const auto init = reinterpret_cast<CuInit>(GetProcAddress(cuda, "cuInit"));
  const auto get_count = reinterpret_cast<CuDeviceGetCount>(
      GetProcAddress(cuda, "cuDeviceGetCount"));
  const auto get_device = reinterpret_cast<CuDeviceGet>(
      GetProcAddress(cuda, "cuDeviceGet"));
  const auto get_attribute = reinterpret_cast<CuDeviceGetAttribute>(
      GetProcAddress(cuda, "cuDeviceGetAttribute"));
  if (init == nullptr || get_count == nullptr || get_device == nullptr ||
      get_attribute == nullptr || init(0) != 0) {
    FreeLibrary(cuda);
    return std::nullopt;
  }
  constexpr int kComputeCapabilityMajor = 75;
  constexpr int kComputeCapabilityMinor = 76;
  int count = 0;
  int best_major = 0;
  int best_minor = 0;
  if (get_count(&count) == 0) {
    for (int ordinal = 0; ordinal < count; ++ordinal) {
      int device = 0;
      int major = 0;
      int minor = 0;
      if (get_device(&device, ordinal) == 0 &&
          get_attribute(&major, kComputeCapabilityMajor, device) == 0 &&
          get_attribute(&minor, kComputeCapabilityMinor, device) == 0 &&
          std::pair{major, minor} > std::pair{best_major, best_minor}) {
        best_major = major;
        best_minor = minor;
      }
    }
  }
  FreeLibrary(cuda);
  if (best_major >= 10) return "blackwell";
  if (best_major == 8 && best_minor >= 9) return "ada";
  if (best_major == 8) return "ampere";
  if (best_major == 7 && best_minor >= 5) return "turing";
  return std::nullopt;
}

int model_preference(const std::filesystem::path& path,
                     const std::optional<std::string>& preferred) {
  const auto name = path.filename().string();
  if (preferred && name.find(*preferred) != std::string::npos) return 0;
  if (name.find("blackwell") != std::string::npos) return 1;
  if (name.find("ada") != std::string::npos) return 2;
  if (name.find("ampere") != std::string::npos) return 3;
  if (name.find("turing") != std::string::npos) return 4;
  return 5;
}

void sort_models(std::vector<std::filesystem::path>& models) {
  const auto preferred = preferred_gpu_architecture();
  std::ranges::stable_sort(models, [&](const auto& left, const auto& right) {
    return model_preference(left, preferred) < model_preference(right, preferred);
  });
}

void extract_asset(std::ifstream& input, const Asset& asset,
                   const std::filesystem::path& destination) {
  if (correct_size(destination, asset.size)) return;
  const auto temporary = destination.wstring() + L".tmp." +
                         std::to_wstring(GetCurrentProcessId()) + L"." +
                         std::to_wstring(GetCurrentThreadId());
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("cannot create an NvAFX cache file");
  input.clear();
  input.seekg(static_cast<std::streamoff>(asset.offset));
  std::vector<char> buffer(kCopyBufferSize);
  std::uint64_t remaining = asset.size;
  while (remaining != 0) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    input.read(buffer.data(), chunk);
    if (input.gcount() != chunk) {
      output.close();
      DeleteFileW(temporary.c_str());
      throw std::runtime_error("embedded NvAFX asset is truncated");
    }
    output.write(buffer.data(), chunk);
    if (!output) {
      output.close();
      DeleteFileW(temporary.c_str());
      throw std::runtime_error("failed to write the NvAFX cache");
    }
    remaining -= static_cast<std::uint64_t>(chunk);
  }
  output.close();
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    if (!correct_size(destination, asset.size)) {
      throw std::runtime_error("cannot finalize an NvAFX cache file");
    }
  }
}

}  // namespace

PackagedRuntimePaths PackagedRuntime::prepare(
    const std::filesystem::path& plugin_path) {
  std::scoped_lock lock(g_extract_mutex);
  std::ifstream input(plugin_path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("cannot open EchoNullPlugin.dll");
  const auto file_size = static_cast<std::uint64_t>(input.tellg());
  bundle::Footer footer{};
  const auto assets = read_index(input, file_size, footer);
  const auto directory = cache_root(footer.bundle_id);
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) throw std::runtime_error("cannot create the EchoNull runtime cache");

  PackagedRuntimePaths paths;
  paths.directory = directory;
  for (const auto& asset : assets) {
    const auto destination = directory / utf8_name(asset.name);
    extract_asset(input, asset, destination);
    if (asset.name.starts_with("aec_48k") &&
        asset.name.ends_with(".trtpkg")) {
      paths.aec_models.push_back(destination);
    } else if (asset.name.starts_with("denoiser_48k") &&
               asset.name.ends_with(".trtpkg")) {
      paths.noise_models.push_back(destination);
    }
  }
  sort_models(paths.aec_models);
  sort_models(paths.noise_models);
  if (paths.aec_models.empty()) {
    throw std::runtime_error("embedded NvAFX AEC model is missing");
  }
#if ECHONULL_HAS_NVAFX
  NvafxApi::instance().load(paths.directory);
#endif
  return paths;
}

}  // namespace echonull
