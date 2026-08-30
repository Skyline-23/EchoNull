#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "application/delay_estimator.hpp"
#include "application/erle_meter.hpp"
#include "application/sample_queue.hpp"
#include "application/streaming_resampler.hpp"
#include "application/timestamped_audio_buffer.hpp"
#include "infrastructure/wav.hpp"
#include "infrastructure/windows/reference_bus.hpp"
#include "infrastructure/windows/telemetry_bus.hpp"
#include "infrastructure/windows/wasapi_format.hpp"

namespace {

void require(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_sample_queue() {
  echonull::SampleQueue queue(5);
  const std::vector<float> first{1, 2, 3, 4};
  const std::vector<float> second{5, 6, 7};
  require(queue.push(first) == 0, "unexpected initial queue drop");
  require(queue.push(second) == 2, "queue did not drop the oldest samples");
  std::vector<float> output(6);
  require(queue.pop(output) == 5, "queue pop count is wrong");
  require(output[0] == 3 && output[4] == 7 && output[5] == 0, "queue contents are wrong");
}

void test_timestamped_buffer() {
  echonull::TimestampedAudioBuffer buffer(1000);
  std::vector<float> samples(480);
  for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<float>(i);
  constexpr std::int64_t start = 5'000'000;
  buffer.push(start, samples);
  std::vector<float> output(480);
  require(buffer.read(start, output) > 0.99, "timestamped buffer lost coverage");
  require(std::abs(output[100] - 100.0F) < 0.01F, "timestamped buffer changed aligned data");
  std::vector<float> missing(48);
  require(buffer.read(start - 1'000'000, missing) == 0.0, "timestamped buffer hid a gap");
}

void test_delay_estimator() {
  constexpr std::size_t duration_ms = 3000;
  constexpr std::size_t delay_ms = 37;
  std::vector<float> far_end(duration_ms * 48);
  std::vector<float> near_end(duration_ms * 48);
  std::uint32_t state = 0x12345678U;
  for (std::size_t millisecond = 0; millisecond < duration_ms; ++millisecond) {
    state = state * 1664525U + 1013904223U;
    const float amplitude = 0.01F + 0.3F * static_cast<float>((state >> 8) & 0xFFFFU) / 65535.0F;
    for (std::size_t sample = 0; sample < 48; ++sample) {
      far_end[millisecond * 48 + sample] = amplitude;
      if (millisecond >= delay_ms) {
        near_end[millisecond * 48 + sample] = far_end[(millisecond - delay_ms) * 48 + sample];
      }
    }
  }

  echonull::DelayEstimator estimator(echonull::kSampleRate, 20.0, 100.0);
  for (std::size_t offset = 0; offset < far_end.size(); offset += 480) {
    estimator.add(std::span<const float>(near_end).subspan(offset, 480),
                  std::span<const float>(far_end).subspan(offset, 480));
  }
  require(estimator.has_estimate(), "delay estimator produced no estimate");
  require(std::abs(estimator.delay_ms() - delay_ms) < 2.0, "delay estimator chose the wrong lag");
  require(estimator.confidence() > 0.8, "delay estimator confidence is unexpectedly low");
}

void test_erle() {
  std::vector<float> before(480, 0.2F);
  std::vector<float> after(480, 0.02F);
  std::vector<float> far_end(480, 0.5F);
  echonull::ErleMeter meter;
  meter.add(before, after, far_end);
  require(std::abs(meter.erle_db() - 20.0) < 0.01, "ERLE calculation is wrong");
}

void test_wav_roundtrip() {
  const auto path = std::filesystem::temp_directory_path() / "echonull-core-test.wav";
  const std::vector<float> input{0.0F, 0.25F, -0.5F, 1.0F};
  {
    echonull::WavWriter writer(path, echonull::kSampleRate);
    writer.write(input);
  }
  const auto decoded = echonull::read_wav_mono(path);
  std::filesystem::remove(path);
  require(decoded.sample_rate == echonull::kSampleRate, "WAV sample rate changed");
  require(decoded.mono_samples == input, "WAV float samples changed");
}

void test_streaming_resampler_is_chunk_invariant() {
  std::vector<float> input(9600, 1.0F);
  echonull::StreamingResampler one_shot(96000, 48000);
  const auto expected = one_shot.push(input).samples;

  echonull::StreamingResampler chunked(96000, 48000);
  std::vector<float> actual;
  std::size_t offset = 0;
  for (const std::size_t chunk_size : {113U, 509U, 37U, 1024U, 2111U, 5806U}) {
    if (offset >= input.size()) break;
    const auto count = std::min(chunk_size, input.size() - offset);
    auto result = chunked.push(std::span<const float>(input).subspan(offset, count));
    actual.insert(actual.end(), result.samples.begin(), result.samples.end());
    offset += count;
  }
  if (offset < input.size()) {
    auto result = chunked.push(std::span<const float>(input).subspan(offset));
    actual.insert(actual.end(), result.samples.begin(), result.samples.end());
  }

  require(actual.size() == expected.size(), "resampler output depends on packet boundaries");
  for (std::size_t index = 0; index < actual.size(); ++index) {
    require(std::abs(actual[index] - expected[index]) < 1.0e-6F,
            "resampler changed a sample at a packet boundary");
  }
  require(expected.size() > 4700 && expected.size() < 4900,
          "96 kHz to 48 kHz conversion produced the wrong duration");
  for (std::size_t index = 32; index + 32 < expected.size(); ++index) {
    require(std::abs(expected[index] - 1.0F) < 1.0e-4F,
            "resampler does not preserve a steady signal");
  }
}

void test_wasapi_stereo_float_downmix() {
  WAVEFORMATEXTENSIBLE format{};
  format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  format.Format.nChannels = 2;
  format.Format.nSamplesPerSec = 96000;
  format.Format.wBitsPerSample = 32;
  format.Format.nBlockAlign = 8;
  format.Format.nAvgBytesPerSec = 768000;
  format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  format.Samples.wValidBitsPerSample = 32;
  format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

  echonull::WasapiFormat decoder(format.Format);
  const std::vector<float> stereo{1.0F, -1.0F, 0.5F, 0.25F};
  std::vector<float> mono(2);
  decoder.decode_mono(reinterpret_cast<const BYTE*>(stereo.data()), 2, mono);
  require(decoder.sample_rate() == 96000 && decoder.channels() == 2,
          "WASAPI mix format metadata was lost");
  require(std::abs(mono[0]) < 1.0e-6F && std::abs(mono[1] - 0.375F) < 1.0e-6F,
          "stereo float downmix is incorrect");
}

void test_reference_bus_roundtrip() {
  echonull::ReferenceBusWriter writer;
  writer.open();
  echonull::ReferenceBusReader reader;
  require(reader.open(), "reference bus reader could not open the writer mapping");
  const std::vector<float> expected{0.125F, -0.25F, 0.5F, -1.0F};
  constexpr std::int64_t timestamp = 8'765'432'100;
  writer.publish(timestamp, expected);
  const auto blocks = reader.read_available();
  require(!blocks.empty(), "reference bus published no readable block");
  const auto& block = blocks.back();
  require(block.timestamp_hns == timestamp, "reference bus changed the timestamp");
  require(block.samples == expected, "reference bus changed the samples");
}

void test_telemetry_bus_roundtrip() {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  const auto now_hns = static_cast<std::int64_t>(
      static_cast<long double>(counter.QuadPart) * 10'000'000.0L /
      static_cast<long double>(frequency.QuadPart));

  echonull::TelemetryBusWriter writer;
  writer.open();
  echonull::TelemetryBusReader reader;
  require(reader.open(), "telemetry reader could not open the writer mapping");
  const echonull::TelemetrySnapshot expected{now_hns, 0.625F, 3, 1, 0, 0};
  writer.publish(expected);
  const auto actual = reader.read_latest();
  require(actual.has_value(), "telemetry bus returned no fresh snapshot");
  require(std::abs(actual->output_level - expected.output_level) < 1.0e-6F,
          "telemetry bus changed the microphone level");
  require(actual->runtime_state == expected.runtime_state &&
              actual->noise_state == expected.noise_state,
          "telemetry bus changed the runtime state");
}

}  // namespace

int main() {
  try {
    test_sample_queue();
    test_timestamped_buffer();
    test_delay_estimator();
    test_erle();
    test_wav_roundtrip();
    test_streaming_resampler_is_chunk_invariant();
    test_wasapi_stereo_float_downmix();
    test_reference_bus_roundtrip();
    test_telemetry_bus_roundtrip();
    std::cout << "All EchoNull core tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}
