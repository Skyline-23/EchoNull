#include "infrastructure/windows/wasapi_format.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace echonull {
namespace {

bool is_extensible(const WAVEFORMATEX& format) {
  return format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
}

}  // namespace

WasapiFormat::WasapiFormat(const WAVEFORMATEX& format)
    : sample_rate_(format.nSamplesPerSec),
      channels_(format.nChannels),
      block_align_(format.nBlockAlign),
      container_bits_(format.wBitsPerSample),
      valid_bits_(format.wBitsPerSample) {
  if (sample_rate_ == 0 || channels_ == 0 || block_align_ == 0 || container_bits_ == 0) {
    throw std::runtime_error("WASAPI returned an invalid endpoint mix format");
  }
  if (block_align_ % channels_ != 0) {
    throw std::runtime_error("WASAPI mix format has a non-integral channel stride");
  }
  bytes_per_sample_ = static_cast<std::uint16_t>(block_align_ / channels_);

  if (is_extensible(format)) {
    const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
    valid_bits_ = extended.Samples.wValidBitsPerSample == 0
                      ? container_bits_
                      : extended.Samples.wValidBitsPerSample;
    if (IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
      encoding_ = Encoding::ieee_float;
    } else if (IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
      encoding_ = Encoding::signed_pcm;
    } else {
      throw std::runtime_error("unsupported WASAPI extensible sample subtype");
    }
  } else if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    encoding_ = Encoding::ieee_float;
  } else if (format.wFormatTag == WAVE_FORMAT_PCM) {
    encoding_ = Encoding::signed_pcm;
  } else {
    throw std::runtime_error("unsupported WASAPI endpoint sample encoding");
  }

  if (encoding_ == Encoding::ieee_float && container_bits_ != 32 && container_bits_ != 64) {
    throw std::runtime_error("unsupported WASAPI floating-point sample width");
  }
  if (encoding_ == Encoding::signed_pcm &&
      (container_bits_ < 16 || container_bits_ > 32 || valid_bits_ > container_bits_)) {
    throw std::runtime_error("unsupported WASAPI PCM sample width");
  }
  if (bytes_per_sample_ * 8U != container_bits_) {
    throw std::runtime_error("unsupported padded WASAPI sample container");
  }
}

std::string WasapiFormat::description() const {
  std::ostringstream value;
  value << sample_rate_ << " Hz, " << channels_ << " channel";
  if (channels_ != 1) value << 's';
  value << ", " << container_bits_ << "-bit "
        << (encoding_ == Encoding::ieee_float ? "float" : "PCM");
  return value.str();
}

float WasapiFormat::decode_sample(const BYTE* sample) const {
  if (encoding_ == Encoding::ieee_float) {
    if (container_bits_ == 32) {
      float value = 0.0F;
      std::memcpy(&value, sample, sizeof(value));
      return std::isfinite(value) ? value : 0.0F;
    }
    double value = 0.0;
    std::memcpy(&value, sample, sizeof(value));
    return std::isfinite(value) ? static_cast<float>(value) : 0.0F;
  }

  std::uint32_t raw = 0;
  for (std::uint16_t byte = 0; byte < bytes_per_sample_; ++byte) {
    raw |= static_cast<std::uint32_t>(sample[byte]) << (8U * byte);
  }
  if (container_bits_ < 32 && (raw & (1U << (container_bits_ - 1U))) != 0) {
    raw |= std::numeric_limits<std::uint32_t>::max() << container_bits_;
  }
  auto signed_value = static_cast<std::int32_t>(raw);
  signed_value >>= container_bits_ - valid_bits_;
  const double scale = std::ldexp(1.0, static_cast<int>(valid_bits_ - 1U));
  return static_cast<float>(static_cast<double>(signed_value) / scale);
}

void WasapiFormat::decode_mono(const BYTE* input, const std::size_t frame_count,
                               const std::span<float> output) const {
  if (output.size() < frame_count) throw std::invalid_argument("mono decode output is too small");
  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    double sum = 0.0;
    const BYTE* frame_data = input + frame * block_align_;
    for (std::uint16_t channel = 0; channel < channels_; ++channel) {
      sum += decode_sample(frame_data + channel * bytes_per_sample_);
    }
    output[frame] = static_cast<float>(sum / static_cast<double>(channels_));
  }
}

void WasapiFormat::encode_sample(const float value, BYTE* sample) const {
  const float bounded = std::clamp(value, -1.0F, 1.0F);
  if (encoding_ == Encoding::ieee_float) {
    if (container_bits_ == 32) {
      std::memcpy(sample, &bounded, sizeof(bounded));
    } else {
      const double converted = bounded;
      std::memcpy(sample, &converted, sizeof(converted));
    }
    return;
  }

  const auto maximum = static_cast<std::int64_t>((1ULL << (valid_bits_ - 1U)) - 1ULL);
  auto converted = static_cast<std::int64_t>(std::llround(static_cast<double>(bounded) *
                                                         static_cast<double>(maximum)));
  converted <<= container_bits_ - valid_bits_;
  const auto raw = static_cast<std::uint32_t>(converted);
  for (std::uint16_t byte = 0; byte < bytes_per_sample_; ++byte) {
    sample[byte] = static_cast<BYTE>((raw >> (8U * byte)) & 0xFFU);
  }
}

void WasapiFormat::encode_mono(const std::span<const float> input, BYTE* output,
                               const std::size_t frame_count) const {
  if (input.size() < frame_count) throw std::invalid_argument("mono encode input is too small");
  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    BYTE* frame_data = output + frame * block_align_;
    for (std::uint16_t channel = 0; channel < channels_; ++channel) {
      encode_sample(input[frame], frame_data + channel * bytes_per_sample_);
    }
  }
}

}  // namespace echonull
