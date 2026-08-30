#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace echonull {

inline constexpr std::uint32_t kSampleRate = 48000;
inline constexpr std::int64_t kHundredNanosecondsPerSecond = 10'000'000;

struct DeviceSelection {
  std::wstring microphone = L"default";
  std::wstring reference = L"default";
  std::wstring output = L"CABLE Input";
};

struct StreamCounters {
  std::uint64_t packets = 0;
  std::uint64_t frames = 0;
  std::uint64_t discontinuities = 0;
  std::uint64_t underruns = 0;
  std::uint64_t overruns = 0;
};

enum class AudioFlow { capture, render };

struct AudioEndpoint {
  AudioFlow flow = AudioFlow::capture;
  std::wstring id;
  std::wstring name;
  bool is_default = false;
};

struct CapturePacket {
  std::int64_t timestamp_hns = 0;
  std::span<const float> samples;
  bool discontinuity = false;
};

struct StreamStatus {
  bool ready = false;
  bool failed = false;
  AudioEndpoint endpoint;
  std::uint32_t sample_rate = 0;
  std::uint32_t device_sample_rate = 0;
  std::uint16_t device_channels = 0;
  std::string device_format;
  StreamCounters counters;
  std::string error;
};

struct AecStatus {
  bool ready = false;
  std::uint32_t input_sample_rate = 0;
  std::uint32_t output_sample_rate = 0;
  std::uint32_t input_channels = 0;
  std::uint32_t output_channels = 0;
  std::uint32_t input_frame_samples = 0;
  std::uint32_t output_frame_samples = 0;
  std::string error;
};

struct EngineSettings {
  std::uint32_t sample_rate = kSampleRate;
  double initial_delay_ms = 40.0;
  bool auto_delay = true;
  double max_delay_ms = 250.0;
  std::uint32_t diagnostics_interval_ms = 1000;
  std::uint32_t timeline_capacity_ms = 4000;
  std::filesystem::path record_directory;
};

struct EngineSnapshot {
  bool running = false;
  double delay_ms = 0.0;
  double delay_confidence = 0.0;
  double processing_latency_ms = 0.0;
  double erle_db = 0.0;
  std::uint64_t processed_frames = 0;
  std::uint64_t mic_underruns = 0;
  std::uint64_t reference_underruns = 0;
  std::uint64_t output_overruns = 0;
  StreamStatus microphone;
  StreamStatus reference;
  StreamStatus output;
  AecStatus aec;
  std::string message;
};

}  // namespace echonull
