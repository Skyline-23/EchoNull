#pragma once
#include <cstdint>

namespace echonull {
// Process WDDM scheduling, separate from CPU/MMCSS. Reference-counted across
// plug-in instances and restored after the last worker stops.
class GpuPriority {
 public:
  explicit GpuPriority(bool enabled = true) noexcept;
  ~GpuPriority();
  GpuPriority(const GpuPriority&) = delete;
  GpuPriority& operator=(const GpuPriority&) = delete;
  [[nodiscard]] std::uint32_t status() const noexcept { return status_; }
  [[nodiscard]] std::uint32_t observed_class() const noexcept { return observed_; }
 private:
  bool acquired_ = false;
  std::uint32_t status_ = 0;
  std::uint32_t observed_ = 0;
};
}  // namespace echonull
