#include <algorithm>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "application/delay_estimator.hpp"
#include "application/protected_output.hpp"
#include "application/streaming_resampler.hpp"
#include "application/timestamped_audio_buffer.hpp"
#include "infrastructure/equalizer_apo_config.hpp"
#include "infrastructure/windows/diagnostic_log.hpp"
#include "infrastructure/windows/telemetry_bus.hpp"

namespace {

void require(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
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

void test_streaming_resampler_equal_rate_is_bit_exact() {
  std::vector<float> input(2048);
  for (std::size_t index = 0; index < input.size(); ++index) {
    input[index] = static_cast<float>(index % 257) / 128.0F - 1.0F;
  }

  echonull::StreamingResampler resampler(48000, 48000);
  std::vector<float> actual;
  for (std::size_t offset = 0; offset < input.size(); offset += 127) {
    const auto count = std::min<std::size_t>(127, input.size() - offset);
    auto result = resampler.push(
        std::span<const float>(input).subspan(offset, count));
    require(result.first_input_frame == static_cast<double>(offset),
            "equal-rate resampler changed the stream position");
    actual.insert(actual.end(), result.samples.begin(), result.samples.end());
  }
  require(actual == input, "equal-rate resampler changed audio samples");
}

void test_protected_output_never_exposes_raw_on_gpu_miss() {
  echonull::ProtectedOutput transition;
  std::vector<float> dry(480, 0.9F);
  std::vector<float> wet(480, -0.4F);
  std::vector<float> output(480);
  transition.render(dry, wet, true, true, output);
  require(std::abs(output.front()) < 0.01F &&
              std::abs(output.back() - wet.back()) < 1.0e-6F,
          "processed startup did not fade in from silence");
  transition.render(dry, wet, true, true, output);
  require(output == wet, "steady GPU output was changed");
  transition.render(dry, {}, true, false, output);
  require(std::abs(output.front() - wet.back()) < 0.01F &&
              output.back() == 0.0F &&
              std::all_of(output.begin(), output.end(), [](float x) { return x <= 0; }),
          "GPU miss exposed unprocessed audio or clicked");
  // A partial chain is not a successful result, even if it has audio samples.
  transition.render(dry, dry, true, false, output);
  require(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }),
          "incomplete AEC/noise chain exposed raw audio");
  transition.render(dry, wet, true, true, output);
  require(std::abs(output.front()) < 0.01F && output.back() == wet.back(),
          "GPU recovery did not use the processed signal");
  transition.render(dry, {}, false, false, output);
  require(output == dry, "explicit effect bypass changed the input");
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
  const echonull::TelemetrySnapshot expected{
      now_hns, 0.625F, 3, 1, 0, 0, 7, 5, 3, 11, 100, 100, 12345, 4, 0, 1};
  writer.publish(expected);
  const auto actual = reader.read_latest();
  require(actual.has_value(), "telemetry bus returned no fresh snapshot");
  require(std::abs(actual->output_level - expected.output_level) < 1.0e-6F,
          "telemetry bus changed the microphone level");
  require(actual->runtime_state == expected.runtime_state &&
              actual->noise_state == expected.noise_state,
          "telemetry bus changed the runtime state");
  require(actual->protected_miss_frames == expected.protected_miss_frames &&
              actual->gpu_deadline_misses == expected.gpu_deadline_misses &&
              actual->queue_overruns == expected.queue_overruns &&
              actual->output_underrun_samples ==
                  expected.output_underrun_samples,
          "telemetry bus changed the real-time diagnostics");
  require(actual->aec_processed_frames == 100 && actual->noise_processed_frames == 100 &&
              actual->gpu_run_max_us == 12345 && actual->gpu_priority_class == 4 &&
              actual->gpu_priority_status == 0 && actual->shared_cuda_context == 1,
          "telemetry bus lost GPU execution diagnostics");

  auto stale = expected;
  stale.timestamp_hns = now_hns - 30'000'000;
  writer.publish(stale);
  require(!reader.read_latest().has_value(),
          "telemetry reader accepted a stale audio-engine snapshot");
  writer.publish(expected);
  require(reader.read_latest().has_value(),
          "telemetry reader did not reconnect after a stale mapping");
}

void test_async_diagnostic_log() {
  std::wstring temporary(32'768, L'\0');
  const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()),
                                    temporary.data());
  require(length != 0 && length < temporary.size(),
          "diagnostic test could not resolve the temp directory");
  temporary.resize(length);
  const auto directory = std::filesystem::path(temporary) /
                         (L"EchoNull-log-test-" +
                          std::to_wstring(GetCurrentProcessId()));
  std::filesystem::remove_all(directory);
  require(SetEnvironmentVariableW(L"ECHONULL_LOG_ROOT",
                                  directory.c_str()) != FALSE,
          "diagnostic test could not set its log directory");
  {
    echonull::AsyncDiagnosticLog log;
    log.open();
    log.publish(echonull::TelemetrySnapshot{
        60'000'000, 0.5F, 5, 3, 3, 0, 4, 2, 1, 17, 123, 123, 17000, 4, 0, 1});
    log.close();
  }
  SetEnvironmentVariableW(L"ECHONULL_LOG_ROOT", nullptr);
  std::ifstream input(directory / L"EchoNull.log", std::ios::binary);
  const std::string contents{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
  require(contents.find("ERROR runtime=overloaded") != std::string::npos &&
              contents.find("aec_error=runtime_or_gpu") !=
                  std::string::npos,
          "diagnostic log omitted the runtime error");
  require(contents.find("protected_miss_frames=4") != std::string::npos &&
              contents.find("output_underrun_samples=17") !=
                  std::string::npos,
          "diagnostic log omitted the real-time counters");
  require(contents.find("GPU_SETUP priority_class=4 priority_status=0 shared_cuda_context=1") != std::string::npos &&
              contents.find("aec_processed_frames=123 noise_processed_frames=123 gpu_run_max_us=17000") != std::string::npos,
          "diagnostic log omitted GPU setup or effect progress");
  require(contents.find("pid=" + std::to_string(GetCurrentProcessId()) +
                            " session=") != std::string::npos,
          "diagnostic log omitted its process and session identity");
  input.close();
  std::filesystem::remove_all(directory);
}

void test_equalizer_apo_capture_scope() {
  const std::string original =
      "Preamp: -3 dB\r\n"
      "VSTPlugin: Library EchoNullPlugin.dll ChunkData \"saved\"\r\n"
      "Filter: ON HP Fc 80 Hz\r\n";
  const auto installed = echonull::install_echonull_capture_scope(original);
  require(echonull::has_capture_scoped_echonull(installed),
          "installer did not capture-scope EchoNull");
  require(installed.find("ChunkData \"saved\"") != std::string::npos,
          "installer discarded EchoNull VST state");
  require(installed.find("If: stage == \"capture\"") != std::string::npos &&
              installed.find("EndIf:") != std::string::npos,
          "installer did not close the capture condition");
  require(echonull::install_echonull_capture_scope(installed) == installed,
          "installer capture block is not idempotent");
  const auto removed = echonull::remove_echonull_capture_scope(installed);
  require(removed.find("EchoNullPlugin.dll") == std::string::npos,
          "uninstaller left the owned EchoNull line behind");
  require(removed.find("Filter: ON HP Fc 80 Hz") != std::string::npos,
          "uninstaller removed an unrelated filter");

  require(!echonull::has_capture_scoped_echonull(
              "VSTPlugin: Library EchoNullPlugin.dll\n"),
          "unguarded EchoNull configuration was accepted");
  require(echonull::has_capture_scoped_echonull(
              "Stage: capture\nVSTPlugin: Library EchoNullPlugin.dll\n"),
          "legacy capture Stage configuration was rejected");
  const std::string existing_stage =
      "Stage: capture\r\n"
      "VSTPlugin: Library EchoNullPlugin.dll ChunkData \"saved\"\r\n"
      "# VSTPlugin: Library ReverbSolo.dll\r\n";
  require(echonull::install_echonull_capture_scope(existing_stage) ==
              existing_stage,
          "installer added a redundant block to a capture-scoped plug-in");
  const std::string redundant_old_block =
      "Stage: capture\r\n"
      "# EchoNull setup begin\r\n"
      "If: stage == \"capture\"\r\n"
      "VSTPlugin: Library EchoNullPlugin.dll ChunkData \"saved\"\r\n"
      "EndIf:\r\n"
      "# EchoNull setup end\r\n";
  require(echonull::install_echonull_capture_scope(redundant_old_block) ==
              "Stage: capture\r\n"
              "VSTPlugin: Library EchoNullPlugin.dll ChunkData \"saved\"\r\n",
          "installer failed to remove an old redundant capture block");
  const auto removed_stage_plugin =
      echonull::remove_echonull_capture_scope(existing_stage);
  require(removed_stage_plugin.find("EchoNullPlugin.dll") ==
              std::string::npos &&
              removed_stage_plugin.find("Stage: capture") !=
                  std::string::npos &&
              removed_stage_plugin.find("ReverbSolo.dll") !=
                  std::string::npos,
          "uninstaller changed unrelated capture configuration");
}

}  // namespace

int main() {
  try {
    test_timestamped_buffer();
    test_delay_estimator();
    test_streaming_resampler_is_chunk_invariant();
    test_streaming_resampler_equal_rate_is_bit_exact();
    test_protected_output_never_exposes_raw_on_gpu_miss();
    test_telemetry_bus_roundtrip();
    test_async_diagnostic_log();
    test_equalizer_apo_capture_scope();
    std::cout << "All EchoNull core tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}
