#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>

#include "application/ports.hpp"
#include "application/sample_queue.hpp"
#include "infrastructure/windows/wasapi_common.hpp"

namespace echonull {

class WasapiRenderer final : public IAudioSink {
 public:
  WasapiRenderer(std::wstring selector,
                 std::uint32_t sample_rate = kSampleRate,
                 std::uint32_t queue_capacity_ms = 2000);
  ~WasapiRenderer() override;

  WasapiRenderer(const WasapiRenderer&) = delete;
  WasapiRenderer& operator=(const WasapiRenderer&) = delete;

  void start() override;
  void stop() override;
  std::size_t push(std::span<const float> samples) override;
  [[nodiscard]] bool wait_ready(std::uint32_t timeout_ms) const override;
  [[nodiscard]] StreamStatus status() const override;
  [[nodiscard]] std::size_t queued_samples() const override;

 private:
  void run();

  std::wstring selector_;
  std::uint32_t sample_rate_ = kSampleRate;
  SampleQueue queue_;
  std::thread thread_;
  std::atomic<bool> stop_requested_{false};
  WorkerState state_;

  mutable std::mutex status_mutex_;
  AudioEndpoint endpoint_;
  std::atomic<std::uint64_t> packets_{0};
  std::atomic<std::uint64_t> frames_{0};
  std::atomic<std::uint64_t> underruns_{0};
  std::atomic<std::uint64_t> overruns_{0};
};

}  // namespace echonull

