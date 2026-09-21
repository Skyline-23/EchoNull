#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <Windows.h>

#include "infrastructure/nvidia/cuda_audio_context.hpp"
#include "infrastructure/nvidia/nvafx_aec.hpp"
#include "infrastructure/nvidia/nvafx_denoiser.hpp"
#include "infrastructure/nvidia/packaged_runtime.hpp"
#include "infrastructure/windows/gpu_priority.hpp"
#include "infrastructure/windows/realtime_audio_thread.hpp"

// Manual non-recording probe. Run alongside a bounded GPU load; no audio devices
// are opened. Timings include both effects, with nonzero synthetic input.
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc < 3) throw std::runtime_error("usage: benchmark bundled-plugin shared|private [high]");
    const bool use_shared = std::wstring(argv[2]) == L"shared";
    const auto runtime = echonull::PackagedRuntime::prepare(argv[1]);
    echonull::GpuPriority priority(argc > 3 && std::wstring(argv[3]) == L"high");
    std::unique_ptr<echonull::CudaAudioContext> context;
    if (use_shared) context = std::make_unique<echonull::CudaAudioContext>();
    echonull::ensure_realtime_audio_thread_priority();
    echonull::NvafxAec aec(runtime.aec_models, 1.0F);
    echonull::NvafxDenoiser noise(runtime.noise_models, 1.0F);
    aec.initialize(use_shared);
    noise.initialize(use_shared);
    const auto count = aec.status().input_frame_samples;
    if (noise.status().input_frame_samples != count) throw std::runtime_error("frame mismatch");
    std::vector<float> near_end(count), far_end(count), processed(count), output(count);
    std::vector<double> durations, completion_delays;
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
    if (!timer) throw std::runtime_error("high-resolution timer unavailable");
    const auto close_timer = std::unique_ptr<void, decltype(&CloseHandle)>(timer, CloseHandle);
    std::uint32_t rng = 0x12345678;
    double energy = 0.0;
    auto epoch = std::chrono::steady_clock::now();
    for (int frame = -100; frame < 600; ++frame) {
      if (frame == 0) { epoch = std::chrono::steady_clock::now(); std::cout << "READY\n" << std::flush; }
      const auto scheduled = epoch + std::chrono::milliseconds(std::max(frame, 0) * 10);
      if (frame >= 0 && scheduled > std::chrono::steady_clock::now()) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<std::int64_t>(1,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                scheduled - std::chrono::steady_clock::now()).count() / 100);
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
          throw std::runtime_error("timer setup failed");
        WaitForSingleObject(timer, INFINITE);
      }
      for (std::size_t i = 0; i < count; ++i) {
        rng = rng * 1664525U + 1013904223U;
        far_end[i] = (static_cast<float>(rng >> 8) / 16777216.0F - 0.5F) * 0.15F;
        const double t = static_cast<double>((frame + 100) * count + i) / 48000.0;
        near_end[i] = far_end[i] * 0.4F + 0.1F * static_cast<float>(std::sin(t * 1320.0));
      }
      const auto begin = std::chrono::steady_clock::now();
      aec.process(near_end, far_end, processed);
      noise.process(processed, output);
      const auto finish = std::chrono::steady_clock::now();
      if (frame >= 0) {
        durations.push_back(std::chrono::duration<double, std::milli>(finish - begin).count());
        completion_delays.push_back(std::chrono::duration<double, std::milli>(finish - scheduled).count());
        for (float value : output) {
          if (!std::isfinite(value)) throw std::runtime_error("nonfinite output");
          energy += static_cast<double>(value) * value;
        }
      }
    }
    auto percentile = [](std::vector<double> values, double p) {
      std::sort(values.begin(), values.end());
      return values[static_cast<std::size_t>(p * (values.size() - 1))];
    };
    std::cout << "{\"shared_context\":" << use_shared
              << ",\"gpu_priority\":" << priority.observed_class()
              << ",\"priority_status\":" << priority.status()
              << ",\"frames\":" << durations.size()
              << ",\"run_p50_ms\":" << percentile(durations, .5)
              << ",\"run_p95_ms\":" << percentile(durations, .95)
              << ",\"run_max_ms\":" << percentile(durations, 1)
              << ",\"completion_p95_ms\":" << percentile(completion_delays, .95)
              << ",\"completion_max_ms\":" << percentile(completion_delays, 1)
              << ",\"over_60ms\":" << std::count_if(completion_delays.begin(), completion_delays.end(), [](double x){ return x > 60; })
              << ",\"output_energy\":" << energy << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
