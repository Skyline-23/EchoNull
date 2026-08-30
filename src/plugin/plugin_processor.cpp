#include "plugin/plugin_processor.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "application/delay_estimator.hpp"
#include "application/streaming_resampler.hpp"
#include "application/timestamped_audio_buffer.hpp"
#include "domain/audio_types.hpp"
#include "infrastructure/nvidia/nvafx_aec.hpp"
#include "infrastructure/nvidia/nvafx_denoiser.hpp"
#include "infrastructure/nvidia/packaged_runtime.hpp"
#include "infrastructure/windows/reference_bus.hpp"
#include "infrastructure/windows/telemetry_bus.hpp"

namespace echonull {
namespace {

std::int64_t qpc_hns() {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<std::int64_t>(
      static_cast<long double>(counter.QuadPart) * kHundredNanosecondsPerSecond /
      static_cast<long double>(frequency.QuadPart));
}

std::int64_t samples_to_hns(const double samples, const std::uint32_t sample_rate) {
  return static_cast<std::int64_t>(std::llround(
      samples * static_cast<double>(kHundredNanosecondsPerSecond) /
      static_cast<double>(sample_rate)));
}

std::int64_t milliseconds_to_hns(const double milliseconds) {
  return static_cast<std::int64_t>(std::llround(milliseconds * 10'000.0));
}

void copy_input(const float* const* inputs, float** outputs,
                const std::int32_t sample_count, const std::uint32_t channel_count) {
  for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
    if (outputs[channel] == nullptr) continue;
    if (inputs[channel] == nullptr) {
      std::fill_n(outputs[channel], sample_count, 0.0F);
    } else if (outputs[channel] != inputs[channel]) {
      std::copy_n(inputs[channel], sample_count, outputs[channel]);
    }
  }
}

float output_peak(float** outputs, const std::int32_t sample_count,
                  const std::uint32_t channel_count) {
  float peak = 0.0F;
  for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
    if (outputs[channel] == nullptr) continue;
    for (std::int32_t sample = 0; sample < sample_count; ++sample) {
      peak = std::max(peak, std::abs(outputs[channel][sample]));
    }
  }
  return std::min(peak, 1.0F);
}

std::vector<float> downmix(const float* const* inputs,
                           const std::int32_t sample_count,
                           const std::uint32_t channel_count) {
  std::vector<float> mono(static_cast<std::size_t>(sample_count), 0.0F);
  if (channel_count == 0 || inputs[0] == nullptr) return mono;
  for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
    if (inputs[channel] == nullptr) continue;
    for (std::int32_t sample = 0; sample < sample_count; ++sample) {
      mono[static_cast<std::size_t>(sample)] += inputs[channel][sample];
    }
  }
  const float scale = 1.0F / static_cast<float>(channel_count);
  for (auto& sample : mono) sample *= scale;
  return mono;
}

}  // namespace

struct PluginProcessor::Impl {
  std::unique_ptr<StreamingResampler> input_resampler;
  std::unique_ptr<StreamingResampler> output_resampler;
  std::unique_ptr<ReferenceBusWriter> reference_writer;
  std::unique_ptr<ReferenceBusReader> reference_reader;
  std::unique_ptr<TelemetryBusWriter> telemetry_writer;
  std::unique_ptr<TimestampedAudioBuffer> reference_timeline;
  std::unique_ptr<DelayEstimator> delay_estimator;
  std::unique_ptr<NvafxAec> aec;
  std::unique_ptr<NvafxDenoiser> denoiser;
  std::deque<float> near_queue;
  std::deque<float> output_queue;
  std::int64_t input_origin_hns = 0;
  std::int64_t near_queue_start_hns = 0;
  double delay_ms = 40.0;
  bool auto_delay = true;
  double max_delay_ms = 250.0;
  std::uint32_t timeline_capacity_ms = 4000;
  std::size_t aec_frame_samples = 0;
  bool aec_ready = false;
  bool noise_ready = false;
};

PluginProcessor::PluginProcessor() : impl_(std::make_unique<Impl>()) {}
PluginProcessor::~PluginProcessor() { stop(); }

void PluginProcessor::set_mode(const PluginMode mode) {
  if (mode_ == mode) return;
  stop();
  mode_ = mode;
  if (!plugin_path_.empty()) start(plugin_path_);
}

void PluginProcessor::set_aec_enabled(const bool enabled) noexcept {
  aec_enabled_.store(enabled, std::memory_order_relaxed);
  if (!enabled && mode_ == PluginMode::aec) {
    runtime_state_.store(PluginRuntimeState::bypassed,
                         std::memory_order_relaxed);
  } else if (enabled && mode_ == PluginMode::aec) {
    runtime_state_.store(impl_->aec_ready ? PluginRuntimeState::waiting_for_reference
                                          : PluginRuntimeState::error,
                         std::memory_order_relaxed);
  }
}

void PluginProcessor::set_aec_strength(const float strength) noexcept {
  const float value = std::clamp(strength, 0.0F, 1.0F);
  aec_strength_.store(value, std::memory_order_relaxed);
  if (impl_->aec) {
    try {
      impl_->aec->set_intensity(value);
    } catch (...) {
      runtime_state_.store(PluginRuntimeState::error, std::memory_order_relaxed);
      aec_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                              std::memory_order_relaxed);
    }
  }
}

void PluginProcessor::set_noise_enabled(const bool enabled) noexcept {
  noise_enabled_.store(enabled, std::memory_order_relaxed);
  noise_runtime_state_.store(
      !enabled ? NoiseRuntimeState::disabled
               : (impl_->noise_ready ? NoiseRuntimeState::active
                                     : NoiseRuntimeState::error),
      std::memory_order_relaxed);
}

void PluginProcessor::set_noise_strength(const float strength) noexcept {
  const float value = std::clamp(strength, 0.0F, 1.0F);
  noise_strength_.store(value, std::memory_order_relaxed);
  if (impl_->denoiser) {
    try {
      impl_->denoiser->set_intensity(value);
    } catch (...) {
      impl_->noise_ready = false;
      noise_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                std::memory_order_relaxed);
      noise_runtime_state_.store(NoiseRuntimeState::error,
                                 std::memory_order_relaxed);
    }
  }
}

void PluginProcessor::set_sample_rate(const std::uint32_t sample_rate) {
  if (sample_rate == 0 || sample_rate_ == sample_rate) return;
  stop();
  sample_rate_ = sample_rate;
  if (!plugin_path_.empty()) start(plugin_path_);
}

void PluginProcessor::start(const std::filesystem::path& plugin_path) {
  stop();
  plugin_path_ = plugin_path;
  runtime_state_.store(PluginRuntimeState::idle, std::memory_order_relaxed);
  try {
    impl_->input_resampler =
        std::make_unique<StreamingResampler>(sample_rate_, kSampleRate);
    impl_->input_origin_hns = 0;

    if (mode_ == PluginMode::reference) {
      impl_->reference_writer = std::make_unique<ReferenceBusWriter>();
      impl_->reference_writer->open();
      runtime_state_.store(PluginRuntimeState::reference_active,
                           std::memory_order_relaxed);
      return;
    }

    if (is_windows_audio_engine_process()) {
      try {
        impl_->telemetry_writer = std::make_unique<TelemetryBusWriter>();
        impl_->telemetry_writer->open();
      } catch (...) {
        impl_->telemetry_writer.reset();
      }
    }

    std::filesystem::path aec_model;
    std::filesystem::path noise_model;
    bool runtime_ready = false;
#if ECHONULL_HAS_NVAFX
    try {
      const auto packaged = PackagedRuntime::prepare(plugin_path_);
      aec_model = packaged.aec_model;
      noise_model = packaged.noise_model;
      runtime_ready = true;
    } catch (...) {
      runtime_ready = false;
    }
#endif
    impl_->delay_ms = 40.0;
    impl_->output_resampler =
        std::make_unique<StreamingResampler>(kSampleRate, sample_rate_);
    impl_->reference_reader = std::make_unique<ReferenceBusReader>();
    impl_->reference_timeline = std::make_unique<TimestampedAudioBuffer>(
        impl_->timeline_capacity_ms, kSampleRate);
    impl_->delay_estimator = std::make_unique<DelayEstimator>(
        kSampleRate, impl_->delay_ms, impl_->max_delay_ms);
    aec_error_reason_.store(
        !runtime_ready ? PluginErrorReason::runtime_or_gpu
        : std::filesystem::is_regular_file(aec_model)
            ? PluginErrorReason::none
            : PluginErrorReason::model_missing,
        std::memory_order_relaxed);
    impl_->aec = std::make_unique<NvafxAec>(
        aec_model,
        aec_strength_.load(std::memory_order_relaxed), kSampleRate);
    try {
      impl_->aec->initialize();
      const auto status = impl_->aec->status();
      impl_->aec_frame_samples = status.input_frame_samples;
      impl_->aec_ready = status.ready && status.input_frame_samples != 0 &&
                         status.input_frame_samples == status.output_frame_samples;
      if (impl_->aec_ready) {
        aec_error_reason_.store(PluginErrorReason::none,
                                std::memory_order_relaxed);
      }
    } catch (...) {
      impl_->aec_ready = false;
      impl_->aec_frame_samples = 480;
      if (aec_error_reason_.load(std::memory_order_relaxed) ==
          PluginErrorReason::none) {
        aec_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                std::memory_order_relaxed);
      }
    }

    noise_error_reason_.store(
        !runtime_ready ? PluginErrorReason::runtime_or_gpu
        : std::filesystem::is_regular_file(noise_model)
            ? PluginErrorReason::none
            : PluginErrorReason::model_missing,
        std::memory_order_relaxed);
    impl_->denoiser = std::make_unique<NvafxDenoiser>(
        noise_model,
        noise_strength_.load(std::memory_order_relaxed), kSampleRate);
    try {
      impl_->denoiser->initialize();
      const auto noise_status = impl_->denoiser->status();
      impl_->noise_ready = noise_status.ready &&
                           noise_status.input_frame_samples ==
                               impl_->aec_frame_samples &&
                           noise_status.output_frame_samples ==
                               impl_->aec_frame_samples;
      if (impl_->noise_ready) {
        noise_error_reason_.store(PluginErrorReason::none,
                                  std::memory_order_relaxed);
      }
    } catch (...) {
      impl_->noise_ready = false;
      if (noise_error_reason_.load(std::memory_order_relaxed) ==
          PluginErrorReason::none) {
        noise_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                  std::memory_order_relaxed);
      }
    }
    noise_runtime_state_.store(
        !noise_enabled_.load(std::memory_order_relaxed)
            ? NoiseRuntimeState::disabled
            : (impl_->noise_ready ? NoiseRuntimeState::active
                                  : NoiseRuntimeState::error),
        std::memory_order_relaxed);
    runtime_state_.store(
        !aec_enabled_.load(std::memory_order_relaxed)
            ? PluginRuntimeState::bypassed
                      : (impl_->aec_ready
                             ? PluginRuntimeState::waiting_for_reference
                             : PluginRuntimeState::error),
        std::memory_order_relaxed);
  } catch (...) {
    stop();
    aec_error_reason_.store(PluginErrorReason::package,
                            std::memory_order_relaxed);
    noise_error_reason_.store(PluginErrorReason::package,
                              std::memory_order_relaxed);
    runtime_state_.store(PluginRuntimeState::error, std::memory_order_relaxed);
    throw;
  }
}

void PluginProcessor::stop() noexcept {
  if (impl_->aec) {
    try { impl_->aec->reset(); } catch (...) {}
  }
  impl_->input_resampler.reset();
  impl_->output_resampler.reset();
  impl_->reference_writer.reset();
  impl_->reference_reader.reset();
  impl_->reference_timeline.reset();
  impl_->telemetry_writer.reset();
  impl_->delay_estimator.reset();
  impl_->aec.reset();
  impl_->denoiser.reset();
  impl_->near_queue.clear();
  impl_->output_queue.clear();
  impl_->input_origin_hns = 0;
  impl_->near_queue_start_hns = 0;
  impl_->aec_frame_samples = 0;
  impl_->aec_ready = false;
  impl_->noise_ready = false;
  output_level_.store(0.0F, std::memory_order_relaxed);
  runtime_state_.store(PluginRuntimeState::idle, std::memory_order_relaxed);
  noise_runtime_state_.store(NoiseRuntimeState::disabled,
                             std::memory_order_relaxed);
  aec_error_reason_.store(PluginErrorReason::none, std::memory_order_relaxed);
  noise_error_reason_.store(PluginErrorReason::none, std::memory_order_relaxed);
}

void PluginProcessor::process(const float* const* inputs, float** outputs,
                              const std::int32_t sample_count,
                              const std::uint32_t channel_count) noexcept {
  if (sample_count <= 0 || channel_count == 0) return;
  copy_input(inputs, outputs, sample_count, channel_count);
  const auto update_meter = [&] {
    const float current = output_level_.load(std::memory_order_relaxed);
    const float level =
        std::max(output_peak(outputs, sample_count, channel_count), current * 0.82F);
    output_level_.store(level, std::memory_order_relaxed);
    if (impl_->telemetry_writer) {
      impl_->telemetry_writer->publish(TelemetrySnapshot{
          qpc_hns(), level,
          static_cast<std::uint32_t>(runtime_state_.load(std::memory_order_relaxed)),
          static_cast<std::uint32_t>(
              noise_runtime_state_.load(std::memory_order_relaxed)),
          static_cast<std::uint32_t>(
              aec_error_reason_.load(std::memory_order_relaxed)),
          static_cast<std::uint32_t>(
              noise_error_reason_.load(std::memory_order_relaxed))});
    }
  };
  if (!impl_->input_resampler) {
    update_meter();
    return;
  }

  if (mode_ == PluginMode::aec &&
      !aec_enabled_.load(std::memory_order_relaxed) &&
      !noise_enabled_.load(std::memory_order_relaxed)) {
    runtime_state_.store(PluginRuntimeState::bypassed,
                         std::memory_order_relaxed);
    update_meter();
    return;
  }

  try {
    if (impl_->input_origin_hns == 0) impl_->input_origin_hns = qpc_hns();
    const auto mono = downmix(inputs, sample_count, channel_count);
    auto converted = impl_->input_resampler->push(mono);
    const auto converted_timestamp = impl_->input_origin_hns +
        samples_to_hns(converted.first_input_frame, sample_rate_);

    if (mode_ == PluginMode::reference) {
      if (impl_->reference_writer && !converted.samples.empty()) {
        impl_->reference_writer->publish(converted_timestamp, converted.samples);
      }
      runtime_state_.store(PluginRuntimeState::reference_active,
                           std::memory_order_relaxed);
      update_meter();
      return;
    }

    if (!converted.samples.empty()) {
      if (impl_->near_queue.empty()) impl_->near_queue_start_hns = converted_timestamp;
      impl_->near_queue.insert(impl_->near_queue.end(),
                               converted.samples.begin(), converted.samples.end());
    }
    if (impl_->reference_reader && impl_->reference_timeline) {
      for (auto& block : impl_->reference_reader->read_available()) {
        impl_->reference_timeline->push(block.timestamp_hns, block.samples);
      }
    }

    const std::size_t frame_samples = impl_->aec_frame_samples == 0
                                          ? 480
                                          : impl_->aec_frame_samples;
    std::vector<float> near_end(frame_samples);
    std::vector<float> unshifted_far(frame_samples);
    std::vector<float> far_end(frame_samples);
    std::vector<float> processed(frame_samples);
    std::vector<float> denoised(frame_samples);
    while (impl_->near_queue.size() >= frame_samples) {
      for (std::size_t index = 0; index < frame_samples; ++index) {
        near_end[index] = impl_->near_queue.front();
        impl_->near_queue.pop_front();
      }

      double unshifted_coverage = 0.0;
      if (impl_->reference_timeline) {
        unshifted_coverage = impl_->reference_timeline->read(
            impl_->near_queue_start_hns, unshifted_far);
      }
      if (unshifted_coverage >= 0.98 && impl_->auto_delay &&
          impl_->delay_estimator) {
        impl_->delay_estimator->add(near_end, unshifted_far);
        if (impl_->delay_estimator->has_estimate()) {
          impl_->delay_ms = impl_->delay_estimator->delay_ms();
        }
      }

      double far_coverage = 0.0;
      if (impl_->reference_timeline) {
        far_coverage = impl_->reference_timeline->read(
            impl_->near_queue_start_hns - milliseconds_to_hns(impl_->delay_ms), far_end);
      }
      if (aec_enabled_.load(std::memory_order_relaxed) &&
          far_coverage >= 0.98 && impl_->aec_ready && impl_->aec) {
        impl_->aec->process(near_end, far_end, processed);
        runtime_state_.store(PluginRuntimeState::aec_active,
                             std::memory_order_relaxed);
      } else {
        processed = near_end;
        runtime_state_.store(
            !aec_enabled_.load(std::memory_order_relaxed)
                ? PluginRuntimeState::bypassed
                : (impl_->aec_ready ? PluginRuntimeState::waiting_for_reference
                                    : PluginRuntimeState::error),
            std::memory_order_relaxed);
      }

      if (noise_enabled_.load(std::memory_order_relaxed) &&
          impl_->noise_ready && impl_->denoiser) {
        try {
          impl_->denoiser->process(processed, denoised);
          processed.swap(denoised);
          noise_runtime_state_.store(NoiseRuntimeState::active,
                                     std::memory_order_relaxed);
        } catch (...) {
          impl_->noise_ready = false;
          noise_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                    std::memory_order_relaxed);
          noise_runtime_state_.store(NoiseRuntimeState::error,
                                     std::memory_order_relaxed);
        }
      }

      if (impl_->output_resampler) {
        auto host_audio = impl_->output_resampler->push(processed);
        impl_->output_queue.insert(impl_->output_queue.end(),
                                   host_audio.samples.begin(), host_audio.samples.end());
      }
      impl_->near_queue_start_hns +=
          samples_to_hns(static_cast<double>(frame_samples), kSampleRate);
      if (impl_->reference_timeline) {
        impl_->reference_timeline->discard_before(
            impl_->near_queue_start_hns -
            milliseconds_to_hns(impl_->max_delay_ms + 500.0));
      }
    }

    for (std::int32_t sample = 0; sample < sample_count; ++sample) {
      const float value = impl_->output_queue.empty() ? 0.0F : impl_->output_queue.front();
      if (!impl_->output_queue.empty()) impl_->output_queue.pop_front();
      for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
        if (outputs[channel] != nullptr) outputs[channel][sample] = value;
      }
    }
    update_meter();
  } catch (...) {
    copy_input(inputs, outputs, sample_count, channel_count);
    runtime_state_.store(PluginRuntimeState::error, std::memory_order_relaxed);
    aec_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                            std::memory_order_relaxed);
    update_meter();
  }
}

}  // namespace echonull
