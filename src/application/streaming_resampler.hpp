#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace echonull {

struct ResampledAudio {
  std::vector<float> samples;
  double first_input_frame = 0.0;
};

// Stateful band-limited mono resampler for real-time endpoint adapters. Input
// positions are measured from the first frame received after reset().
class StreamingResampler {
 public:
  StreamingResampler(std::uint32_t input_sample_rate,
                     std::uint32_t output_sample_rate,
                     std::size_t half_filter_width = 16);

  [[nodiscard]] ResampledAudio push(std::span<const float> input);
  void reset();

  [[nodiscard]] std::uint32_t input_sample_rate() const { return input_sample_rate_; }
  [[nodiscard]] std::uint32_t output_sample_rate() const { return output_sample_rate_; }
  [[nodiscard]] std::uint64_t input_frames_received() const { return input_frames_received_; }

 private:
  [[nodiscard]] float interpolate(double position) const;
  void discard_consumed_input();

  std::uint32_t input_sample_rate_ = 0;
  std::uint32_t output_sample_rate_ = 0;
  std::size_t half_filter_width_ = 0;
  double input_frames_per_output_frame_ = 1.0;
  double cutoff_ = 1.0;
  std::vector<float> buffer_;
  std::int64_t buffer_start_frame_ = 0;
  double next_input_position_ = 0.0;
  std::uint64_t input_frames_received_ = 0;
};

}  // namespace echonull
