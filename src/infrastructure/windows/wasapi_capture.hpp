#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "application/ports.hpp"
#include "infrastructure/windows/wasapi_common.hpp"

namespace echonull {

class WasapiCapture final : public IAudioCaptureSource {
 public:
  WasapiCapture(std::wstring selector, bool loopback,
                std::uint32_t sample_rate = kSampleRate);
  ~WasapiCapture() override;

  WasapiCapture(const WasapiCapture&) = delete;
  WasapiCapture& operator=(const WasapiCapture&) = delete;

  void start(CaptureHandler handler) override;
  void stop() override;
  [[nodiscard]] bool wait_ready(std::uint32_t timeout_ms) const override;
  [[nodiscard]] StreamStatus status() const override;

 private:
  void run();
  static std::int64_t qpc_now_hns();

  std::wstring selector_;
  bool loopback_ = false;
  std::uint32_t sample_rate_ = kSampleRate;
  CaptureHandler handler_;
  std::thread thread_;
  std::atomic<bool> stop_requested_{false};
  WorkerState state_;

  mutable std::mutex status_mutex_;
  AudioEndpoint endpoint_;
  std::atomic<std::uint64_t> packets_{0};
  std::atomic<std::uint64_t> frames_{0};
  std::atomic<std::uint64_t> discontinuities_{0};
};

}  // namespace echonull

