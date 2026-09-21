#include "plugin/plugin_processor.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "application/delay_estimator.hpp"
#include "application/output_transition.hpp"
#include "application/streaming_resampler.hpp"
#include "application/timestamped_audio_buffer.hpp"
#include "domain/audio_types.hpp"
#include "infrastructure/nvidia/nvafx_aec.hpp"
#include "infrastructure/nvidia/nvafx_denoiser.hpp"
#include "infrastructure/nvidia/packaged_runtime.hpp"
#include "infrastructure/windows/diagnostic_log.hpp"
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

  void reserve(const std::size_t sample_count) { samples_.reserve(sample_count); }

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

constexpr std::size_t kAsyncQueueCapacity = 16;
constexpr std::size_t kWorkerLatencyFrames = 4;
constexpr std::size_t kOutputLeadFrames = 6;
constexpr std::uint64_t kOverloadBypassFrames = 200;  // 2 seconds at 10 ms/frame.

struct WorkFrame {
  std::uint64_t sequence = 0;
  bool run_aec = false;
  bool run_noise = false;
  std::vector<float> near_end;
  std::vector<float> far_end;
};

struct ResultFrame {
  std::uint64_t sequence = 0;
  bool effect_applied = false;
  std::vector<float> samples;
};

template <typename Frame>
class SpscFrameQueue {
 public:
  void configure(const std::size_t frame_samples) {
    slots_.clear();
    slots_.resize(kAsyncQueueCapacity + 1);
    for (auto& slot : slots_) {
      if constexpr (std::is_same_v<Frame, WorkFrame>) {
        slot.near_end.resize(frame_samples);
        slot.far_end.resize(frame_samples);
      } else {
        slot.samples.resize(frame_samples);
      }
    }
    read_.store(0, std::memory_order_relaxed);
    write_.store(0, std::memory_order_relaxed);
  }

  [[nodiscard]] bool try_push_work(const std::uint64_t sequence,
                                   const std::span<const float> near_end,
                                   const std::span<const float> far_end,
                                   const bool run_aec,
                                   const bool run_noise) noexcept
    requires std::is_same_v<Frame, WorkFrame>
  {
    const auto write = write_.load(std::memory_order_relaxed);
    const auto next = increment(write);
    if (next == read_.load(std::memory_order_acquire)) return false;
    auto& slot = slots_[write];
    slot.sequence = sequence;
    slot.run_aec = run_aec;
    slot.run_noise = run_noise;
    std::copy(near_end.begin(), near_end.end(), slot.near_end.begin());
    std::copy(far_end.begin(), far_end.end(), slot.far_end.begin());
    write_.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool try_push_result(const std::uint64_t sequence,
                                     const std::span<const float> samples,
                                     const bool effect_applied) noexcept
    requires std::is_same_v<Frame, ResultFrame>
  {
    const auto write = write_.load(std::memory_order_relaxed);
    const auto next = increment(write);
    if (next == read_.load(std::memory_order_acquire)) return false;
    auto& slot = slots_[write];
    slot.sequence = sequence;
    slot.effect_applied = effect_applied;
    std::copy(samples.begin(), samples.end(), slot.samples.begin());
    write_.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] Frame* try_front() noexcept {
    const auto read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) return nullptr;
    return &slots_[read];
  }

  void pop() noexcept {
    const auto read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) return;
    read_.store(increment(read), std::memory_order_release);
  }

 private:
  [[nodiscard]] std::size_t increment(const std::size_t index) const noexcept {
    return (index + 1) % slots_.size();
  }

  std::vector<Frame> slots_;
  std::atomic<std::size_t> read_{0};
  std::atomic<std::size_t> write_{0};
};

class DelayedDryFrames {
 public:
  struct Frame {
    std::uint64_t sequence = 0;
    bool expects_result = false;
    std::vector<float> samples;
  };

  void configure(const std::size_t frame_samples) {
    frames_.clear();
    frames_.resize(kWorkerLatencyFrames + 2);
    for (auto& frame : frames_) frame.samples.resize(frame_samples);
    clear();
  }

  void push(const std::uint64_t sequence,
            const std::span<const float> samples,
            const bool expects_result) noexcept {
    auto& frame = frames_[(head_ + size_) % frames_.size()];
    frame.sequence = sequence;
    frame.expects_result = expects_result;
    std::copy(samples.begin(), samples.end(), frame.samples.begin());
    ++size_;
  }

  [[nodiscard]] Frame& front() noexcept { return frames_[head_]; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  void pop() noexcept {
    if (size_ == 0) return;
    head_ = (head_ + 1) % frames_.size();
    --size_;
  }

  void clear() noexcept {
    head_ = 0;
    size_ = 0;
  }

 private:
  std::vector<Frame> frames_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

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
  std::unique_ptr<AsyncDiagnosticLog> diagnostic_log;
  std::unique_ptr<TimestampedAudioBuffer> reference_timeline;
  std::unique_ptr<DelayEstimator> delay_estimator;
  std::unique_ptr<NvafxAec> aec;
  std::unique_ptr<NvafxDenoiser> denoiser;
  SpscFrameQueue<WorkFrame> work_queue;
  SpscFrameQueue<ResultFrame> result_queue;
  DelayedDryFrames delayed_dry;
  OutputTransition output_transition;
  std::thread gpu_worker;
  HANDLE gpu_wake_event = nullptr;
  HANDLE gpu_initialized_event = nullptr;
  std::atomic<bool> gpu_worker_stop{false};
  std::atomic<std::uint64_t> minimum_useful_sequence{1};
  std::atomic<std::uint64_t> fallback_frames{0};
  std::atomic<std::uint64_t> gpu_deadline_misses{0};
  std::atomic<std::uint64_t> queue_overruns{0};
  std::atomic<std::uint64_t> output_underrun_samples{0};
  std::atomic<std::uint64_t> aec_shed_until_sequence{0};
  std::atomic<std::uint64_t> noise_shed_until_sequence{0};
  std::atomic<bool> aec_ready{false};
  std::atomic<bool> noise_ready{false};
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
  std::vector<float> worker_processed_scratch;
  std::vector<float> worker_denoised_scratch;
  std::vector<float> startup_silence;
  std::vector<float> transition_scratch;
  std::int64_t input_origin_hns = 0;
  std::int64_t near_queue_start_hns = 0;
  double delay_ms = 40.0;
  bool auto_delay = true;
  double max_delay_ms = 250.0;
  std::uint32_t timeline_capacity_ms = 4000;
  std::size_t aec_frame_samples = 0;
  std::uint64_t telemetry_frames_since_publish = 0;
  std::uint64_t next_sequence = 1;
  std::uint64_t overload_hold_samples = 0;
  std::uint64_t dry_bypass_until_sequence = 0;
  float last_output_sample = 0.0F;
  bool pipeline_active = false;
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
    runtime_state_.store(impl_->aec_ready.load(std::memory_order_relaxed)
                             ? PluginRuntimeState::waiting_for_reference
                             : PluginRuntimeState::error,
                         std::memory_order_relaxed);
  }
}

void PluginProcessor::set_aec_strength(const float strength) noexcept {
  const float value = std::clamp(strength, 0.0F, 1.0F);
  aec_strength_.store(value, std::memory_order_relaxed);
}

void PluginProcessor::set_noise_enabled(const bool enabled) noexcept {
  noise_enabled_.store(enabled, std::memory_order_relaxed);
  noise_runtime_state_.store(
      !enabled ? NoiseRuntimeState::disabled
               : (impl_->overload_hold_samples != 0
                      ? NoiseRuntimeState::overloaded
                      : (impl_->noise_ready.load(std::memory_order_relaxed)
                             ? NoiseRuntimeState::active
                             : NoiseRuntimeState::error)),
      std::memory_order_relaxed);
}

void PluginProcessor::set_noise_strength(const float strength) noexcept {
  const float value = std::clamp(strength, 0.0F, 1.0F);
  noise_strength_.store(value, std::memory_order_relaxed);
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
      try {
        impl_->diagnostic_log = std::make_unique<AsyncDiagnosticLog>();
        impl_->diagnostic_log->open();
      } catch (...) {
        impl_->diagnostic_log.reset();
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
    noise_error_reason_.store(
        !runtime_ready ? PluginErrorReason::runtime_or_gpu
        : !noise_models.empty()
            ? PluginErrorReason::none
            : PluginErrorReason::model_missing,
        std::memory_order_relaxed);
    impl_->denoiser = std::make_unique<NvafxDenoiser>(
        noise_models,
        noise_strength_.load(std::memory_order_relaxed), kSampleRate);
    impl_->gpu_wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->gpu_initialized_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl_->gpu_wake_event == nullptr ||
        impl_->gpu_initialized_event == nullptr) {
      throw std::runtime_error("could not create the GPU worker events");
    }
    impl_->gpu_worker_stop.store(false, std::memory_order_relaxed);
    impl_->gpu_worker = std::thread([this] {
      ensure_realtime_audio_thread_priority();
      impl_->aec_frame_samples = 480;
      try {
        impl_->aec->initialize();
        const auto status = impl_->aec->status();
        impl_->aec_frame_samples = status.input_frame_samples == 0
                                       ? 480
                                       : status.input_frame_samples;
        const bool ready = status.ready && status.input_frame_samples != 0 &&
                           status.input_frame_samples ==
                               status.output_frame_samples;
        impl_->aec_ready.store(ready, std::memory_order_release);
        if (ready) {
          aec_error_reason_.store(PluginErrorReason::none,
                                  std::memory_order_relaxed);
        }
      } catch (...) {
        impl_->aec_ready.store(false, std::memory_order_release);
        if (aec_error_reason_.load(std::memory_order_relaxed) ==
            PluginErrorReason::none) {
          aec_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                  std::memory_order_relaxed);
        }
      }

      try {
        impl_->denoiser->initialize();
        const auto status = impl_->denoiser->status();
        const bool ready = status.ready &&
                           status.input_frame_samples ==
                               impl_->aec_frame_samples &&
                           status.output_frame_samples ==
                               impl_->aec_frame_samples;
        impl_->noise_ready.store(ready, std::memory_order_release);
        if (ready) {
          noise_error_reason_.store(PluginErrorReason::none,
                                    std::memory_order_relaxed);
        }
      } catch (...) {
        impl_->noise_ready.store(false, std::memory_order_release);
        if (noise_error_reason_.load(std::memory_order_relaxed) ==
            PluginErrorReason::none) {
          noise_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                    std::memory_order_relaxed);
        }
      }

      impl_->worker_processed_scratch.resize(impl_->aec_frame_samples);
      impl_->worker_denoised_scratch.resize(impl_->aec_frame_samples);
      SetEvent(impl_->gpu_initialized_event);

      float applied_aec_strength =
          aec_strength_.load(std::memory_order_relaxed);
      float applied_noise_strength =
          noise_strength_.load(std::memory_order_relaxed);
      const auto frame_deadline_hns = samples_to_hns(
          static_cast<double>(impl_->aec_frame_samples), kSampleRate);
      while (!impl_->gpu_worker_stop.load(std::memory_order_acquire)) {
        WaitForSingleObject(impl_->gpu_wake_event, 20);
        if (impl_->gpu_worker_stop.load(std::memory_order_acquire)) break;
        while (auto* job = impl_->work_queue.try_front()) {
          const auto sequence = job->sequence;
          if (sequence < impl_->minimum_useful_sequence.load(
                             std::memory_order_acquire)) {
            impl_->work_queue.pop();
            continue;
          }

          auto& processed = impl_->worker_processed_scratch;
          auto& denoised = impl_->worker_denoised_scratch;
          std::copy(job->near_end.begin(), job->near_end.end(),
                    processed.begin());
          const auto started_hns = performance_timestamp_hns();
          std::int64_t aec_elapsed_hns = 0;
          bool effect_applied = false;

          const float desired_aec =
              aec_strength_.load(std::memory_order_relaxed);
          if (desired_aec != applied_aec_strength &&
              impl_->aec_ready.load(std::memory_order_acquire)) {
            try {
              impl_->aec->set_intensity(desired_aec);
              applied_aec_strength = desired_aec;
            } catch (...) {
              impl_->aec_ready.store(false, std::memory_order_release);
              aec_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                      std::memory_order_relaxed);
            }
          }
          if (job->run_aec &&
              sequence >= impl_->aec_shed_until_sequence.load(
                              std::memory_order_relaxed) &&
              impl_->aec_ready.load(std::memory_order_acquire)) {
            try {
              const auto aec_started_hns = performance_timestamp_hns();
              impl_->aec->process(job->near_end, job->far_end, processed);
              effect_applied = true;
              aec_elapsed_hns =
                  performance_timestamp_hns() - aec_started_hns;
            } catch (...) {
              std::copy(job->near_end.begin(), job->near_end.end(),
                        processed.begin());
              impl_->aec_ready.store(false, std::memory_order_release);
              aec_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                      std::memory_order_relaxed);
            }
          }

          const float desired_noise =
              noise_strength_.load(std::memory_order_relaxed);
          if (desired_noise != applied_noise_strength &&
              impl_->noise_ready.load(std::memory_order_acquire)) {
            try {
              impl_->denoiser->set_intensity(desired_noise);
              applied_noise_strength = desired_noise;
            } catch (...) {
              impl_->noise_ready.store(false, std::memory_order_release);
              noise_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                        std::memory_order_relaxed);
            }
          }
          if (job->run_noise &&
              sequence >= impl_->noise_shed_until_sequence.load(
                              std::memory_order_relaxed) &&
              impl_->noise_ready.load(std::memory_order_acquire)) {
            try {
              impl_->denoiser->process(processed, denoised);
              processed.swap(denoised);
              effect_applied = true;
            } catch (...) {
              impl_->noise_ready.store(false, std::memory_order_release);
              noise_error_reason_.store(PluginErrorReason::runtime_or_gpu,
                                        std::memory_order_relaxed);
            }
          }

          const auto total_elapsed_hns =
              performance_timestamp_hns() - started_hns;
          if (total_elapsed_hns * 5 > frame_deadline_hns * 4) {
            impl_->noise_shed_until_sequence.store(sequence + 50,
                                                   std::memory_order_relaxed);
          }
          if (aec_elapsed_hns > frame_deadline_hns) {
            impl_->aec_shed_until_sequence.store(sequence + 25,
                                                 std::memory_order_relaxed);
          }
          if (total_elapsed_hns > frame_deadline_hns) {
            impl_->gpu_deadline_misses.fetch_add(1,
                                                 std::memory_order_relaxed);
          }
          if (sequence >= impl_->minimum_useful_sequence.load(
                              std::memory_order_acquire) &&
              !impl_->result_queue.try_push_result(sequence, processed,
                                                   effect_applied)) {
            impl_->queue_overruns.fetch_add(1, std::memory_order_relaxed);
          }
          impl_->work_queue.pop();
        }
      }
    });
    if (WaitForSingleObject(impl_->gpu_initialized_event, INFINITE) !=
        WAIT_OBJECT_0) {
      throw std::runtime_error("GPU worker initialization failed");
    }
    CloseHandle(impl_->gpu_initialized_event);
    impl_->gpu_initialized_event = nullptr;

    const std::size_t scratch_frame_samples =
        impl_->aec_frame_samples == 0 ? 480 : impl_->aec_frame_samples;
    impl_->work_queue.configure(scratch_frame_samples);
    impl_->result_queue.configure(scratch_frame_samples);
    impl_->delayed_dry.configure(scratch_frame_samples);
    impl_->mono_scratch.reserve(std::max<std::size_t>(
        scratch_frame_samples, static_cast<std::size_t>(4096)));
    impl_->converted_scratch.reserve(4096);
    impl_->host_audio_scratch.reserve(4096);
    impl_->near_queue.reserve(scratch_frame_samples * 4);
    impl_->output_queue.reserve(static_cast<std::size_t>(sample_rate_ / 5));
    impl_->near_end_scratch.resize(scratch_frame_samples);
    impl_->unshifted_far_scratch.resize(scratch_frame_samples);
    impl_->far_end_scratch.resize(scratch_frame_samples);
    impl_->transition_scratch.resize(scratch_frame_samples);
    const auto startup_samples = static_cast<std::size_t>(std::ceil(
        static_cast<double>(kOutputLeadFrames * scratch_frame_samples) *
        static_cast<double>(sample_rate_) / static_cast<double>(kSampleRate)));
    impl_->startup_silence.assign(startup_samples, 0.0F);
    noise_runtime_state_.store(
        !noise_enabled_.load(std::memory_order_relaxed)
            ? NoiseRuntimeState::disabled
            : (impl_->noise_ready.load(std::memory_order_relaxed)
                   ? NoiseRuntimeState::active
                   : NoiseRuntimeState::error),
        std::memory_order_relaxed);
    runtime_state_.store(
        !aec_enabled_.load(std::memory_order_relaxed)
            ? PluginRuntimeState::bypassed
            : (impl_->aec_ready.load(std::memory_order_relaxed)
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
  impl_->loopback.reset();
  impl_->gpu_worker_stop.store(true, std::memory_order_release);
  if (impl_->gpu_wake_event != nullptr) SetEvent(impl_->gpu_wake_event);
  if (impl_->gpu_worker.joinable()) impl_->gpu_worker.join();
  if (impl_->aec) {
    try { impl_->aec->reset(); } catch (...) {}
  }
  if (impl_->gpu_initialized_event != nullptr) {
    CloseHandle(impl_->gpu_initialized_event);
    impl_->gpu_initialized_event = nullptr;
  }
  if (impl_->gpu_wake_event != nullptr) {
    CloseHandle(impl_->gpu_wake_event);
    impl_->gpu_wake_event = nullptr;
  }
  impl_->input_resampler.reset();
  impl_->output_resampler.reset();
  impl_->reference_timeline.reset();
  impl_->telemetry_writer.reset();
  impl_->diagnostic_log.reset();
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
  impl_->next_sequence = 1;
  impl_->overload_hold_samples = 0;
  impl_->dry_bypass_until_sequence = 0;
  impl_->output_transition.reset();
  impl_->last_output_sample = 0.0F;
  impl_->pipeline_active = false;
  impl_->delayed_dry.clear();
  impl_->minimum_useful_sequence.store(1, std::memory_order_relaxed);
  impl_->fallback_frames.store(0, std::memory_order_relaxed);
  impl_->gpu_deadline_misses.store(0, std::memory_order_relaxed);
  impl_->queue_overruns.store(0, std::memory_order_relaxed);
  impl_->output_underrun_samples.store(0, std::memory_order_relaxed);
  impl_->aec_shed_until_sequence.store(0, std::memory_order_relaxed);
  impl_->noise_shed_until_sequence.store(0, std::memory_order_relaxed);
  impl_->aec_ready.store(false, std::memory_order_relaxed);
  impl_->noise_ready.store(false, std::memory_order_relaxed);
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
    if ((impl_->telemetry_writer || impl_->diagnostic_log) &&
        impl_->telemetry_frames_since_publish >= telemetry_interval) {
      impl_->telemetry_frames_since_publish %= telemetry_interval;
      const TelemetrySnapshot snapshot{
          performance_timestamp_hns(), level,
          static_cast<std::uint32_t>(runtime_state_.load(std::memory_order_relaxed)),
          static_cast<std::uint32_t>(
              noise_runtime_state_.load(std::memory_order_relaxed)),
          static_cast<std::uint32_t>(
              aec_error_reason_.load(std::memory_order_relaxed)),
          static_cast<std::uint32_t>(
              noise_error_reason_.load(std::memory_order_relaxed)),
          impl_->fallback_frames.load(std::memory_order_relaxed),
          impl_->gpu_deadline_misses.load(std::memory_order_relaxed),
          impl_->queue_overruns.load(std::memory_order_relaxed),
          impl_->output_underrun_samples.load(std::memory_order_relaxed)};
      if (impl_->telemetry_writer) impl_->telemetry_writer->publish(snapshot);
      if (impl_->diagnostic_log) impl_->diagnostic_log->publish(snapshot);
    }
  };
  if (!impl_->input_resampler) {
    copy_input(inputs, outputs, sample_count, channel_count);
    update_meter();
    return;
  }

  if (!aec_enabled_.load(std::memory_order_relaxed) &&
      !noise_enabled_.load(std::memory_order_relaxed)) {
    if (impl_->pipeline_active) {
      impl_->pipeline_active = false;
      impl_->minimum_useful_sequence.store(impl_->next_sequence,
                                           std::memory_order_release);
      impl_->delayed_dry.clear();
      impl_->near_queue.clear();
      impl_->output_queue.clear();
      impl_->input_resampler->reset();
      if (impl_->output_resampler) impl_->output_resampler->reset();
      impl_->input_origin_hns = 0;
      impl_->near_queue_start_hns = 0;
      impl_->last_output_sample = 0.0F;
      impl_->dry_bypass_until_sequence = 0;
      impl_->output_transition.reset();
    }
    copy_input(inputs, outputs, sample_count, channel_count);
    runtime_state_.store(PluginRuntimeState::bypassed,
                         std::memory_order_relaxed);
    update_meter();
    return;
  }

  try {
    if (!impl_->pipeline_active) {
      impl_->pipeline_active = true;
      impl_->input_resampler->reset();
      if (impl_->output_resampler) impl_->output_resampler->reset();
      impl_->near_queue.clear();
      impl_->output_queue.clear();
      impl_->delayed_dry.clear();
      impl_->output_queue.append(impl_->startup_silence);
      impl_->input_origin_hns = 0;
      impl_->near_queue_start_hns = 0;
      impl_->last_output_sample = 0.0F;
      impl_->dry_bypass_until_sequence = 0;
      impl_->output_transition.reset();
      impl_->minimum_useful_sequence.store(impl_->next_sequence,
                                           std::memory_order_release);
    }
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
        std::unique_lock lock(impl_->reference_mutex, std::try_to_lock);
        if (lock.owns_lock()) {
          impl_->drained_reference.swap(impl_->pending_reference);
        }
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
    } else if (impl_->aec_ready.load(std::memory_order_acquire)) {
      aec_error_reason_.store(PluginErrorReason::none,
                              std::memory_order_relaxed);
    }

    const std::size_t frame_samples = impl_->aec_frame_samples == 0
                                          ? 480
                                          : impl_->aec_frame_samples;
    auto& near_end = impl_->near_end_scratch;
    auto& unshifted_far = impl_->unshifted_far_scratch;
    auto& far_end = impl_->far_end_scratch;
    while (impl_->near_queue.size() >= frame_samples) {
      impl_->near_queue.read_exact(near_end);
      impl_->overload_hold_samples =
          impl_->overload_hold_samples > frame_samples
              ? impl_->overload_hold_samples - frame_samples
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
      const bool aec_enabled = aec_enabled_.load(std::memory_order_relaxed);
      const bool noise_enabled = noise_enabled_.load(std::memory_order_relaxed);
      const bool aec_ready = impl_->aec_ready.load(std::memory_order_acquire);
      const bool noise_ready =
          impl_->noise_ready.load(std::memory_order_acquire);
      const bool run_aec = aec_enabled && aec_ready && !reference_failed &&
                           far_coverage >= 0.98;
      const bool run_noise = noise_enabled && noise_ready;
      const bool aec_load_shed =
          impl_->next_sequence < impl_->aec_shed_until_sequence.load(
                                     std::memory_order_relaxed);
      const bool noise_load_shed =
          impl_->next_sequence < impl_->noise_shed_until_sequence.load(
                                     std::memory_order_relaxed);
      const bool dry_bypass =
          impl_->next_sequence < impl_->dry_bypass_until_sequence;

      if (impl_->overload_hold_samples != 0 || aec_load_shed || dry_bypass) {
        runtime_state_.store(PluginRuntimeState::overloaded,
                             std::memory_order_relaxed);
      } else if (!aec_enabled) {
        runtime_state_.store(PluginRuntimeState::bypassed,
                             std::memory_order_relaxed);
      } else if (reference_failed || !aec_ready) {
        runtime_state_.store(PluginRuntimeState::error,
                             std::memory_order_relaxed);
      } else if (!run_aec) {
        runtime_state_.store(PluginRuntimeState::waiting_for_reference,
                             std::memory_order_relaxed);
      } else {
        runtime_state_.store(PluginRuntimeState::aec_active,
                             std::memory_order_relaxed);
      }
      noise_runtime_state_.store(
          !noise_enabled
              ? NoiseRuntimeState::disabled
              : (impl_->overload_hold_samples != 0 || noise_load_shed ||
                         dry_bypass
                     ? NoiseRuntimeState::overloaded
                     : (noise_ready ? NoiseRuntimeState::active
                                    : NoiseRuntimeState::error)),
          std::memory_order_relaxed);

      const auto sequence = impl_->next_sequence++;
      const bool process_aec = run_aec && !aec_load_shed && !dry_bypass;
      const bool process_noise = run_noise && !noise_load_shed && !dry_bypass;
      bool enqueued = false;
      if (process_aec || process_noise) {
        enqueued = impl_->work_queue.try_push_work(
            sequence, near_end, far_end, process_aec, process_noise);
        if (!enqueued) {
          impl_->queue_overruns.fetch_add(1, std::memory_order_relaxed);
          impl_->dry_bypass_until_sequence =
              sequence + kOverloadBypassFrames;
        } else {
          SetEvent(impl_->gpu_wake_event);
        }
      }
      impl_->delayed_dry.push(sequence, near_end, enqueued);

      if (impl_->delayed_dry.size() > kWorkerLatencyFrames) {
        auto& dry = impl_->delayed_dry.front();
        while (auto* stale = impl_->result_queue.try_front()) {
          if (stale->sequence >= dry.sequence) break;
          impl_->result_queue.pop();
        }
        std::span<const float> selected = dry.samples;
        bool wet = false;
        bool consume_result = false;
        if (auto* result = impl_->result_queue.try_front();
            result != nullptr && result->sequence == dry.sequence) {
          if (dry.expects_result &&
              dry.sequence >= impl_->dry_bypass_until_sequence) {
            selected = result->samples;
            wet = result->effect_applied;
          }
          consume_result = true;
        } else if (dry.expects_result &&
                   dry.sequence >= impl_->dry_bypass_until_sequence) {
          impl_->fallback_frames.fetch_add(1, std::memory_order_relaxed);
          impl_->overload_hold_samples = kSampleRate / 2;
          impl_->dry_bypass_until_sequence =
              sequence + kOverloadBypassFrames;
          runtime_state_.store(PluginRuntimeState::overloaded,
                               std::memory_order_relaxed);
          if (noise_enabled) {
            noise_runtime_state_.store(NoiseRuntimeState::overloaded,
                                       std::memory_order_relaxed);
          }
        }
        impl_->output_transition.render(
            dry.samples, selected, wet, impl_->transition_scratch);
        if (impl_->output_resampler) {
          impl_->output_resampler->push_into(impl_->transition_scratch,
                                             impl_->host_audio_scratch);
          impl_->output_queue.append(impl_->host_audio_scratch);
        }
        // Keep the result slot owned by the audio thread until all reads finish.
        if (consume_result) impl_->result_queue.pop();
        impl_->minimum_useful_sequence.store(dry.sequence + 1,
                                             std::memory_order_release);
        impl_->delayed_dry.pop();
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
      float value = 0.0F;
      if (index < available_output) {
        value = impl_->output_queue.at(index);
        impl_->last_output_sample = value;
      } else {
        impl_->last_output_sample *= 0.9995F;
        value = impl_->last_output_sample;
      }
      for (std::uint32_t channel = 0; channel < channel_count; ++channel) {
        if (outputs[channel] != nullptr) outputs[channel][sample] = value;
      }
    }
    impl_->output_queue.consume(available_output);
    if (available_output < static_cast<std::size_t>(sample_count)) {
      impl_->output_underrun_samples.fetch_add(
          static_cast<std::size_t>(sample_count) - available_output,
          std::memory_order_relaxed);
      impl_->overload_hold_samples = kSampleRate / 2;
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
