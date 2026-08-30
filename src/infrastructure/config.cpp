#include "infrastructure/config.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace echonull {
namespace {

std::string trim(std::string value) {
  const auto is_space = [](const unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
  return value;
}

bool parse_bool(const std::string& value) {
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on") {
    return true;
  }
  if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off") {
    return false;
  }
  throw std::runtime_error("invalid boolean value: " + value);
}

std::filesystem::path environment_path(const wchar_t* name) {
  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
  if (required == 0) {
    return {};
  }
  std::wstring value(required, L'\0');
  GetEnvironmentVariableW(name, value.data(), required);
  value.resize(required - 1);
  return std::filesystem::path(value);
}

}  // namespace

std::wstring utf8_to_wide(const std::string& input) {
  if (input.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                         static_cast<int>(input.size()), nullptr, 0);
  if (length <= 0) {
    throw std::runtime_error("invalid UTF-8 text");
  }
  std::wstring output(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
                      output.data(), length);
  return output;
}

std::string wide_to_utf8(const std::wstring& input) {
  if (input.empty()) {
    return {};
  }
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                         static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    throw std::runtime_error("invalid UTF-16 text");
  }
  std::string output(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
                      output.data(), length, nullptr, nullptr);
  return output;
}

Config Config::load(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open config: " + path.string());
  }

  std::unordered_map<std::string, std::string> values;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    line = trim(line);
    if (line.empty() || line.front() == '#' || line.front() == ';') {
      continue;
    }
    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      throw std::runtime_error("config line " + std::to_string(line_number) + " is missing '='");
    }
    values[trim(line.substr(0, separator))] = trim(line.substr(separator + 1));
  }

  Config config;
  const auto get = [&values](const char* key) -> const std::string* {
    const auto found = values.find(key);
    return found == values.end() ? nullptr : &found->second;
  };

  if (const auto* value = get("microphone")) config.devices.microphone = utf8_to_wide(*value);
  if (const auto* value = get("reference")) config.devices.reference = utf8_to_wide(*value);
  if (const auto* value = get("output")) config.devices.output = utf8_to_wide(*value);
  if (const auto* value = get("model")) config.model_path = utf8_to_wide(*value);
  if (const auto* value = get("noise_model")) {
    config.noise_model_path = utf8_to_wide(*value);
  }
  if (const auto* value = get("sample_rate")) config.sample_rate = static_cast<std::uint32_t>(std::stoul(*value));
  if (const auto* value = get("delay_ms")) config.delay_ms = std::stod(*value);
  if (const auto* value = get("auto_delay")) config.auto_delay = parse_bool(*value);
  if (const auto* value = get("max_delay_ms")) config.max_delay_ms = std::stod(*value);
  if (const auto* value = get("intensity")) config.intensity = std::stof(*value);
  if (const auto* value = get("diagnostics_interval_ms")) {
    config.diagnostics_interval_ms = static_cast<std::uint32_t>(std::stoul(*value));
  }
  if (const auto* value = get("reconnect_delay_ms")) {
    config.reconnect_delay_ms = static_cast<std::uint32_t>(std::stoul(*value));
  }
  if (const auto* value = get("timeline_capacity_ms")) {
    config.timeline_capacity_ms = static_cast<std::uint32_t>(std::stoul(*value));
  }
  if (const auto* value = get("record_directory")) config.record_directory = utf8_to_wide(*value);

  if (config.sample_rate != kSampleRate) {
    throw std::runtime_error("EchoNull currently requires sample_rate = 48000");
  }
  if (config.delay_ms < 0.0 || config.max_delay_ms < config.delay_ms || config.max_delay_ms > 1000.0) {
    throw std::runtime_error("delay_ms/max_delay_ms are outside the supported range");
  }
  if (config.intensity < 0.0F || config.intensity > 1.0F) {
    throw std::runtime_error("intensity must be between 0.0 and 1.0");
  }
  return config;
}

std::filesystem::path Config::resolve_model_path() const {
  if (!model_path.empty()) {
    return std::filesystem::absolute(model_path);
  }

  const auto root = environment_path(L"AFX_SDK_ROOT");
  if (root.empty()) {
    return {};
  }

  const auto base = root / L"features" / L"nvafxaec" / L"models";
  for (const auto* architecture : {L"blackwell", L"ada", L"ampere", L"turing"}) {
    const auto candidate = base / architecture / L"aec_48k.trtpkg";
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::filesystem::path Config::resolve_noise_model_path() const {
  if (!noise_model_path.empty()) {
    return std::filesystem::absolute(noise_model_path);
  }

  const auto root = environment_path(L"AFX_SDK_ROOT");
  if (root.empty()) return {};

  const auto base = root / L"features" / L"nvafxdenoiser" / L"models";
  for (const auto* architecture : {L"blackwell", L"ada", L"ampere", L"turing"}) {
    const auto candidate = base / architecture / L"denoiser_48k.trtpkg";
    if (std::filesystem::exists(candidate)) return candidate;
  }
  return {};
}

EngineSettings Config::engine_settings() const {
  EngineSettings settings;
  settings.sample_rate = sample_rate;
  settings.initial_delay_ms = delay_ms;
  settings.auto_delay = auto_delay;
  settings.max_delay_ms = max_delay_ms;
  settings.diagnostics_interval_ms = diagnostics_interval_ms;
  settings.timeline_capacity_ms = timeline_capacity_ms;
  settings.record_directory = record_directory;
  return settings;
}

}  // namespace echonull
