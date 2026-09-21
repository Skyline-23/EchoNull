#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

namespace echonull {

// Smooth a change between a processed frame and its delayed dry counterpart.
// The audio callback owns this object; render() does not allocate or lock.
class OutputTransition {
 public:
  void reset() noexcept {
    previous_wet_ = false;
    previous_sample_ = 0.0F;
  }

  void render(const std::span<const float> dry,
              const std::span<const float> selected, const bool wet,
              const std::span<float> output) noexcept {
    const auto count = std::min({dry.size(), selected.size(), output.size()});
    constexpr std::size_t kFadeSamples = 240;  // 5 ms at 48 kHz.
    if (wet && !previous_wet_) {
      for (std::size_t index = 0; index < count; ++index) {
        const float mix = std::min(1.0F, static_cast<float>(index + 1) /
                                                static_cast<float>(kFadeSamples));
        output[index] = dry[index] + (selected[index] - dry[index]) * mix;
      }
    } else if (!wet && previous_wet_ && count != 0) {
      const float correction = previous_sample_ - dry.front();
      for (std::size_t index = 0; index < count; ++index) {
        const float fade = 1.0F -
                           std::min(1.0F, static_cast<float>(index + 1) /
                                                   static_cast<float>(kFadeSamples));
        output[index] = dry[index] + correction * fade;
      }
    } else {
      std::copy_n(selected.begin(), count, output.begin());
    }
    if (count != 0) previous_sample_ = output[count - 1];
    previous_wet_ = wet;
  }

 private:
  bool previous_wet_ = false;
  float previous_sample_ = 0.0F;
};

}  // namespace echonull
