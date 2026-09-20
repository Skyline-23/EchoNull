#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace echonull {

class DelayEstimator {
 public:
  DelayEstimator(std::uint32_t sample_rate, double initial_delay_ms, double max_delay_ms);

  void add(std::span<const float> near_end, std::span<const float> far_end);
  [[nodiscard]] double delay_ms() const { return smoothed_delay_ms_; }
  [[nodiscard]] double confidence() const { return confidence_; }
  [[nodiscard]] bool has_estimate() const { return has_estimate_; }

 private:
  void begin_estimate();
  void advance_estimate();
  void finish_estimate();

  std::uint32_t samples_per_bin_;
  std::size_t max_lag_bins_;
  std::deque<float> near_envelope_;
  std::deque<float> far_envelope_;
  std::size_t samples_in_bin_ = 0;
  double near_energy_in_bin_ = 0.0;
  double far_energy_in_bin_ = 0.0;
  std::size_t bins_since_estimate_ = 0;
  double smoothed_delay_ms_;
  double confidence_ = 0.0;
  bool has_estimate_ = false;
  bool estimation_in_progress_ = false;
  std::size_t next_lag_ = 0;
  std::size_t best_lag_ = 0;
  double best_score_ = -1.0;
  double estimate_near_mean_ = 0.0;
  double estimate_near_power_ = 0.0;
  std::vector<double> estimate_near_;
  std::vector<double> estimate_far_;
  std::vector<double> far_prefix_sum_;
  std::vector<double> far_prefix_power_;
};

}  // namespace echonull
