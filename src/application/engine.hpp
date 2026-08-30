#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>

#include "application/ports.hpp"

namespace echonull {

struct ProcessedFrame {
  std::int64_t timestamp_hns = 0;
  double delay_ms = 0.0;
  std::span<const float> near_end;
  std::span<const float> far_end;
  std::span<const float> output;
};

struct EngineHooks {
  std::function<void()> on_started;
  std::function<void(const ProcessedFrame&)> on_frame;
};

class Engine {
 public:
  Engine(EngineSettings settings,
         IAudioCaptureSource& microphone,
         IAudioCaptureSource& reference,
         IAudioSink& output,
         IAecProcessor& aec,
         SnapshotHandler snapshot_handler = {});

  void run(std::atomic<bool>& stop_requested, const EngineHooks& hooks = {});
  [[nodiscard]] EngineSnapshot snapshot() const;

 private:
  void publish(std::string message = {});
  void stop_components() noexcept;

  EngineSettings settings_;
  IAudioCaptureSource& microphone_;
  IAudioCaptureSource& reference_;
  IAudioSink& output_;
  IAecProcessor& aec_;
  SnapshotHandler snapshot_handler_;
  mutable std::mutex snapshot_mutex_;
  EngineSnapshot snapshot_;
};

}  // namespace echonull
