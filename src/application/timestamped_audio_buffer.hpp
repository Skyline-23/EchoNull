#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

#include "domain/audio_types.hpp"

namespace echonull {

class TimestampedAudioBuffer {
 public:
  explicit TimestampedAudioBuffer(std::uint32_t capacity_ms,
                                  std::uint32_t sample_rate = kSampleRate);

  void push(std::int64_t start_hns, std::vector<float> samples);
  double read(std::int64_t start_hns, std::span<float> output) const;
  [[nodiscard]] std::int64_t earliest_hns() const;
  [[nodiscard]] std::int64_t latest_hns() const;
  bool wait_until(std::int64_t end_hns, std::uint32_t timeout_ms) const;
  void discard_before(std::int64_t time_hns);
  void clear();

 private:
  struct Chunk {
    std::int64_t start_hns = 0;
    std::vector<float> samples;
  };

  [[nodiscard]] std::int64_t chunk_end_hns(const Chunk& chunk) const;

  const std::uint32_t sample_rate_;
  const std::int64_t capacity_hns_;
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  std::deque<Chunk> chunks_;
};

}  // namespace echonull
