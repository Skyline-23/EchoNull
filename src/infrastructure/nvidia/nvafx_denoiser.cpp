#include "infrastructure/nvidia/nvafx_denoiser.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#if ECHONULL_HAS_NVAFX
#include <nvAudioEffects.h>
#include "infrastructure/nvidia/nvafx_api.hpp"

#if defined(NVAFX_PARAM_NUM_SAMPLES_PER_INPUT_FRAME)
#define ECHONULL_DENOISER_INPUT_FRAME_PARAM NVAFX_PARAM_NUM_SAMPLES_PER_INPUT_FRAME
#else
#define ECHONULL_DENOISER_INPUT_FRAME_PARAM NVAFX_PARAM_NUM_INPUT_SAMPLES_PER_FRAME
#endif

#if defined(NVAFX_PARAM_NUM_SAMPLES_PER_OUTPUT_FRAME)
#define ECHONULL_DENOISER_OUTPUT_FRAME_PARAM NVAFX_PARAM_NUM_SAMPLES_PER_OUTPUT_FRAME
#else
#define ECHONULL_DENOISER_OUTPUT_FRAME_PARAM NVAFX_PARAM_NUM_OUTPUT_SAMPLES_PER_FRAME
#endif
#endif

namespace echonull {

struct NvafxDenoiser::Impl {
  std::vector<std::filesystem::path> model_paths;
  std::atomic<float> requested_intensity{1.0F};
  float applied_intensity = 1.0F;
  std::uint32_t sample_rate = kSampleRate;
  mutable std::mutex mutex;
  DenoiserStatus status;
#if ECHONULL_HAS_NVAFX
  NvAFX_Handle handle = nullptr;
#endif
};

NvafxDenoiser::NvafxDenoiser(std::vector<std::filesystem::path> model_paths,
                             const float intensity,
                             const std::uint32_t sample_rate)
    : impl_(std::make_unique<Impl>()) {
  impl_->model_paths = std::move(model_paths);
  impl_->requested_intensity.store(std::clamp(intensity, 0.0F, 1.0F),
                                   std::memory_order_relaxed);
  impl_->sample_rate = sample_rate;
}

NvafxDenoiser::~NvafxDenoiser() { reset(); }

void NvafxDenoiser::initialize() {
  reset();
#if !ECHONULL_HAS_NVAFX
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->status.error = "EchoNull was built without the NVIDIA AFX SDK";
  }
  throw std::runtime_error("NvAFX Denoiser is unavailable in this build");
#else
  const auto check = [](const NvAFX_Status value, const char* operation) {
    if (value != NVAFX_STATUS_SUCCESS) {
      throw std::runtime_error(std::string(operation) +
                               " failed with NvAFX status " +
                               std::to_string(static_cast<int>(value)));
    }
  };

  std::string last_error = "no compatible NvAFX Denoiser model was found";
  for (const auto& model_path : impl_->model_paths) try {
    auto& api = NvafxApi::instance();
    if (model_path.empty() || !std::filesystem::exists(model_path)) {
      throw std::runtime_error("NvAFX Noise Removal model not found: " +
                               model_path.string());
    }
    check(api.create_effect("denoiser", &impl_->handle),
          "NvAFX_CreateEffect(denoiser)");
    const auto model_utf8 = model_path.u8string();
    const auto* model = reinterpret_cast<const char*>(model_utf8.c_str());
    check(api.set_string(impl_->handle, NVAFX_PARAM_MODEL_PATH, model),
          "NvAFX_SetString(noise_model_path)");
    check(api.set_u32(impl_->handle, NVAFX_PARAM_INPUT_SAMPLE_RATE,
                       impl_->sample_rate),
          "NvAFX_SetU32(input_sample_rate)");
    check(api.set_u32(impl_->handle, NVAFX_PARAM_OUTPUT_SAMPLE_RATE,
                       impl_->sample_rate),
          "NvAFX_SetU32(output_sample_rate)");
    const float intensity =
        impl_->requested_intensity.load(std::memory_order_relaxed);
    check(api.set_float(impl_->handle, NVAFX_PARAM_INTENSITY_RATIO, intensity),
          "NvAFX_SetFloat(intensity_ratio)");
    check(api.load_effect(impl_->handle), "NvAFX_Load(denoiser)");

    DenoiserStatus current;
    check(api.get_u32(impl_->handle, NVAFX_PARAM_INPUT_SAMPLE_RATE,
                       &current.input_sample_rate),
          "NvAFX_GetU32(input_sample_rate)");
    check(api.get_u32(impl_->handle, NVAFX_PARAM_OUTPUT_SAMPLE_RATE,
                       &current.output_sample_rate),
          "NvAFX_GetU32(output_sample_rate)");
    check(api.get_u32(impl_->handle, NVAFX_PARAM_NUM_INPUT_CHANNELS,
                       &current.input_channels),
          "NvAFX_GetU32(input_channels)");
    check(api.get_u32(impl_->handle, NVAFX_PARAM_NUM_OUTPUT_CHANNELS,
                       &current.output_channels),
          "NvAFX_GetU32(output_channels)");
    check(api.get_u32(impl_->handle, ECHONULL_DENOISER_INPUT_FRAME_PARAM,
                       &current.input_frame_samples),
          "NvAFX_GetU32(input_frame_samples)");
    check(api.get_u32(impl_->handle, ECHONULL_DENOISER_OUTPUT_FRAME_PARAM,
                       &current.output_frame_samples),
          "NvAFX_GetU32(output_frame_samples)");
    if (current.input_sample_rate != impl_->sample_rate ||
        current.output_sample_rate != impl_->sample_rate ||
        current.input_channels != 1 || current.output_channels != 1 ||
        current.input_frame_samples == 0 ||
        current.input_frame_samples != current.output_frame_samples) {
      throw std::runtime_error("NvAFX Denoiser reported an incompatible stream format");
    }
    current.ready = true;
    std::scoped_lock lock(impl_->mutex);
    impl_->applied_intensity = intensity;
    impl_->status = current;
    return;
  } catch (const std::exception& error) {
    last_error = error.what();
    if (impl_->handle != nullptr) {
      NvafxApi::instance().destroy_effect(impl_->handle);
      impl_->handle = nullptr;
    }
  }
  std::scoped_lock lock(impl_->mutex);
  impl_->status = {};
  impl_->status.error = last_error;
  throw std::runtime_error(last_error);
#endif
}

void NvafxDenoiser::reset() noexcept {
#if ECHONULL_HAS_NVAFX
  std::scoped_lock lock(impl_->mutex);
  if (impl_->handle != nullptr) {
    NvafxApi::instance().reset(impl_->handle);
    NvafxApi::instance().destroy_effect(impl_->handle);
    impl_->handle = nullptr;
  }
  impl_->status = {};
#else
  std::scoped_lock lock(impl_->mutex);
  impl_->status = {};
#endif
}

void NvafxDenoiser::set_intensity(const float intensity) {
  const float value = std::clamp(intensity, 0.0F, 1.0F);
  impl_->requested_intensity.store(value, std::memory_order_relaxed);
}

void NvafxDenoiser::process(const std::span<const float> input,
                            const std::span<float> output) {
#if !ECHONULL_HAS_NVAFX
  static_cast<void>(input);
  static_cast<void>(output);
  throw std::runtime_error("NvAFX Denoiser is unavailable in this build");
#else
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->status.ready || impl_->handle == nullptr) {
    throw std::runtime_error("NvAFX Denoiser is not initialized");
  }
  if (input.size() != impl_->status.input_frame_samples ||
      output.size() != impl_->status.output_frame_samples) {
    throw std::runtime_error("NvAFX Denoiser frame size mismatch");
  }
  const float requested_intensity =
      impl_->requested_intensity.load(std::memory_order_relaxed);
  if (requested_intensity != impl_->applied_intensity) {
    if (NvafxApi::instance().set_float(
            impl_->handle, NVAFX_PARAM_INTENSITY_RATIO,
            requested_intensity) != NVAFX_STATUS_SUCCESS) {
      throw std::runtime_error("NvAFX_SetFloat(denoiser intensity) failed");
    }
    impl_->applied_intensity = requested_intensity;
  }
  const float* input_buffers[1] = {input.data()};
  float* output_buffers[1] = {output.data()};
  const NvAFX_Status result = NvafxApi::instance().run(
      impl_->handle, input_buffers, output_buffers,
      impl_->status.input_frame_samples, impl_->status.input_channels);
  if (result != NVAFX_STATUS_SUCCESS) {
    throw std::runtime_error("NvAFX_Run(denoiser) failed with status " +
                             std::to_string(static_cast<int>(result)));
  }
#endif
}

DenoiserStatus NvafxDenoiser::status() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->status;
}

}  // namespace echonull
