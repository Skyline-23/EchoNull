#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

namespace echonull {

// A missed GPU deadline must never expose the unprocessed microphone. This is
// failure containment, not a substitute for completing inference on time.
// Called only when the output FIFO actually needs the frame, not at submission.
class ProtectedOutput {
 public:
  void reset() noexcept { last_ = 0.0F; unavailable_ = true; }

  void render(std::span<const float> unprocessed,
              std::span<const float> processed, bool requires_processing,
              bool complete, std::span<float> output) noexcept {
    constexpr std::size_t kFadeSamples = 240;  // 5 ms at 48 kHz.
    const bool missing = requires_processing && !complete;
    const auto source = requires_processing ? processed : unprocessed;
    const float previous = last_;
    for (std::size_t i = 0; i < output.size(); ++i) {
      const float mix = std::min(1.0F, static_cast<float>(i + 1) / kFadeSamples);
      if (missing) {
        output[i] = previous * (1.0F - mix);
      } else {
        const float value = i < source.size() ? source[i] : 0.0F;
        output[i] = unavailable_ && requires_processing ? value * mix : value;
      }
    }
    if (!output.empty()) last_ = output.back();
    unavailable_ = missing;
  }

 private:
  float last_ = 0.0F;
  bool unavailable_ = true;
};
}  // namespace echonull
