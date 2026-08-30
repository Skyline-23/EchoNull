#pragma once

#include <cstdint>
#include <span>

namespace echonull {

class ErleMeter {
 public:
  void add(std::span<const float> before, std::span<const float> after,
           std::span<const float> far_end);
  [[nodiscard]] double erle_db() const;
  [[nodiscard]] std::uint64_t active_samples() const { return active_samples_; }

 private:
  double before_power_ = 0.0;
  double after_power_ = 0.0;
  std::uint64_t active_samples_ = 0;
};

}  // namespace echonull
