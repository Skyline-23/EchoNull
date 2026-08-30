#pragma once

#include <Audioclient.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace echonull {

class WasapiFormat {
 public:
  explicit WasapiFormat(const WAVEFORMATEX& format);

  [[nodiscard]] std::uint32_t sample_rate() const { return sample_rate_; }
  [[nodiscard]] std::uint16_t channels() const { return channels_; }
  [[nodiscard]] std::uint16_t block_align() const { return block_align_; }
  [[nodiscard]] std::string description() const;

  void decode_mono(const BYTE* input, std::size_t frame_count,
                   std::span<float> output) const;
  void encode_mono(std::span<const float> input, BYTE* output,
                   std::size_t frame_count) const;

 private:
  enum class Encoding { ieee_float, signed_pcm };

  [[nodiscard]] float decode_sample(const BYTE* sample) const;
  void encode_sample(float value, BYTE* sample) const;

  Encoding encoding_ = Encoding::ieee_float;
  std::uint32_t sample_rate_ = 0;
  std::uint16_t channels_ = 0;
  std::uint16_t block_align_ = 0;
  std::uint16_t container_bits_ = 0;
  std::uint16_t valid_bits_ = 0;
  std::uint16_t bytes_per_sample_ = 0;
};

}  // namespace echonull
