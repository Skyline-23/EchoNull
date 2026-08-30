#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <span>

namespace echonull {

class SampleQueue {
 public:
  explicit SampleQueue(std::size_t capacity_samples);

  std::size_t push(std::span<const float> samples);
  std::size_t pop(std::span<float> output);
  [[nodiscard]] std::size_t size() const;
  void clear();

 private:
  const std::size_t capacity_samples_;
  mutable std::mutex mutex_;
  std::deque<float> samples_;
};

}  // namespace echonull
