#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "domain/audio_types.hpp"

namespace echonull {

using CaptureHandler = std::function<void(const CapturePacket&)>;

class IAudioCaptureSource {
 public:
  virtual ~IAudioCaptureSource() = default;
  virtual void start(CaptureHandler handler) = 0;
  virtual void stop() = 0;
  [[nodiscard]] virtual bool wait_ready(std::uint32_t timeout_ms) const = 0;
  [[nodiscard]] virtual StreamStatus status() const = 0;
};

class IAudioSink {
 public:
  virtual ~IAudioSink() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual std::size_t push(std::span<const float> samples) = 0;
  [[nodiscard]] virtual bool wait_ready(std::uint32_t timeout_ms) const = 0;
  [[nodiscard]] virtual StreamStatus status() const = 0;
  [[nodiscard]] virtual std::size_t queued_samples() const = 0;
};

class IAecProcessor {
 public:
  virtual ~IAecProcessor() = default;
  virtual void initialize() = 0;
  virtual void reset() = 0;
  virtual void process(std::span<const float> near_end,
                       std::span<const float> far_end,
                       std::span<float> output) = 0;
  [[nodiscard]] virtual AecStatus status() const = 0;
};

class IDeviceCatalog {
 public:
  virtual ~IDeviceCatalog() = default;
  [[nodiscard]] virtual std::vector<AudioEndpoint> list(AudioFlow flow) const = 0;
};

using SnapshotHandler = std::function<void(const EngineSnapshot&)>;

}  // namespace echonull

