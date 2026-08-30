#pragma once

#include <Windows.h>

#include <cstdint>
#include <optional>

namespace echonull {

struct TelemetrySnapshot {
  std::int64_t timestamp_hns = 0;
  float output_level = 0.0F;
  std::uint32_t runtime_state = 0;
  std::uint32_t noise_state = 0;
  std::uint32_t aec_error = 0;
  std::uint32_t noise_error = 0;
};

[[nodiscard]] bool is_windows_audio_engine_process();

class TelemetryBusWriter {
 public:
  TelemetryBusWriter() = default;
  ~TelemetryBusWriter();

  TelemetryBusWriter(const TelemetryBusWriter&) = delete;
  TelemetryBusWriter& operator=(const TelemetryBusWriter&) = delete;

  void open();
  void close() noexcept;
  void publish(const TelemetrySnapshot& snapshot) noexcept;

 private:
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
};

class TelemetryBusReader {
 public:
  TelemetryBusReader() = default;
  ~TelemetryBusReader();

  TelemetryBusReader(const TelemetryBusReader&) = delete;
  TelemetryBusReader& operator=(const TelemetryBusReader&) = delete;

  bool open();
  void close() noexcept;
  [[nodiscard]] std::optional<TelemetrySnapshot> read_latest();

 private:
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
};

}  // namespace echonull
