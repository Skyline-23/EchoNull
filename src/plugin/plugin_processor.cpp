#include "plugin/plugin_processor.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "application/delay_estimator.hpp"
#include "application/streaming_resampler.hpp"
#include "application/timestamped_audio_buffer.hpp"
#include "domain/audio_types.hpp"
#include "infrastructure/nvidia/nvafx_aec.hpp"
#include "infrastructure/nvidia/nvafx_denoiser.hpp"
#include "infrastructure/nvidia/packaged_runtime.hpp"
#include "infrastructure/windows/performance_clock.hpp"
#include "infrastructure/windows/realtime_audio_thread.hpp"
#include "infrastructure/windows/telemetry_bus.hpp"
#include "infrastructure/windows/wasapi_loopback.hpp"

namespace echonull {
namespace {

class FloatFifo {
 public:
  [[nodiscard]] std::size_t size() const noexcept {
    return samples_.size() - read_position_;
  }

  void append(const std::span<const float> samples) {
    compact_if_needed(samples.size());
    samples_.insert(samples_.end(), samples.begin(), samples.end());
  }

  void read_exact(const std::span<float> output) noexcept {
    std::copy_n(samples_.data() + read_position_, output.size(), output.data());
    consume(output.size());
  }

  [[nodiscard]] float at(const std::size_t offset) const noexcept {
    return samples_[read_position_ + offset];
  }

  void consume(const std::size_t count) noexcept {
    read_position_ += std::min(count, size());
    if (read_position_ == samples_.size()) clear();
  }

  void clear() noexcept {
    samples_.clear();
    read_position_ = 0;
  }

 private:
  void compact_if_needed(const std::size_t incoming) {
    if (read_position_ == 0 ||
        samples_.capacity() - samples_.size() >= incoming) {
      return;
    }
    std::move(samples_.begin() + static_cast<std::ptrdiff_t>(read_position_),
              samples_.end(), samples_.begin());
    samples_.resize(size());
    read_position_ = 0;
  }

  std::vector<float> samples_;
  std::size_t read_position_ = 0;
};

std::int64_t samples_to_hns(const double samples, const std::uint32_t sample_rate) {
  return static_cast<std::int64_t>(std::llround(
      samples * static_cast<double>(kHundredNanosecondsPerSecond) /
      static_cast<double>(sample_rate)));
}

std::int64_t milliseconds_to_hns(const double milliseconds) {
  return static_cast<std::int64_t>(std::llround(milliseconds * 10'000.0));
}

std::uint64_t overload_backoff_samples(const std::uint32_t sample_rate,
                                       std::uint32_t& level) noexcept {
  constexpr std::uint32_t kMaximumLevel = 4;
  const std::uint64_t base = std::max<std::uint32_t>(1, sample_rate / 4);
  const std::uint64_t duration = base << std::min(level, kMaximumLevel);
  level = std::min(level + 1, kMaximumLevel);
  return duration;
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

void downmix(const float* const* inputs, const std::int32_t sample_count,
             const std::uint32_t channel_count, std::vector<float>& mono) {
  mono.assign(static_cast<std::size_t>(sample_count), 0.0F);
  std::uint32_t active_channels = 0;
  for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
    if (inputs[channel] == nullptr) continue;
    ++active_channels;
    for (std::int32_t sample = 0; sample < sample_count; ++sample) {
      mono[static_cast<std::size_t>(sample)] += inputs[channel][sample];
    }
  }
  if (active_channels == 0) return;
  const float scale = 1.0F / static_cast<float>(active_channels);
  for (auto& sample : mono) sample *= scale;
}

}  // namespace

struct PluginProcessor::Impl {
  struct ReferenceBlock {
    std::int64_t timestamp_hns = 0;
    std::vector<float> samples;
  };

  std::unique_ptr<StreamingResampler> input_resampler;
  std::unique_ptr<StreamingResampler> output_resampler;
  std::unique_ptr<WasapiLoopbackCapture> loopback;
  std::unique_ptr<TelemetryBusWriter> telemetry_writer;
  std::unique_ptr<TimestampedAudioBuffer> reference_timeline;
  std::unique_ptr<DelayEstimator> delay_estimator;
  std::unique_ptr<NvafxAec> aec;
  std::unique_ptr<NvafxDenoiser> denoiser;
  std::mutex reference_mutex;
  std::deque<ReferenceBlock> pending_reference;
  std::deque<ReferenceBlock> drained_reference;
  FloatFifo near_queue;
  FloatFifo output_queue;
  std::vector<float> mono_scratch;
  std::vector<float> converted_scratch;
  std::vector<float> host_audio_scratch;
  std::vector<float> near_end_scratch;
  std::vector<float> unshifted_far_scratch;
  std::vector<float> far_end_scratch;
  std::vector<float> processed_scratch;
  std::vector<float> denoised_scratch;
  std::int64_t input_origin_hns = 0;
  std::int64_t near_queue_start_hns = 0;
  double delay_ms = 40.0;
  bool auto_delay = true;
  double max_delay_ms = 250.0;
  std::uint32_t timeline_capacity_ms = 4000;
  std::size_t aec_frame_samples = 0;
  std::uint64_t telemetry_frames_since_publish = 0;
  std::uint64_t aec_backoff_samples = 0;
  std::uint64_t noise_backoff_samples = 0;
  std::uint32_t aec_backoff_level = 0;
  std::uint32_t noise_backoff_level = 0;
  bool aec_ready = false;
  bool noise_ready = false;
};

PluginProcessor::PluginProcessor() : impl_(std::make_unique<Impl>()) {}
PluginProcessor::~PluginProcessor() { stop(); }

void PluginProcessor::set_reference_endpoint(std::wstring endpoint_id) {
  if (reference_endpoint_id_ == endpoint_id) return;
  stop();
  reference_endpoint_id_ = std::move(endpoint_id);
  if (!plugin_path_.empty()) start(plugin_path_);
}

void PluginProcessor::set_aec_enabled(const bool enabled) noexcept {
  aec_enabled_.store(enabled, std::memory_order_relaxed);
  if (!enabled) {
    runtime_state_.store(PluginRuntimeState::bypassed,
                         std::memory_order_relaxed);
  } else {
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
               : (impl_->noise_backoff_samples != 0
                      ? NoiseRuntimeState::overloaded
                      : (impl_->noise_ready ? NoiseRuntimeState::active
                                            : NoiseRuntimeState::error)),
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

    if (is_windows_audio_engine_process()) {
      try {
        impl_->telemetry_writer = std::make_unique<TelemetryBusWriter>();
        impl_->telemetry_writer->open();
      } catch (...) {
        impl_->telemetry_writer.reset();
      }
    }

    std::vector<std::filesystem::path> aec_models;
    std::vector<std::filesystem::path> noise_models;
    bool runtime_ready = false;
#if ECHONULL_HAS_NVAFX
    try {
      const auto packaged = PackagedRuntime::prepare(plugin_path_);
      aec_models = packaged.aec_models;
      noise_models = packaged.noise_models;
      runtime_ready = true;
    } catch (...) {
      runtime_ready = false;
    }
#endif
    impl_->delay_ms = 40.0;
    impl_->output_resampler =
        std::make_unique<StreamingResampler>(kSampleRate, sample_rate_);
    impl_->reference_timeline = std::make_unique<TimestampedAudioBuffer>(
        impl_->timeline_capacity_ms, kSampleRate);
    impl_->delay_estimator = std::make_unique<DelayEstimator>(
        kSampleRate, impl_->delay_ms, impl_->max_delay_ms);
    aec_error_reason_.store(
        !runtime_ready ? PluginErrorReason::runtime_or_gpu
        : !aec_models.empty()
            ? PluginErrorReason::none
            : PluginErrorReason::model_missing,
        std::memory_order_relaxed);
    impl_->aec = std::make_unique<NvafxAec>(
        aec_models,
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
        : !noise_models.empty()
            ? PluginErrorReason::none
            : PluginErrorReason::model_missing,
        std::memory_order_relaxed);
    impl_->denoiser = std::make_unique<NvafxDenoiser>(
        noise_models,
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
    const std::size_t scratch_frame_samples =
        impl_->aec_frame_samples == 0 ? 480 : impl_->aec_frame_samples;
    impl_->mono_scratch.reserve(std::max<std::size_t>(
        scratch_frame_samples, static_cast<std::size_t>(4096)));
    impl_->converted_scratch.reserve(4096);
    impl_->host_audio_scratch.reserve(4096);
    impl_->near_end_scratch.resize(scratch_frame_samples);
    impl_->unshifted_far_scratch.resize(scratch_frame_samples);
    impl_->far_end_scratch.resize(scratch_frame_samples);
    impl_->processed_scratch.resize(scratch_frame_samples);
    impl_->denoised_scratch.resize(scratch_frame_samples);
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

    if (!reference_endpoint_id_.empty()) {
      impl_->loopback =
          std::make_unique<WasapiLoopbackCapture>(reference_endpoint_id_);
      impl_->loopback->start(
          [this](const std::int64_t timestamp_hns,
                  std::vector<float> samples,
                  const bool discontinuity) {
            std::scoped_lock lock(impl_->reference_mutex);
            if (discontinuity) impl_->pending_reference.clear();
            impl_->pending_reference.push_back(
                Impl::ReferenceBlock{timestamp_hns, std::move(samples)});
            while (impl_->pending_reference.size() > 256) {
              impl_->pending_reference.pop_front();
            }
          });
    }
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
  impl_->loopback.reset();
  impl_->reference_timeline.reset();
  impl_->telemetry_writer.reset();
  impl_->delay_estimator.reset();
  impl_->aec.reset();
  impl_->denoiser.reset();
  impl_->near_queue.clear();
  impl_->output_queue.clear();
  {
    std::scoped_lock lock(impl_->reference_mutex);
    impl_->pending_reference.clear();
  }
  impl_->drained_reference.clear();
  impl_->input_origin_hns = 0;
  impl_->near_queue_start_hns = 0;
  impl_->aec_frame_samples = 0;
  impl_->telemetry_frames_since_publish = 0;
  impl_->aec_backoff_samples = 0;
  impl_->noise_backoff_samples = 0;
  impl_->aec_backoff_level = 0;
  impl_->noise_backoff_level = 0;
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
  ensure_realtime_audio_thread_priority();
  if (sample_count <= 0 || channel_count == 0) return;
  const auto update_meter = [&] {
    const float current = output_level_.load(std::memory_order_relaxed);
    const float peak = output_peak(outputs, sample_count, channel_count);
    const float block_seconds = static_cast<float>(sample_count) /
                                static_cast<float>(std::max(1U, sample_rate_));
    const float decay = std::exp(-block_seconds / 0.35F);
    const float level = peak >= current ? peak : current * decay;
    output_level_.store(level, std::memory_order_relaxed);
    impl_->telemetry_frames_since_publish +=
        static_cast<std::uint64_t>(sample_count);
    const std::uint64_t telemetry_interval =
        std::max<std::uint64_t>(1, sample_rate_ / 20);
    if (impl_->telemetry_writer &&
        impl_->telemetry_frames_since_publish >= telemetry_interval) {
      impl_->telemetry_frames_since_publish %= telemetry_interval;
      impl_->telemetry_writer->publish(TelemetrySnapshot{
          performance_timestamp_hns(), level,
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
    copy_input(inputs, outputs, sample_count, channel_count);
    update_meter();
    return;
  }

  if (!aec_enabled_.load(std::memory_order_relaxed) &&
      !noise_enabled_.load(std::memory_order_relaxed)) {
    copy_input(inputs, outputs, sample_count, channel_count);
    runtime_state_.store(PluginRuntimeState::bypassed,
                         std::memory_order_relaxed);
    update_meter();
    return;
  }

  try {
    if (impl_->input_origin_hns == 0) {
      impl_->input_origin_hns = performance_timestamp_hns();
    }
    downmix(inputs, sample_count, channel_count, impl_->mono_scratch);
    const double first_input_frame = impl_->input_resampler->push_into(
        impl_->mono_scratch, impl_->converted_scratch);
    const auto converted_timestamp = impl_->input_origin_hns +
        samples_to_hns(first_input_frame, sample_rate_);

    if (!impl_->converted_scratch.empty()) {
      if (impl_->near_queue.size() == 0) {
        impl_->near_queue_start_hns = converted_timestamp;
      }
      impl_->near_queue.append(impl_->converted_scratch);
    }
    if (impl_->reference_timeline) {
      impl_->drained_reference.clear();
      {
        std::scoped_lock lock(impl_->reference_mutex);
        impl_->drained_reference.swap(impl_->pending_reference);
      }
      for (auto& block : impl_->drained_reference) {
        impl_->reference_timeline->push(block.timestamp_hns,
                                        std::move(block.samples));
      }
    }

    const bool reference_failed = impl_->loopback && impl_->loopback->failed();
    if (reference_failed) {
      aec_error_reason_.store(PluginErrorReason::reference_capture,
                              std::memory_order_relaxed);
    }

    const std::size_t frame_samples = impl_->aec_frame_samples == 0
                                          ? 480
                                          : impl_->aec_frame_samples;
    auto& near_end = impl_->near_end_scratch;
    auto& unshifted_far = impl_->unshifted_far_scratch;
    auto& far_end = impl_->far_end_scratch;
    auto& processed = impl_->processed_scratch;
    auto& denoised = impl_->denoised_scratch;
    const std::int64_t frame_deadline_hns =
        samples_to_hns(static_cast<double>(frame_samples), kSampleRate);
    while (impl_->near_queue.size() >= frame_samples) {
      impl_->near_queue.read_exact(near_end);

      const bool aec_load_shed = impl_->aec_backoff_samples != 0;
      const bool noise_load_shed = impl_->noise_backoff_samples != 0;
      impl_->aec_backoff_samples =
          impl_->aec_backoff_samples > frame_samples
              ? impl_->aec_backoff_samples - frame_samples
              : 0;
      impl_->noise_backoff_samples =
          impl_->noise_backoff_samples > frame_samples
              ? impl_->noise_backoff_samples - frame_samples
              : 0;

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
      std::int64_t aec_elapsed_hns = 0;
      if (aec_enabled_.load(std::memory_order_relaxed) && !aec_load_shed &&
          far_coverage >= 0.98 && impl_->aec_ready && impl_->aec) {
        const std::int64_t aec_start_hns = performance_timestamp_hns();
        impl_->aec->process(near_end, far_end, processed);
        aec_elapsed_hns = performance_timestamp_hns() - aec_start_hns;
        runtime_state_.store(PluginRuntimeState::aec_active,
                             std::memory_order_relaxed);
        if (aec_elapsed_hns * 4 > frame_deadline_hns * 3) {
          impl_->aec_backoff_samples = overload_backoff_samples(
              kSampleRate, impl_->aec_backoff_level);
          runtime_state_.store(PluginRuntimeState::overloaded,
                               std::memory_order_relaxed);
        } else {
          impl_->aec_backoff_level = 0;
        }
      } else {
        processed = near_end;
        runtime_state_.store(
            aec_load_shed
                ? PluginRuntimeState::overloaded
                : (!aec_enabled_.load(std::memory_order_relaxed)
                       ? PluginRuntimeState::bypassed
                       : (reference_failed
                              ? PluginRuntimeState::error
                              : (impl_->aec_ready
                                     ? PluginRuntimeState::waiting_for_reference
                                     : PluginRuntimeState::error))),
            std::memory_order_relaxed);
      }

      if (noise_enabled_.load(std::memory_order_relaxed) &&
          !aec_load_shed && !noise_load_shed &&
          impl_->aec_backoff_samples == 0 && impl_->noise_ready &&
          impl_->denoiser) {
        try {
          const std::int64_t noise_start_hns = performance_timestamp_hns();
          impl_->denoiser->process(processed, denoised);
          const std::int64_t noise_elapsed_hns =
              performance_timestamp_hns() - noise_start_hns;
          processed.swap(denoised);
          if ((aec_elapsed_hns + noise_elapsed_hns) * 5 >
              frame_deadline_hns * 4) {
            impl_->noise_backoff_samples = overload_backoff_samples(
                kSampleRate, impl_->noise_backoff_level);
            noise_runtime_state_.store(NoiseRuntimeState::overloaded,
                                       std::memory_order_relaxed);
          } else {
            impl_->noise_backoff_level = 0;
            noise_runtime_state_.store(NoiseRuntimeState::active,
                                       std::memory_order_relaxed);
          }
        } catch (...) {
          impl_->noise_ready = false;
          noise_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                    std::memory_order_relaxed);
          noise_runtime_state_.store(NoiseRuntimeState::error,
                                     std::memory_order_relaxed);
        }
      } else if (noise_enabled_.load(std::memory_order_relaxed) &&
                 impl_->noise_ready &&
                 (aec_load_shed || noise_load_shed ||
                  impl_->aec_backoff_samples != 0)) {
        noise_runtime_state_.store(NoiseRuntimeState::overloaded,
                                   std::memory_order_relaxed);
      }

      if (impl_->output_resampler) {
        impl_->output_resampler->push_into(processed,
                                           impl_->host_audio_scratch);
        impl_->output_queue.append(impl_->host_audio_scratch);
      }
      impl_->near_queue_start_hns +=
          samples_to_hns(static_cast<double>(frame_samples), kSampleRate);
      if (impl_->reference_timeline) {
        impl_->reference_timeline->discard_before(
            impl_->near_queue_start_hns -
            milliseconds_to_hns(impl_->max_delay_ms + 500.0));
      }
    }

    const std::size_t available_output = std::min(
        static_cast<std::size_t>(sample_count), impl_->output_queue.size());
    for (std::int32_t sample = 0; sample < sample_count; ++sample) {
      const auto index = static_cast<std::size_t>(sample);
      const float value =
          index < available_output ? impl_->output_queue.at(index) : 0.0F;
      for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
        if (outputs[channel] != nullptr) outputs[channel][sample] = value;
      }
    }
    impl_->output_queue.consume(available_output);
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
