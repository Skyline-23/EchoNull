#include "infrastructure/nvidia/nvafx_aec.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#if ECHONULL_HAS_NVAFX
#include <nvAudioEffects.h>
#include "infrastructure/nvidia/nvafx_api.hpp"
#if __has_include(<nvAFXAec.h>)
#include <nvAFXAec.h>
#endif

#if defined(NVAFX_EFFECT_AEC)
#define ECHONULL_NVAFX_AEC_EFFECT NVAFX_EFFECT_AEC
#else
#define ECHONULL_NVAFX_AEC_EFFECT "aec"
#endif

#if defined(NVAFX_PARAM_NUM_SAMPLES_PER_INPUT_FRAME)
#define ECHONULL_NVAFX_INPUT_FRAME_PARAM NVAFX_PARAM_NUM_SAMPLES_PER_INPUT_FRAME
#else
#define ECHONULL_NVAFX_INPUT_FRAME_PARAM NVAFX_PARAM_NUM_INPUT_SAMPLES_PER_FRAME
#endif

#if defined(NVAFX_PARAM_NUM_SAMPLES_PER_OUTPUT_FRAME)
#define ECHONULL_NVAFX_OUTPUT_FRAME_PARAM NVAFX_PARAM_NUM_SAMPLES_PER_OUTPUT_FRAME
#else
#define ECHONULL_NVAFX_OUTPUT_FRAME_PARAM NVAFX_PARAM_NUM_OUTPUT_SAMPLES_PER_FRAME
#endif
#endif

namespace echonull {

struct NvafxAec::Impl {
  std::filesystem::path model_path;
  float intensity = 1.0F;
  std::uint32_t sample_rate = kSampleRate;
  mutable std::mutex mutex;
  AecStatus status;
#if ECHONULL_HAS_NVAFX
  NvAFX_Handle handle = nullptr;
#endif
};

NvafxAec::NvafxAec(std::filesystem::path model_path, const float intensity,
                   const std::uint32_t sample_rate)
    : impl_(std::make_unique<Impl>()) {
  impl_->model_path = std::move(model_path);
  impl_->intensity = intensity;
  impl_->sample_rate = sample_rate;
}

NvafxAec::~NvafxAec() {
  reset();
}

void NvafxAec::initialize() {
  reset();
#if !ECHONULL_HAS_NVAFX
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->status.error = "EchoNull was built without the NVIDIA AFX SDK";
  }
  throw std::runtime_error("EchoNull was built without the NVIDIA AFX SDK; reconfigure with AFX_SDK_ROOT");
#else
  const auto check = [](const NvAFX_Status value, const char* operation) {
    if (value != NVAFX_STATUS_SUCCESS) {
      throw std::runtime_error(std::string(operation) + " failed with NvAFX status " +
                               std::to_string(static_cast<int>(value)));
    }
  };

  try {
    auto& api = NvafxApi::instance();
    if (impl_->model_path.empty() || !std::filesystem::exists(impl_->model_path)) {
      throw std::runtime_error("NvAFX AEC model not found: " + impl_->model_path.string());
    }
    check(api.create_effect(ECHONULL_NVAFX_AEC_EFFECT, &impl_->handle),
          "NvAFX_CreateEffect(aec)");
    const auto model_utf8 = impl_->model_path.u8string();
    const auto* model = reinterpret_cast<const char*>(model_utf8.c_str());
    check(api.set_string(impl_->handle, NVAFX_PARAM_MODEL_PATH, model),
          "NvAFX_SetString(model_path)");
    check(api.set_u32(impl_->handle, NVAFX_PARAM_INPUT_SAMPLE_RATE, impl_->sample_rate),
          "NvAFX_SetU32(input_sample_rate)");
    check(api.set_u32(impl_->handle, NVAFX_PARAM_OUTPUT_SAMPLE_RATE, impl_->sample_rate),
          "NvAFX_SetU32(output_sample_rate)");
    check(api.set_float(impl_->handle, NVAFX_PARAM_INTENSITY_RATIO, impl_->intensity),
          "NvAFX_SetFloat(intensity_ratio)");
    check(api.load_effect(impl_->handle), "NvAFX_Load(aec)");

    AecStatus current;
    check(api.get_u32(impl_->handle, NVAFX_PARAM_INPUT_SAMPLE_RATE, &current.input_sample_rate),
          "NvAFX_GetU32(input_sample_rate)");
    check(api.get_u32(impl_->handle, NVAFX_PARAM_OUTPUT_SAMPLE_RATE, &current.output_sample_rate),
          "NvAFX_GetU32(output_sample_rate)");
    check(api.get_u32(impl_->handle, NVAFX_PARAM_NUM_INPUT_CHANNELS, &current.input_channels),
          "NvAFX_GetU32(input_channels)");
    check(api.get_u32(impl_->handle, NVAFX_PARAM_NUM_OUTPUT_CHANNELS, &current.output_channels),
          "NvAFX_GetU32(output_channels)");
    check(api.get_u32(impl_->handle, ECHONULL_NVAFX_INPUT_FRAME_PARAM,
                       &current.input_frame_samples),
          "NvAFX_GetU32(input_frame_samples)");
    check(api.get_u32(impl_->handle, ECHONULL_NVAFX_OUTPUT_FRAME_PARAM,
                       &current.output_frame_samples),
          "NvAFX_GetU32(output_frame_samples)");

    if (current.input_sample_rate != impl_->sample_rate ||
        current.output_sample_rate != impl_->sample_rate ||
        current.input_channels != 2 || current.output_channels != 1 ||
        current.input_frame_samples == 0 || current.output_frame_samples == 0) {
      throw std::runtime_error("NvAFX AEC reported an incompatible stream format");
    }
    current.ready = true;
    std::scoped_lock lock(impl_->mutex);
    impl_->status = current;
  } catch (const std::exception& error) {
    if (impl_->handle != nullptr) {
      NvafxApi::instance().destroy_effect(impl_->handle);
      impl_->handle = nullptr;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->status = {};
    impl_->status.error = error.what();
    throw;
  }
#endif
}

void NvafxAec::reset() {
  std::scoped_lock lock(impl_->mutex);
#if ECHONULL_HAS_NVAFX
  if (impl_->handle != nullptr) {
    NvafxApi::instance().reset(impl_->handle);
    NvafxApi::instance().destroy_effect(impl_->handle);
    impl_->handle = nullptr;
  }
#endif
  impl_->status = {};
}

void NvafxAec::set_intensity(const float intensity) {
  const float value = std::clamp(intensity, 0.0F, 1.0F);
  std::scoped_lock lock(impl_->mutex);
  impl_->intensity = value;
#if ECHONULL_HAS_NVAFX
  if (impl_->handle != nullptr &&
      NvafxApi::instance().set_float(impl_->handle, NVAFX_PARAM_INTENSITY_RATIO, value) !=
          NVAFX_STATUS_SUCCESS) {
    throw std::runtime_error("NvAFX_SetFloat(aec intensity) failed");
  }
#endif
}

void NvafxAec::process(const std::span<const float> near_end,
                       const std::span<const float> far_end,
                       const std::span<float> output) {
#if !ECHONULL_HAS_NVAFX
  static_cast<void>(near_end);
  static_cast<void>(far_end);
  static_cast<void>(output);
  throw std::runtime_error("NvAFX is unavailable in this build");
#else
  std::scoped_lock lock(impl_->mutex);
  const auto current = impl_->status;
  if (!current.ready || impl_->handle == nullptr) {
    throw std::runtime_error("NvAFX AEC is not initialized");
  }
  if (near_end.size() != current.input_frame_samples ||
      far_end.size() != current.input_frame_samples ||
      output.size() != current.output_frame_samples) {
    throw std::runtime_error("NvAFX AEC frame size mismatch");
  }
  const float* input_buffers[2] = {near_end.data(), far_end.data()};
  float* output_buffers[1] = {output.data()};
  const NvAFX_Status result = NvafxApi::instance().run(
      impl_->handle, input_buffers, output_buffers,
      current.input_frame_samples, current.input_channels);
  if (result != NVAFX_STATUS_SUCCESS) {
    throw std::runtime_error("NvAFX_Run failed with status " +
                             std::to_string(static_cast<int>(result)));
  }
#endif
}

AecStatus NvafxAec::status() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->status;
}

}  // namespace echonull
