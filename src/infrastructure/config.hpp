#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "domain/audio_types.hpp"

namespace echonull {

struct Config {
  std::filesystem::path model_path;
  std::filesystem::path noise_model_path;
  std::uint32_t sample_rate = kSampleRate;
  double delay_ms = 40.0;
  bool auto_delay = true;
  double max_delay_ms = 250.0;
  std::uint32_t timeline_capacity_ms = 4000;

  static Config load(const std::filesystem::path& path);
  [[nodiscard]] std::filesystem::path resolve_model_path() const;
  [[nodiscard]] std::filesystem::path resolve_noise_model_path() const;
};

std::wstring utf8_to_wide(const std::string& input);
std::string wide_to_utf8(const std::wstring& input);

}  // namespace echonull
