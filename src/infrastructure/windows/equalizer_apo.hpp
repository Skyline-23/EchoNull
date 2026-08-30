#pragma once

#include <filesystem>
#include <vector>

#include "domain/audio_types.hpp"

namespace echonull {

class EqualizerApoIntegration {
 public:
  [[nodiscard]] static std::filesystem::path config_directory();
  [[nodiscard]] static bool post_mix_enabled(const AudioEndpoint& endpoint);
  [[nodiscard]] static std::vector<AudioEndpoint> enabled_playback_endpoints();

  static void apply_reference_endpoint(const AudioEndpoint& endpoint,
                                       const std::filesystem::path& plugin_path);
};

}  // namespace echonull
