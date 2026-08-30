#include "application/streaming_resampler.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace echonull {
namespace {

double sinc(const double value) {
  if (std::abs(value) < 1.0e-12) return 1.0;
  const double radians = std::numbers::pi * value;
  return std::sin(radians) / radians;
}

}  // namespace

StreamingResampler::StreamingResampler(const std::uint32_t input_sample_rate,
                                       const std::uint32_t output_sample_rate,
                                       const std::size_t half_filter_width)
    : input_sample_rate_(input_sample_rate),
      output_sample_rate_(output_sample_rate),
      half_filter_width_(half_filter_width) {
  if (input_sample_rate_ == 0 || output_sample_rate_ == 0) {
    throw std::invalid_argument("resampler sample rates must be non-zero");
  }
  if (half_filter_width_ < 4) {
    throw std::invalid_argument("resampler half-filter width must be at least four");
  }
  input_frames_per_output_frame_ =
      static_cast<double>(input_sample_rate_) / static_cast<double>(output_sample_rate_);
  cutoff_ = std::min(1.0, static_cast<double>(output_sample_rate_) /
                              static_cast<double>(input_sample_rate_)) * 0.94;
  reset();
}

void StreamingResampler::reset() {
  buffer_.assign(half_filter_width_, 0.0F);
  buffer_start_frame_ = -static_cast<std::int64_t>(half_filter_width_);
  next_input_position_ = 0.0;
  input_frames_received_ = 0;
}

ResampledAudio StreamingResampler::push(const std::span<const float> input) {
  buffer_.insert(buffer_.end(), input.begin(), input.end());
  input_frames_received_ += input.size();

  ResampledAudio result;
  result.first_input_frame = next_input_position_;
  const auto last_available_frame =
      buffer_start_frame_ + static_cast<std::int64_t>(buffer_.size()) - 1;
  while (static_cast<std::int64_t>(std::floor(next_input_position_)) +
             static_cast<std::int64_t>(half_filter_width_) <= last_available_frame) {
    result.samples.push_back(interpolate(next_input_position_));
    next_input_position_ += input_frames_per_output_frame_;
  }
  discard_consumed_input();
  return result;
}

float StreamingResampler::interpolate(const double position) const {
  const auto center = static_cast<std::int64_t>(std::floor(position));
  const auto half_width = static_cast<std::int64_t>(half_filter_width_);
  double weighted_sum = 0.0;
  double weight_sum = 0.0;
  for (std::int64_t frame = center - half_width + 1;
       frame <= center + half_width; ++frame) {
    const double distance = position - static_cast<double>(frame);
    const double normalized_distance = distance / static_cast<double>(half_filter_width_);
    if (std::abs(normalized_distance) >= 1.0) continue;
    const double window = 0.5 + 0.5 * std::cos(std::numbers::pi * normalized_distance);
    const double weight = cutoff_ * sinc(cutoff_ * distance) * window;
    const auto index = static_cast<std::size_t>(frame - buffer_start_frame_);
    weighted_sum += static_cast<double>(buffer_[index]) * weight;
    weight_sum += weight;
  }
  if (std::abs(weight_sum) < 1.0e-12) return 0.0F;
  return static_cast<float>(weighted_sum / weight_sum);
}

void StreamingResampler::discard_consumed_input() {
  const auto first_needed = static_cast<std::int64_t>(std::floor(next_input_position_)) -
                            static_cast<std::int64_t>(half_filter_width_) + 1;
  if (first_needed <= buffer_start_frame_) return;
  const auto requested = static_cast<std::size_t>(first_needed - buffer_start_frame_);
  const auto discard = std::min(requested, buffer_.size());
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(discard));
  buffer_start_frame_ += static_cast<std::int64_t>(discard);
}

}  // namespace echonull
