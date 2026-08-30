#pragma once

#include <cstdint>
#include <string>

namespace echonull {

inline constexpr std::uint32_t kSampleRate = 48000;
inline constexpr std::int64_t kHundredNanosecondsPerSecond = 10'000'000;

enum class AudioFlow { capture, render };

struct AudioEndpoint {
  AudioFlow flow = AudioFlow::capture;
  std::wstring id;
  std::wstring apo_guid;
  std::wstring name;
  bool is_default = false;
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

}  // namespace echonull
