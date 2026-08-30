#include "infrastructure/nvidia/nvafx_denoiser.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#if ECHONULL_HAS_NVAFX
#include <nvAudioEffects.h>

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
  std::filesystem::path model_path;
  float intensity = 1.0F;
  std::uint32_t sample_rate = kSampleRate;
  mutable std::mutex mutex;
  DenoiserStatus status;
#if ECHONULL_HAS_NVAFX
  NvAFX_Handle handle = nullptr;
#endif
};

NvafxDenoiser::NvafxDenoiser(std::filesystem::path model_path,
                             const float intensity,
                             const std::uint32_t sample_rate)
    : impl_(std::make_unique<Impl>()) {
  impl_->model_path = std::move(model_path);
  impl_->intensity = std::clamp(intensity, 0.0F, 1.0F);
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

  try {
    if (impl_->model_path.empty() || !std::filesystem::exists(impl_->model_path)) {
      throw std::runtime_error("NvAFX Noise Removal model not found: " +
                               impl_->model_path.string());
    }
    check(NvAFX_CreateEffect("denoiser", &impl_->handle),
          "NvAFX_CreateEffect(denoiser)");
    const auto model_utf8 = impl_->model_path.u8string();
    const auto* model = reinterpret_cast<const char*>(model_utf8.c_str());
    check(NvAFX_SetString(impl_->handle, NVAFX_PARAM_MODEL_PATH, model),
          "NvAFX_SetString(noise_model_path)");
    check(NvAFX_SetU32(impl_->handle, NVAFX_PARAM_INPUT_SAMPLE_RATE,
                       impl_->sample_rate),
          "NvAFX_SetU32(input_sample_rate)");
    check(NvAFX_SetU32(impl_->handle, NVAFX_PARAM_OUTPUT_SAMPLE_RATE,
                       impl_->sample_rate),
          "NvAFX_SetU32(output_sample_rate)");
    check(NvAFX_SetFloat(impl_->handle, NVAFX_PARAM_INTENSITY_RATIO,
                         impl_->intensity),
          "NvAFX_SetFloat(intensity_ratio)");
    check(NvAFX_Load(impl_->handle), "NvAFX_Load(denoiser)");

    DenoiserStatus current;
    check(NvAFX_GetU32(impl_->handle, NVAFX_PARAM_INPUT_SAMPLE_RATE,
                       &current.input_sample_rate),
          "NvAFX_GetU32(input_sample_rate)");
    check(NvAFX_GetU32(impl_->handle, NVAFX_PARAM_OUTPUT_SAMPLE_RATE,
                       &current.output_sample_rate),
          "NvAFX_GetU32(output_sample_rate)");
    check(NvAFX_GetU32(impl_->handle, NVAFX_PARAM_NUM_INPUT_CHANNELS,
                       &current.input_channels),
          "NvAFX_GetU32(input_channels)");
    check(NvAFX_GetU32(impl_->handle, NVAFX_PARAM_NUM_OUTPUT_CHANNELS,
                       &current.output_channels),
          "NvAFX_GetU32(output_channels)");
    check(NvAFX_GetU32(impl_->handle, ECHONULL_DENOISER_INPUT_FRAME_PARAM,
                       &current.input_frame_samples),
          "NvAFX_GetU32(input_frame_samples)");
    check(NvAFX_GetU32(impl_->handle, ECHONULL_DENOISER_OUTPUT_FRAME_PARAM,
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
    impl_->status = current;
  } catch (const std::exception& error) {
    if (impl_->handle != nullptr) {
      NvAFX_DestroyEffect(impl_->handle);
      impl_->handle = nullptr;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->status = {};
    impl_->status.error = error.what();
    throw;
  }
#endif
}

void NvafxDenoiser::reset() noexcept {
#if ECHONULL_HAS_NVAFX
  std::scoped_lock lock(impl_->mutex);
  if (impl_->handle != nullptr) {
    NvAFX_Reset(impl_->handle);
    NvAFX_DestroyEffect(impl_->handle);
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
  std::scoped_lock lock(impl_->mutex);
  impl_->intensity = value;
#if ECHONULL_HAS_NVAFX
  if (impl_->handle != nullptr &&
      NvAFX_SetFloat(impl_->handle, NVAFX_PARAM_INTENSITY_RATIO, value) !=
          NVAFX_STATUS_SUCCESS) {
    throw std::runtime_error("NvAFX_SetFloat(denoiser intensity) failed");
  }
#endif
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
  const float* input_buffers[1] = {input.data()};
  float* output_buffers[1] = {output.data()};
  const NvAFX_Status result = NvAFX_Run(
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
