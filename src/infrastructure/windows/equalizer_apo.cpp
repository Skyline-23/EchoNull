#include "infrastructure/windows/equalizer_apo.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "infrastructure/equalizer_apo_config.hpp"
#include "infrastructure/windows/device_manager.hpp"

namespace echonull {
namespace {

constexpr wchar_t kEqualizerApoPostMixGuid[] =
    L"{EC1CC9CE-FAED-4822-828A-82A81A6F018F}";
constexpr wchar_t kFxGuidPropertySet[] =
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}";
constexpr wchar_t kDisableEnhancementsValue[] =
    L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";

std::wstring read_registry_string(HKEY root, const wchar_t* path,
                                  const wchar_t* name) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) !=
      ERROR_SUCCESS) {
    return {};
  }
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) !=
          ERROR_SUCCESS ||
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
  return value;
}

std::string lowercase_trimmed(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  value = value.substr(first, last - first + 1);
  std::transform(value.begin(), value.end(), value.begin(), [](const char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return value;
}

bool registry_string_equals(HKEY key, const std::wstring& name,
                            const wchar_t* expected) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &bytes) !=
          ERROR_SUCCESS ||
      type != REG_SZ || bytes < sizeof(wchar_t)) {
    return false;
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegQueryValueExW(key, name.c_str(), nullptr, &type,
                      reinterpret_cast<BYTE*>(value.data()), &bytes) !=
      ERROR_SUCCESS) {
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
                          reinterpret_cast<BYTE*>(&value), &bytes) ==
             ERROR_SUCCESS &&
         type == REG_DWORD && value != 0;
}

}  // namespace

bool EqualizerApoIntegration::post_mix_enabled(
    const AudioEndpoint& endpoint) {
  if (endpoint.flow != AudioFlow::render || endpoint.apo_guid.empty()) {
    return false;
  }
  const std::wstring path =
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" +
      endpoint.apo_guid + L"\\FxProperties";
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return false;
  }
  const bool active =
      !enhancements_disabled(key) &&
      (registry_string_equals(
           key, std::wstring(kFxGuidPropertySet) + L",2",
           kEqualizerApoPostMixGuid) ||
       registry_string_equals(
           key, std::wstring(kFxGuidPropertySet) + L",6",
           kEqualizerApoPostMixGuid) ||
       registry_string_equals(
           key, std::wstring(kFxGuidPropertySet) + L",7",
           kEqualizerApoPostMixGuid));
  RegCloseKey(key);
  return active;
}

std::vector<AudioEndpoint>
EqualizerApoIntegration::enabled_playback_endpoints() {
  WasapiDeviceCatalog catalog;
  auto endpoints = catalog.list(AudioFlow::render);
  std::erase_if(endpoints, [](const AudioEndpoint& endpoint) {
    return !post_mix_enabled(endpoint);
  });
  return endpoints;
}

bool EqualizerApoIntegration::capture_stage_guard_present() {
  const auto config_directory = read_registry_string(
      HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", L"ConfigPath");
  if (config_directory.empty()) return false;
  std::ifstream input(std::filesystem::path(config_directory) / L"config.txt",
                      std::ios::binary);
  if (!input) return false;

  return has_capture_scoped_echonull(
      std::string(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>()));
}

}  // namespace echonull
