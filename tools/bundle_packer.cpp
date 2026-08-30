#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "infrastructure/bundle_format.hpp"

namespace {

struct Asset {
  std::string name;
  std::filesystem::path path;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

constexpr std::size_t kCopyBufferSize = 4U * 1024U * 1024U;

void copy_bytes(std::ifstream& input, std::ofstream& output,
                std::uint64_t count) {
  std::vector<char> buffer(kCopyBufferSize);
  while (count != 0) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<std::uint64_t>(count, buffer.size()));
    input.read(buffer.data(), chunk);
    if (input.gcount() != chunk) throw std::runtime_error("unexpected end of input");
    output.write(buffer.data(), chunk);
    if (!output) throw std::runtime_error("failed to write bundle");
    count -= static_cast<std::uint64_t>(chunk);
  }
}

std::uint64_t fnv1a(std::uint64_t value, const void* data,
                    const std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t index = 0; index < size; ++index) {
    value ^= bytes[index];
    value *= 1099511628211ULL;
  }
  return value;
}

bool valid_asset_name(const std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos &&
         name.find('\\') == std::string_view::npos &&
         name.find(':') == std::string_view::npos;
}

std::uint64_t unpacked_size(std::ifstream& input, const std::uint64_t size) {
  if (size < sizeof(echonull::bundle::Footer)) return size;
  echonull::bundle::Footer footer{};
  input.seekg(static_cast<std::streamoff>(size - sizeof(footer)));
  input.read(reinterpret_cast<char*>(&footer), sizeof(footer));
  input.clear();
  if (footer.magic == echonull::bundle::kMagic &&
      footer.version == echonull::bundle::kVersion &&
      footer.original_size <= size - sizeof(footer)) {
    return footer.original_size;
  }
  return size;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
  try {
    if (argc < 4 || (argc - 2) % 2 != 0) {
      throw std::runtime_error(
          "usage: bundle_packer <plugin.dll> <asset-name> <asset-path> [...]");
    }

    const std::filesystem::path plugin = argv[1];
    std::vector<Asset> assets;
    for (int index = 2; index < argc; index += 2) {
      const std::filesystem::path name_path = argv[index];
      const auto name = name_path.u8string();
      const std::string utf8_name(reinterpret_cast<const char*>(name.data()), name.size());
      if (!valid_asset_name(utf8_name)) {
        throw std::runtime_error("bundle asset names must be plain filenames");
      }
      const std::filesystem::path path = argv[index + 1];
      if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("bundle asset does not exist: " + path.string());
      }
      assets.push_back(Asset{utf8_name, path});
    }

    std::ifstream source(plugin, std::ios::binary | std::ios::ate);
    if (!source) throw std::runtime_error("cannot open plug-in for packing");
    const auto physical_size = static_cast<std::uint64_t>(source.tellg());
    const auto original_size = unpacked_size(source, physical_size);
    source.seekg(0);

    const auto temporary = plugin.wstring() + L".bundle.tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create packed plug-in");
    copy_bytes(source, output, original_size);
    source.close();

    std::uint64_t bundle_id = 14695981039346656037ULL;
    for (auto& asset : assets) {
      asset.offset = static_cast<std::uint64_t>(output.tellp());
      asset.size = std::filesystem::file_size(asset.path);
      std::ifstream input(asset.path, std::ios::binary);
      if (!input) throw std::runtime_error("cannot open bundle asset: " + asset.path.string());
      copy_bytes(input, output, asset.size);
      bundle_id = fnv1a(bundle_id, asset.name.data(), asset.name.size());
      bundle_id = fnv1a(bundle_id, &asset.size, sizeof(asset.size));
      const auto timestamp = std::filesystem::last_write_time(asset.path).time_since_epoch().count();
      bundle_id = fnv1a(bundle_id, &timestamp, sizeof(timestamp));
    }

    const auto index_offset = static_cast<std::uint64_t>(output.tellp());
    for (const auto& asset : assets) {
      const echonull::bundle::Entry entry{
          static_cast<std::uint32_t>(asset.name.size()), 0, asset.offset, asset.size};
      output.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
      output.write(asset.name.data(), static_cast<std::streamsize>(asset.name.size()));
    }
    const auto index_end = static_cast<std::uint64_t>(output.tellp());
    const echonull::bundle::Footer footer{
        echonull::bundle::kMagic, echonull::bundle::kVersion,
        static_cast<std::uint32_t>(assets.size()), original_size, index_offset,
        index_end - index_offset, bundle_id};
    output.write(reinterpret_cast<const char*>(&footer), sizeof(footer));
    output.close();
    if (!output) throw std::runtime_error("failed to finalize packed plug-in");

    if (!MoveFileExW(temporary.c_str(), plugin.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      DeleteFileW(temporary.c_str());
      throw std::runtime_error("cannot replace plug-in with packed output");
    }
    std::wcout << L"Packed " << assets.size() << L" assets into " << plugin << L'\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "EchoNull bundle error: " << error.what() << '\n';
    return 1;
  }
}
