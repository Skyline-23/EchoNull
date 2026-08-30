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

}  // namespace

DelayEstimator::DelayEstimator(const std::uint32_t sample_rate,
                               const double initial_delay_ms,
                               const double max_delay_ms)
    : samples_per_bin_(std::max<std::uint32_t>(1, sample_rate / 1000)),
      max_lag_bins_(static_cast<std::size_t>(std::ceil(max_delay_ms))),
      smoothed_delay_ms_(initial_delay_ms) {}

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

  if (near_envelope_.size() >= kMinimumBins && bins_since_estimate_ >= kEstimateIntervalBins) {
    bins_since_estimate_ = 0;
    estimate();
  }
}

void DelayEstimator::estimate() {
  const std::vector<double> near_values(near_envelope_.begin(), near_envelope_.end());
  const std::vector<double> far_values(far_envelope_.begin(), far_envelope_.end());
  if (near_values.size() != far_values.size() || near_values.size() <= max_lag_bins_) {
    return;
  }

  double best_score = -1.0;
  std::size_t best_lag = 0;
  for (std::size_t lag = 0; lag <= max_lag_bins_; ++lag) {
    const std::size_t begin = max_lag_bins_;
    const std::size_t count = near_values.size() - begin;
    double near_mean = 0.0;
    double far_mean = 0.0;
    for (std::size_t i = begin; i < near_values.size(); ++i) {
      near_mean += near_values[i];
      far_mean += far_values[i - lag];
    }
    near_mean /= static_cast<double>(count);
    far_mean /= static_cast<double>(count);

    double numerator = 0.0;
    double near_power = 0.0;
    double far_power = 0.0;
    for (std::size_t i = begin; i < near_values.size(); ++i) {
      const double near_centered = near_values[i] - near_mean;
      const double far_centered = far_values[i - lag] - far_mean;
      numerator += near_centered * far_centered;
      near_power += near_centered * near_centered;
      far_power += far_centered * far_centered;
    }
    const double denominator = std::sqrt(near_power * far_power);
    const double score = denominator > 1e-12 ? numerator / denominator : -1.0;
    if (score > best_score) {
      best_score = score;
      best_lag = lag;
    }
  }

  confidence_ = std::clamp(best_score, 0.0, 1.0);
  if (confidence_ < 0.35) {
    return;
  }
  const double measured_ms = static_cast<double>(best_lag);
  smoothed_delay_ms_ = has_estimate_ ? 0.8 * smoothed_delay_ms_ + 0.2 * measured_ms : measured_ms;
  has_estimate_ = true;
}

}  // namespace echonull
