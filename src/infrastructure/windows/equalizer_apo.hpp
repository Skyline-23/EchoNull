#pragma once

#include <vector>

#include "domain/audio_types.hpp"

namespace echonull {

class EqualizerApoIntegration {
 public:
  [[nodiscard]] static bool post_mix_enabled(const AudioEndpoint& endpoint);
  [[nodiscard]] static std::vector<AudioEndpoint> enabled_playback_endpoints();
  [[nodiscard]] static bool capture_stage_guard_present();
};

}  // namespace echonull
