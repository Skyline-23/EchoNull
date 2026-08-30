#include "application/timestamped_audio_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace echonull {

TimestampedAudioBuffer::TimestampedAudioBuffer(const std::uint32_t capacity_ms,
                                               const std::uint32_t sample_rate)
    : sample_rate_(sample_rate),
      capacity_hns_(static_cast<std::int64_t>(capacity_ms) * 10'000) {}

std::int64_t TimestampedAudioBuffer::chunk_end_hns(const Chunk& chunk) const {
  return chunk.start_hns + static_cast<std::int64_t>(
      std::llround(static_cast<double>(chunk.samples.size()) *
                   static_cast<double>(kHundredNanosecondsPerSecond) /
                   static_cast<double>(sample_rate_)));
}

void TimestampedAudioBuffer::push(const std::int64_t start_hns,
                                  const std::span<const float> samples) {
  if (samples.empty()) {
    return;
  }
  {
    std::scoped_lock lock(mutex_);
    if (!chunks_.empty() && start_hns < chunks_.back().start_hns) {
      chunks_.clear();
    }
    chunks_.push_back(Chunk{start_hns, std::vector<float>(samples.begin(), samples.end())});
    const std::int64_t cutoff = chunk_end_hns(chunks_.back()) - capacity_hns_;
    while (!chunks_.empty() && chunk_end_hns(chunks_.front()) < cutoff) {
      chunks_.pop_front();
    }
  }
  condition_.notify_all();
}

double TimestampedAudioBuffer::read(const std::int64_t start_hns,
                                    const std::span<float> output) const {
  std::scoped_lock lock(mutex_);
  std::fill(output.begin(), output.end(), 0.0F);
  if (chunks_.empty() || output.empty()) {
    return 0.0;
  }

  std::size_t covered = 0;
  std::size_t chunk_index = 0;
  for (std::size_t i = 0; i < output.size(); ++i) {
    const auto time_hns = start_hns + static_cast<std::int64_t>(std::llround(
        static_cast<double>(i) * static_cast<double>(kHundredNanosecondsPerSecond) /
        static_cast<double>(sample_rate_)));

    while (chunk_index < chunks_.size() && chunk_end_hns(chunks_[chunk_index]) <= time_hns) {
      ++chunk_index;
    }
    if (chunk_index >= chunks_.size()) {
      break;
    }
    const auto& chunk = chunks_[chunk_index];
    if (time_hns < chunk.start_hns) {
      continue;
    }

    const double position = static_cast<double>(time_hns - chunk.start_hns) *
                            static_cast<double>(sample_rate_) /
                            static_cast<double>(kHundredNanosecondsPerSecond);
    const auto index = static_cast<std::size_t>(std::floor(position));
    if (index >= chunk.samples.size()) {
      continue;
    }
    const auto next = std::min(index + 1, chunk.samples.size() - 1);
    const float fraction = static_cast<float>(position - static_cast<double>(index));
    output[i] = chunk.samples[index] + (chunk.samples[next] - chunk.samples[index]) * fraction;
    ++covered;
  }
  return static_cast<double>(covered) / static_cast<double>(output.size());
}

std::int64_t TimestampedAudioBuffer::earliest_hns() const {
  std::scoped_lock lock(mutex_);
  return chunks_.empty() ? 0 : chunks_.front().start_hns;
}

std::int64_t TimestampedAudioBuffer::latest_hns() const {
  std::scoped_lock lock(mutex_);
  return chunks_.empty() ? 0 : chunk_end_hns(chunks_.back());
}

bool TimestampedAudioBuffer::wait_until(const std::int64_t end_hns,
                                        const std::uint32_t timeout_ms) const {
  std::unique_lock lock(mutex_);
  return condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, end_hns] {
    return !chunks_.empty() && chunk_end_hns(chunks_.back()) >= end_hns;
  });
}

void TimestampedAudioBuffer::discard_before(const std::int64_t time_hns) {
  std::scoped_lock lock(mutex_);
  while (!chunks_.empty() && chunk_end_hns(chunks_.front()) < time_hns) {
    chunks_.pop_front();
  }
}

void TimestampedAudioBuffer::clear() {
  std::scoped_lock lock(mutex_);
  chunks_.clear();
}

}  // namespace echonull
