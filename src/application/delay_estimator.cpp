#include "application/delay_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace echonull {
namespace {

constexpr std::size_t kHistoryBins = 3000;
constexpr std::size_t kMinimumBins = 1200;
constexpr std::size_t kEstimateIntervalBins = 500;
constexpr std::size_t kLagsPerAudioBlock = 8;

}  // namespace

DelayEstimator::DelayEstimator(const std::uint32_t sample_rate,
                               const double initial_delay_ms,
                               const double max_delay_ms)
    : samples_per_bin_(std::max<std::uint32_t>(1, sample_rate / 1000)),
      max_lag_bins_(static_cast<std::size_t>(std::ceil(max_delay_ms))),
      smoothed_delay_ms_(initial_delay_ms) {
  estimate_near_.reserve(kHistoryBins);
  estimate_far_.reserve(kHistoryBins);
  far_prefix_sum_.reserve(kHistoryBins + 1);
  far_prefix_power_.reserve(kHistoryBins + 1);
}

void DelayEstimator::add(const std::span<const float> near_end,
                         const std::span<const float> far_end) {
  const auto count = std::min(near_end.size(), far_end.size());
  for (std::size_t i = 0; i < count; ++i) {
    near_energy_in_bin_ += static_cast<double>(near_end[i]) * near_end[i];
    far_energy_in_bin_ += static_cast<double>(far_end[i]) * far_end[i];
    ++samples_in_bin_;
    if (samples_in_bin_ < samples_per_bin_) {
      continue;
    }

    near_envelope_.push_back(static_cast<float>(std::sqrt(near_energy_in_bin_ / samples_in_bin_)));
    far_envelope_.push_back(static_cast<float>(std::sqrt(far_energy_in_bin_ / samples_in_bin_)));
    samples_in_bin_ = 0;
    near_energy_in_bin_ = 0.0;
    far_energy_in_bin_ = 0.0;
    ++bins_since_estimate_;

    while (near_envelope_.size() > kHistoryBins) near_envelope_.pop_front();
    while (far_envelope_.size() > kHistoryBins) far_envelope_.pop_front();
  }

  if (!estimation_in_progress_ && near_envelope_.size() >= kMinimumBins &&
      bins_since_estimate_ >= kEstimateIntervalBins) {
    bins_since_estimate_ = 0;
    begin_estimate();
  }
  if (estimation_in_progress_) advance_estimate();
}

void DelayEstimator::begin_estimate() {
  estimate_near_.assign(near_envelope_.begin(), near_envelope_.end());
  estimate_far_.assign(far_envelope_.begin(), far_envelope_.end());
  if (estimate_near_.size() != estimate_far_.size() ||
      estimate_near_.size() <= max_lag_bins_) {
    return;
  }

  far_prefix_sum_.assign(estimate_far_.size() + 1, 0.0);
  far_prefix_power_.assign(estimate_far_.size() + 1, 0.0);
  for (std::size_t index = 0; index < estimate_far_.size(); ++index) {
    const double sample = estimate_far_[index];
    far_prefix_sum_[index + 1] = far_prefix_sum_[index] + sample;
    far_prefix_power_[index + 1] =
        far_prefix_power_[index] + sample * sample;
  }

  const std::size_t begin = max_lag_bins_;
  const std::size_t count = estimate_near_.size() - begin;
  double near_sum = 0.0;
  double near_power_sum = 0.0;
  for (std::size_t index = begin; index < estimate_near_.size(); ++index) {
    const double sample = estimate_near_[index];
    near_sum += sample;
    near_power_sum += sample * sample;
  }
  estimate_near_mean_ = near_sum / static_cast<double>(count);
  estimate_near_power_ =
      near_power_sum - static_cast<double>(count) * estimate_near_mean_ *
                           estimate_near_mean_;
  next_lag_ = 0;
  best_lag_ = 0;
  best_score_ = -1.0;
  estimation_in_progress_ = true;
}

void DelayEstimator::advance_estimate() {
  const std::size_t begin = max_lag_bins_;
  const std::size_t count = estimate_near_.size() - begin;
  const std::size_t end_lag =
      std::min(max_lag_bins_ + 1, next_lag_ + kLagsPerAudioBlock);
  for (; next_lag_ < end_lag; ++next_lag_) {
    const std::size_t far_begin = begin - next_lag_;
    const std::size_t far_end = estimate_far_.size() - next_lag_;
    const double far_sum =
        far_prefix_sum_[far_end] - far_prefix_sum_[far_begin];
    const double far_square_sum =
        far_prefix_power_[far_end] - far_prefix_power_[far_begin];
    const double far_mean = far_sum / static_cast<double>(count);
    const double far_power =
        far_square_sum - static_cast<double>(count) * far_mean * far_mean;

    double cross_sum = 0.0;
    for (std::size_t index = begin; index < estimate_near_.size(); ++index) {
      cross_sum += estimate_near_[index] * estimate_far_[index - next_lag_];
    }
    const double numerator =
        cross_sum - static_cast<double>(count) * estimate_near_mean_ * far_mean;
    const double denominator = std::sqrt(
        std::max(0.0, estimate_near_power_) * std::max(0.0, far_power));
    const double score =
        denominator > 1.0e-12 ? numerator / denominator : -1.0;
    if (score > best_score_) {
      best_score_ = score;
      best_lag_ = next_lag_;
    }
  }
  if (next_lag_ > max_lag_bins_) finish_estimate();
}

void DelayEstimator::finish_estimate() {
  estimation_in_progress_ = false;
  confidence_ = std::clamp(best_score_, 0.0, 1.0);
  if (confidence_ < 0.35) {
    return;
  }
  const double measured_ms = static_cast<double>(best_lag_);
  smoothed_delay_ms_ = has_estimate_ ? 0.8 * smoothed_delay_ms_ + 0.2 * measured_ms : measured_ms;
  has_estimate_ = true;
}

}  // namespace echonull
