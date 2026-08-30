#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "application/delay_estimator.hpp"
#include "application/erle_meter.hpp"
#include "application/sample_queue.hpp"
#include "application/timestamped_audio_buffer.hpp"
#include "infrastructure/wav.hpp"

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
  std::vector<float> far(duration_ms * 48);
  std::vector<float> near(duration_ms * 48);
  std::uint32_t state = 0x12345678U;
  for (std::size_t millisecond = 0; millisecond < duration_ms; ++millisecond) {
    state = state * 1664525U + 1013904223U;
    const float amplitude = 0.01F + 0.3F * static_cast<float>((state >> 8) & 0xFFFFU) / 65535.0F;
    for (std::size_t sample = 0; sample < 48; ++sample) {
      far[millisecond * 48 + sample] = amplitude;
      if (millisecond >= delay_ms) near[millisecond * 48 + sample] = far[(millisecond - delay_ms) * 48 + sample];
    }
  }

  echonull::DelayEstimator estimator(echonull::kSampleRate, 20.0, 100.0);
  for (std::size_t offset = 0; offset < far.size(); offset += 480) {
    estimator.add(std::span<const float>(near).subspan(offset, 480),
                  std::span<const float>(far).subspan(offset, 480));
  }
  require(estimator.has_estimate(), "delay estimator produced no estimate");
  require(std::abs(estimator.delay_ms() - delay_ms) < 2.0, "delay estimator chose the wrong lag");
  require(estimator.confidence() > 0.8, "delay estimator confidence is unexpectedly low");
}

void test_erle() {
  std::vector<float> before(480, 0.2F);
  std::vector<float> after(480, 0.02F);
  std::vector<float> far(480, 0.5F);
  echonull::ErleMeter meter;
  meter.add(before, after, far);
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

}  // namespace

int main() {
  try {
    test_sample_queue();
    test_timestamped_buffer();
    test_delay_estimator();
    test_erle();
    test_wav_roundtrip();
    std::cout << "All EchoNull core tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}

