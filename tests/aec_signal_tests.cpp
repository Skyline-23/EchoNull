#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "infrastructure/nvidia/cuda_audio_context.hpp"
#include "infrastructure/nvidia/nvafx_aec.hpp"
#include "infrastructure/nvidia/packaged_runtime.hpp"

// Offline signal test. Generates its own signal; never opens a microphone.
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc < 2 || argc > 3) throw std::runtime_error("expected the bundled plugin path and optional echo delay in ms");
    const auto delay_ms = argc == 3 ? std::stoul(argv[2]) : 0U;
    if (delay_ms > 250) throw std::runtime_error("maximum synthetic delay is 250 ms");
    const auto runtime = echonull::PackagedRuntime::prepare(argv[1]);
    echonull::CudaAudioContext context;
    echonull::NvafxAec aec(runtime.aec_models, 1.0F);
    aec.initialize(true);
    const auto count = aec.status().input_frame_samples;
    std::vector<float> near(count), far(count), output(count);
    const auto delay_samples = delay_ms * 48;
    std::vector<float> history(delay_samples + 1, 0);
    std::size_t position = 0;
    std::uint32_t rng = 17;
    float smooth = 0;
    double before = 0, after = 0;
    for (unsigned frame = 0; frame < 700; ++frame) {
      for (unsigned i = 0; i < count; ++i) {
        rng = rng * 1664525U + 1013904223U;
        const float noise = static_cast<float>(rng >> 8) / 16777216.0F - 0.5F;
        smooth = 0.75F * smooth + 0.25F * noise;
        const double time = static_cast<double>(frame * count + i) / 48000;
        const float envelope = static_cast<float>(0.5 + 0.4 * std::sin(2 * 3.141592653589793 * time * 3.1));
        far[i] = smooth * envelope;
        history[position % history.size()] = far[i];
        near[i] = position >= delay_samples
            ? history[(position - delay_samples) % history.size()] * 0.6F : 0;
        ++position;
      }
      aec.process(near, far, output);
      if (frame > 300) for (unsigned i = 0; i < count; ++i) {
        if (!std::isfinite(output[i])) throw std::runtime_error("non-finite AEC output");
        before += near[i] * near[i]; after += output[i] * output[i];
      }
    }
    const double reduction = 10 * std::log10(before / std::max(after, 1e-20));
    std::cout << "{\"echo_delay_ms\":" << delay_ms << ",\"synthetic_echo_reduction_db\":" << reduction
              << ",\"input_energy\":" << before << ",\"output_energy\":" << after << "}\n";
    if (reduction < 6) throw std::runtime_error("AEC failed to suppress the known reference echo");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
