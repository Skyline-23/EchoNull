#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "plugin/plugin_processor.hpp"
#include "infrastructure/windows/performance_clock.hpp"

// Full production processor, synthetic 96 kHz stereo mic + timestamped playback.
// Test-only hooks are compiled out of the distributed plug-in. No audio devices.
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc < 2) throw std::runtime_error("usage: pipeline-benchmark bundled-plugin [delay-ms] [sample-rate] [frames]");
    const auto delay = argc > 2 ? std::stoul(argv[2]) : 0U;
    const auto rate = argc > 3 ? std::stoul(argv[3]) : 96000U;
    const auto frames = argc > 4 ? std::stoul(argv[4]) : 600U;
    if ((rate != 48000 && rate != 96000) || frames < 200 || frames > 3000 || delay > 250)
      throw std::runtime_error("probe bounds: 48/96 kHz, 200-3000 frames, <=250 ms injected delay");
    const auto count = rate / 100;
    echonull::PluginProcessor processor;
    processor.set_sample_rate(rate);
    processor.set_noise_enabled(true);
    processor.start(argv[1]);
    if (processor.aec_error_reason() != echonull::PluginErrorReason::none ||
        processor.noise_error_reason() != echonull::PluginErrorReason::none)
      throw std::runtime_error("both GPU effects must initialize");
    std::vector<float> reference(480 * frames + 48000);
    std::uint32_t rng = 42;
    for (auto& sample : reference) {
      rng = 1664525U * rng + 1013904223U;
      sample = (static_cast<float>(rng >> 8) / 16777216.0F - .5F) * .15F;
    }
    std::vector<float> input(count), left(count), right(count);
    const float* inputs[] = {input.data(), input.data()};
    float* outputs[] = {left.data(), right.data()};
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
    if (!timer) throw std::runtime_error("high resolution timer failed");
    const auto timer_guard = std::unique_ptr<void, decltype(&CloseHandle)>(timer, CloseHandle);
    const auto origin = echonull::performance_timestamp_hns();
    processor.test_feed_reference(origin - 1'000'000, std::move(reference));
    std::cout << "READY\n" << std::flush;
    double energy = 0;
    std::int64_t callback_max_hns = 0;
    for (unsigned long frame = 0; frame < frames; ++frame) {
      const auto wait = origin + frame * 100'000LL - echonull::performance_timestamp_hns();
      if (wait > 0) {
        LARGE_INTEGER due{};
        due.QuadPart = -wait;
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
          throw std::runtime_error("timer setup failed");
        WaitForSingleObject(timer, INFINITE);
      }
      for (std::size_t i = 0; i < count; ++i) {
        const auto t = static_cast<double>(frame * count + i) / rate;
        input[i] = .13F * static_cast<float>(std::sin(t * 1320.0));
      }
      if (frame == 100 && delay != 0) processor.test_delay_next_job(delay);
      const auto begin = echonull::performance_timestamp_hns();
      processor.process(inputs, outputs, static_cast<std::int32_t>(count), 2);
      callback_max_hns = std::max(callback_max_hns,
          echonull::performance_timestamp_hns() - begin);
      for (const auto value : left) {
        if (!std::isfinite(value)) throw std::runtime_error("nonfinite output");
        energy += static_cast<double>(value) * value;
      }
    }
    const auto s = processor.test_diagnostics();
    std::cout << "{\"pipeline\":true,\"sample_rate\":" << rate
      << ",\"injected_delay_ms\":" << delay
      << ",\"gpu_priority\":" << s.gpu_priority_class
      << ",\"priority_status\":" << s.gpu_priority_status
      << ",\"shared_context\":" << s.shared_cuda_context
      << ",\"aec_frames\":" << s.aec_processed_frames
      << ",\"noise_frames\":" << s.noise_processed_frames
      << ",\"protected_miss_frames\":" << s.protected_miss_frames
      << ",\"queue_overruns\":" << s.queue_overruns
      << ",\"output_underrun_samples\":" << s.output_underrun_samples
      << ",\"gpu_over_10ms\":" << s.gpu_deadline_misses
      << ",\"gpu_run_max_us\":" << s.gpu_run_max_us
      << ",\"callback_max_us\":" << callback_max_hns / 10
      << ",\"output_energy\":" << energy << "}\n";
    if (s.aec_processed_frames < frames - 10 || s.noise_processed_frames < frames - 10 ||
        s.aec_error || s.noise_error || s.queue_overruns || s.output_underrun_samples)
      throw std::runtime_error("pipeline failed or skipped enabled effects");
    if ((delay < 60 && s.protected_miss_frames != 0) ||
        (delay >= 80 && s.protected_miss_frames == 0))
      throw std::runtime_error("deadline regression");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
