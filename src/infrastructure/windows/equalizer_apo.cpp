#include "infrastructure/windows/equalizer_apo.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "infrastructure/text_encoding.hpp"
#include "infrastructure/windows/device_manager.hpp"

namespace echonull {
namespace {

constexpr wchar_t kEqualizerApoPostMixGuid[] =
    L"{EC1CC9CE-FAED-4822-828A-82A81A6F018F}";
constexpr wchar_t kFxGuidPropertySet[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}";
constexpr wchar_t kDisableEnhancementsValue[] =
    L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";
constexpr char kManagedBegin[] = "# EchoNull managed reference begin";
constexpr char kManagedEnd[] = "# EchoNull managed reference end";
constexpr wchar_t kManagedFileName[] = L"EchoNull-reference.txt";

std::wstring read_registry_string(HKEY root, const wchar_t* path, const wchar_t* name) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return {};
  }
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
    RegCloseKey(key);
    return {};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  const LONG result = RegQueryValueExW(
      key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) return {};
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  if (type == REG_EXPAND_SZ) {
    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required != 0) {
      std::wstring expanded(required, L'\0');
      ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required);
      while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
      value = std::move(expanded);
    }
  }
  return value;
}

bool registry_string_equals(HKEY key, const std::wstring& name,
                            const wchar_t* expected) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
      type != REG_SZ || bytes < sizeof(wchar_t)) {
    return false;
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegQueryValueExW(key, name.c_str(), nullptr, &type,
                      reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) {
    return false;
  }
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  return _wcsicmp(value.c_str(), expected) == 0;
}

bool enhancements_disabled(HKEY key) {
  DWORD value = 0;
  DWORD type = 0;
  DWORD bytes = sizeof(value);
  return RegQueryValueExW(key, kDisableEnhancementsValue, nullptr, &type,
                          reinterpret_cast<BYTE*>(&value), &bytes) == ERROR_SUCCESS &&
         type == REG_DWORD && value != 0;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void write_file_atomically(const std::filesystem::path& path,
                           const std::string& contents) {
  const auto temporary = path.wstring() + L".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write Equalizer APO configuration");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) throw std::runtime_error("failed while writing Equalizer APO configuration");
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    throw std::runtime_error("cannot replace Equalizer APO configuration");
  }
}

std::string quote_config_path(const std::filesystem::path& path) {
  auto value = wide_to_utf8(std::filesystem::absolute(path).wstring());
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    if (character == '"') escaped += "\"\"";
    else escaped += character;
  }
  return '"' + escaped + '"';
}

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const char character) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  });
  return value;
}

std::string trim_ascii(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string scope_aec_plugin_to_capture(
    const std::string& root, const std::filesystem::path& plugin_path) {
  const std::string plugin_name = lowercase_ascii(
      wide_to_utf8(plugin_path.filename().wstring()));
  if (plugin_name.empty()) return root;

  std::string result;
  result.reserve(root.size() + 32);
  std::string previous_line;
  std::size_t offset = 0;
  while (offset < root.size()) {
    const auto newline = root.find('\n', offset);
    const auto length = newline == std::string::npos
                            ? root.size() - offset
                            : newline - offset + 1;
    const std::string line = root.substr(offset, length);
    const std::string normalized = lowercase_ascii(trim_ascii(line));
    const bool is_plugin_line =
        normalized.starts_with("vstplugin:") &&
        normalized.find(plugin_name) != std::string::npos;
    const bool is_reference_mode =
        normalized.find("mode 0") != std::string::npos;
    if (is_plugin_line && !is_reference_mode &&
        lowercase_ascii(trim_ascii(previous_line)) != "stage: capture") {
      result += "Stage: capture\r\n";
    }
    result += line;
    previous_line = line;
    offset += length;
  }
  return result;
}

}  // namespace

std::filesystem::path EqualizerApoIntegration::config_directory() {
  const auto value = read_registry_string(
      HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", L"ConfigPath");
  if (value.empty()) {
    throw std::runtime_error("Equalizer APO is not installed or ConfigPath is missing");
  }
  const std::filesystem::path path(value);
  if (!std::filesystem::is_directory(path)) {
    throw std::runtime_error("Equalizer APO ConfigPath does not exist");
  }
  return path;
}

bool EqualizerApoIntegration::post_mix_enabled(const AudioEndpoint& endpoint) {
  if (endpoint.flow != AudioFlow::render || endpoint.apo_guid.empty()) return false;
  const std::wstring path =
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" +
      endpoint.apo_guid + L"\\FxProperties";
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return false;
  }
  const bool active = !enhancements_disabled(key) &&
      (registry_string_equals(key, std::wstring(kFxGuidPropertySet) + L",2",
                              kEqualizerApoPostMixGuid) ||
       registry_string_equals(key, std::wstring(kFxGuidPropertySet) + L",6",
                              kEqualizerApoPostMixGuid) ||
       registry_string_equals(key, std::wstring(kFxGuidPropertySet) + L",7",
                              kEqualizerApoPostMixGuid));
  RegCloseKey(key);
  return active;
}

std::vector<AudioEndpoint> EqualizerApoIntegration::enabled_playback_endpoints() {
  WasapiDeviceCatalog catalog;
  auto endpoints = catalog.list(AudioFlow::render);
  std::erase_if(endpoints, [](const AudioEndpoint& endpoint) {
    return !post_mix_enabled(endpoint);
  });
  return endpoints;
}

void EqualizerApoIntegration::apply_reference_endpoint(
    const AudioEndpoint& endpoint, const std::filesystem::path& plugin_path) {
  if (!post_mix_enabled(endpoint)) {
    throw std::runtime_error(
        "the selected playback endpoint does not have Equalizer APO post-mix enabled");
  }
  if (!std::filesystem::is_regular_file(plugin_path)) {
    throw std::runtime_error("the loaded EchoNullPlugin.dll path is not a regular file");
  }

  const auto directory = config_directory();
  const auto managed_path = directory / kManagedFileName;
  std::string managed =
      "# Generated by the EchoNull plug-in editor.\r\n"
      "Device: " + wide_to_utf8(endpoint.apo_guid) + "\r\n"
      "Stage: post-mix\r\n"
      "VSTPlugin: Library " + quote_config_path(plugin_path) + " Mode 0\r\n";
  write_file_atomically(managed_path, managed);

  const auto root_path = directory / L"config.txt";
  std::string root = scope_aec_plugin_to_capture(read_file(root_path), plugin_path);
  const std::string block = std::string(kManagedBegin) + "\r\n" +
                            "Device: all\r\n" +
                            "Include: " + wide_to_utf8(kManagedFileName) + "\r\n" +
                            kManagedEnd + "\r\n";
  const auto begin = root.find(kManagedBegin);
  if (begin == std::string::npos) {
    if (!root.empty() && root.back() != '\n') root += "\r\n";
    root += block;
  } else {
    const auto end_marker = root.find(kManagedEnd, begin);
    if (end_marker == std::string::npos) {
      throw std::runtime_error("Equalizer APO config has an incomplete EchoNull managed block");
    }
    auto end = end_marker + std::char_traits<char>::length(kManagedEnd);
    if (end < root.size() && root[end] == '\r') ++end;
    if (end < root.size() && root[end] == '\n') ++end;
    root.replace(begin, end - begin, block);
  }
  write_file_atomically(root_path, root);
}

}  // namespace echonull
