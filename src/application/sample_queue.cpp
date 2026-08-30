#include "application/sample_queue.hpp"

#include <algorithm>

namespace echonull {

SampleQueue::SampleQueue(const std::size_t capacity_samples)
    : capacity_samples_(std::max<std::size_t>(1, capacity_samples)) {}

std::size_t SampleQueue::push(const std::span<const float> samples) {
  std::scoped_lock lock(mutex_);
  std::size_t dropped = 0;
  if (samples.size() >= capacity_samples_) {
    dropped = samples_.size() + samples.size() - capacity_samples_;
    samples_.clear();
    samples_.insert(samples_.end(), samples.end() - static_cast<std::ptrdiff_t>(capacity_samples_), samples.end());
    return dropped;
  }
  while (samples_.size() + samples.size() > capacity_samples_) {
    samples_.pop_front();
    ++dropped;
  }
  samples_.insert(samples_.end(), samples.begin(), samples.end());
  return dropped;
}

std::size_t SampleQueue::pop(const std::span<float> output) {
  std::scoped_lock lock(mutex_);
  const std::size_t count = std::min(output.size(), samples_.size());
  for (std::size_t i = 0; i < count; ++i) {
    output[i] = samples_.front();
    samples_.pop_front();
  }
  std::fill(output.begin() + static_cast<std::ptrdiff_t>(count), output.end(), 0.0F);
  return count;
}

std::size_t SampleQueue::size() const {
  std::scoped_lock lock(mutex_);
  return samples_.size();
}

void SampleQueue::clear() {
  std::scoped_lock lock(mutex_);
  samples_.clear();
}

}  // namespace echonull
