#include "application/erle_meter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace echonull {

void ErleMeter::add(const std::span<const float> before,
                    const std::span<const float> after,
                    const std::span<const float> far_end) {
  const auto count = std::min({before.size(), after.size(), far_end.size()});
  if (count == 0) {
    return;
  }
  double far_power = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    far_power += static_cast<double>(far_end[i]) * far_end[i];
  }
  const double far_rms = std::sqrt(far_power / static_cast<double>(count));
  if (far_rms < 0.003) {
    return;
  }
  for (std::size_t i = 0; i < count; ++i) {
    before_power_ += static_cast<double>(before[i]) * before[i];
    after_power_ += static_cast<double>(after[i]) * after[i];
  }
  active_samples_ += count;
}

double ErleMeter::erle_db() const {
  if (active_samples_ == 0 || after_power_ <= 1e-20 || before_power_ <= 1e-20) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return 10.0 * std::log10(before_power_ / after_power_);
}

}  // namespace echonull
