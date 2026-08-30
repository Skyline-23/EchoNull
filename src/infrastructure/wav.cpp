#include "infrastructure/wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace echonull {
namespace {

template <typename T>
T read_scalar(std::istream& input) {
  T value{};
  input.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!input) {
    throw std::runtime_error("unexpected end of WAV file");
  }
  return value;
}

void write_u16(std::ostream& output, const std::uint16_t value) {
  output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_u32(std::ostream& output, const std::uint32_t value) {
  output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool id_equals(const std::array<char, 4>& id, const char* expected) {
  return std::memcmp(id.data(), expected, id.size()) == 0;
}

}  // namespace

WavData read_wav_mono(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open WAV file: " + path.string());
  }

  std::array<char, 4> id{};
  input.read(id.data(), id.size());
  if (!id_equals(id, "RIFF")) throw std::runtime_error("WAV file is not RIFF");
  static_cast<void>(read_scalar<std::uint32_t>(input));
  input.read(id.data(), id.size());
  if (!id_equals(id, "WAVE")) throw std::runtime_error("RIFF file is not WAVE");

  std::uint16_t format_tag = 0;
  std::uint16_t channels = 0;
  std::uint32_t sample_rate = 0;
  std::uint16_t bits_per_sample = 0;
  std::vector<std::byte> raw_data;

  while (input.read(id.data(), id.size())) {
    const auto chunk_size = read_scalar<std::uint32_t>(input);
    const auto chunk_start = input.tellg();
    if (id_equals(id, "fmt ")) {
      format_tag = read_scalar<std::uint16_t>(input);
      channels = read_scalar<std::uint16_t>(input);
      sample_rate = read_scalar<std::uint32_t>(input);
      static_cast<void>(read_scalar<std::uint32_t>(input));
      static_cast<void>(read_scalar<std::uint16_t>(input));
      bits_per_sample = read_scalar<std::uint16_t>(input);
      if (format_tag == 0xFFFE && chunk_size >= 40) {
        static_cast<void>(read_scalar<std::uint16_t>(input));
        static_cast<void>(read_scalar<std::uint16_t>(input));
        static_cast<void>(read_scalar<std::uint32_t>(input));
        format_tag = read_scalar<std::uint16_t>(input);
      }
    } else if (id_equals(id, "data")) {
      raw_data.resize(chunk_size);
      input.read(reinterpret_cast<char*>(raw_data.data()), static_cast<std::streamsize>(chunk_size));
      if (!input) throw std::runtime_error("truncated WAV data chunk");
    }
    input.seekg(chunk_start + static_cast<std::streamoff>(chunk_size + (chunk_size & 1U)));
  }

  if (channels == 0 || sample_rate == 0 || raw_data.empty()) {
    throw std::runtime_error("WAV file is missing format or audio data");
  }
  const std::size_t bytes_per_sample = bits_per_sample / 8;
  if (bytes_per_sample == 0) throw std::runtime_error("unsupported WAV bit depth");
  const std::size_t frame_bytes = bytes_per_sample * channels;
  const std::size_t frame_count = raw_data.size() / frame_bytes;
  WavData result{sample_rate, std::vector<float>(frame_count, 0.0F)};

  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    double sum = 0.0;
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const auto* bytes = raw_data.data() + frame * frame_bytes + channel * bytes_per_sample;
      float sample = 0.0F;
      if (format_tag == 3 && bits_per_sample == 32) {
        std::memcpy(&sample, bytes, sizeof(sample));
      } else if (format_tag == 1 && bits_per_sample == 16) {
        std::int16_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        sample = static_cast<float>(value) / 32768.0F;
      } else if (format_tag == 1 && bits_per_sample == 24) {
        const auto* raw = reinterpret_cast<const unsigned char*>(bytes);
        std::int32_t value = static_cast<std::int32_t>(raw[0]) |
                             (static_cast<std::int32_t>(raw[1]) << 8) |
                             (static_cast<std::int32_t>(raw[2]) << 16);
        if ((value & 0x00800000) != 0) value |= static_cast<std::int32_t>(0xFF000000);
        sample = static_cast<float>(value) / 8388608.0F;
      } else if (format_tag == 1 && bits_per_sample == 32) {
        std::int32_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        sample = static_cast<float>(static_cast<double>(value) / 2147483648.0);
      } else {
        throw std::runtime_error("WAV must be PCM16/24/32 or float32");
      }
      sum += sample;
    }
    result.mono_samples[frame] = static_cast<float>(sum / channels);
  }
  return result;
}

WavWriter::WavWriter(const std::filesystem::path& path, const std::uint32_t sample_rate)
    : stream_(path, std::ios::binary), sample_rate_(sample_rate) {
  if (!stream_) {
    throw std::runtime_error("cannot create WAV file: " + path.string());
  }
  std::array<char, 44> placeholder{};
  stream_.write(placeholder.data(), placeholder.size());
}

WavWriter::~WavWriter() {
  try {
    close();
  } catch (...) {
  }
}

WavWriter::WavWriter(WavWriter&& other) noexcept
    : stream_(std::move(other.stream_)),
      sample_rate_(other.sample_rate_),
      sample_count_(other.sample_count_) {
  other.sample_count_ = 0;
}

WavWriter& WavWriter::operator=(WavWriter&& other) noexcept {
  if (this != &other) {
    try { close(); } catch (...) {}
    stream_ = std::move(other.stream_);
    sample_rate_ = other.sample_rate_;
    sample_count_ = other.sample_count_;
    other.sample_count_ = 0;
  }
  return *this;
}

void WavWriter::write(const std::span<const float> samples) {
  if (!stream_) return;
  stream_.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(samples.size_bytes()));
  sample_count_ += samples.size();
}

void WavWriter::close() {
  if (!stream_.is_open()) return;
  const std::uint64_t data_bytes_64 = sample_count_ * sizeof(float);
  if (data_bytes_64 > std::numeric_limits<std::uint32_t>::max() - 36U) {
    throw std::runtime_error("WAV recording exceeded the RIFF size limit");
  }
  const auto data_bytes = static_cast<std::uint32_t>(data_bytes_64);
  stream_.seekp(0);
  stream_.write("RIFF", 4);
  write_u32(stream_, 36U + data_bytes);
  stream_.write("WAVE", 4);
  stream_.write("fmt ", 4);
  write_u32(stream_, 16);
  write_u16(stream_, 3);
  write_u16(stream_, 1);
  write_u32(stream_, sample_rate_);
  write_u32(stream_, sample_rate_ * sizeof(float));
  write_u16(stream_, sizeof(float));
  write_u16(stream_, 32);
  stream_.write("data", 4);
  write_u32(stream_, data_bytes);
  stream_.close();
}

}  // namespace echonull
