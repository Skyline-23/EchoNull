#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace echonull {

struct WavData {
  std::uint32_t sample_rate = 0;
  std::vector<float> mono_samples;
};

WavData read_wav_mono(const std::filesystem::path& path);

class WavWriter {
 public:
  WavWriter() = default;
  WavWriter(const std::filesystem::path& path, std::uint32_t sample_rate);
  ~WavWriter();

  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;
  WavWriter(WavWriter&& other) noexcept;
  WavWriter& operator=(WavWriter&& other) noexcept;

  void write(std::span<const float> samples);
  void close();
  [[nodiscard]] bool is_open() const { return stream_.is_open(); }

 private:
  std::ofstream stream_;
  std::uint32_t sample_rate_ = 0;
  std::uint64_t sample_count_ = 0;
};

}  // namespace echonull
