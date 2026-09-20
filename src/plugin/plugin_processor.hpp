#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>

namespace echonull {

enum class PluginRuntimeState {
  idle,
  waiting_for_reference,
  aec_active,
  bypassed,
  error,
  overloaded,
};
enum class NoiseRuntimeState { disabled, active, error, overloaded };
enum class PluginErrorReason {
  none,
  package,
  model_missing,
  runtime_or_gpu,
  reference_capture,
};

class PluginProcessor {
 public:
  PluginProcessor();
  ~PluginProcessor();

  PluginProcessor(const PluginProcessor&) = delete;
  PluginProcessor& operator=(const PluginProcessor&) = delete;

  void set_reference_endpoint(std::wstring endpoint_id);
  void set_aec_enabled(bool enabled) noexcept;
  void set_aec_strength(float strength) noexcept;
  void set_noise_enabled(bool enabled) noexcept;
  void set_noise_strength(float strength) noexcept;
  void set_sample_rate(std::uint32_t sample_rate);
  void start(const std::filesystem::path& plugin_path);
  void stop() noexcept;
  void process(const float* const* inputs, float** outputs,
               std::int32_t sample_count, std::uint32_t channel_count) noexcept;

  [[nodiscard]] const std::wstring& reference_endpoint() const noexcept {
    return reference_endpoint_id_;
  }
  [[nodiscard]] bool aec_enabled() const noexcept {
    return aec_enabled_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] float aec_strength() const noexcept {
    return aec_strength_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool noise_enabled() const noexcept {
    return noise_enabled_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] float noise_strength() const noexcept {
    return noise_strength_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] NoiseRuntimeState noise_runtime_state() const noexcept {
    return noise_runtime_state_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] PluginErrorReason aec_error_reason() const noexcept {
    return aec_error_reason_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] PluginErrorReason noise_error_reason() const noexcept {
    return noise_error_reason_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] PluginRuntimeState runtime_state() const noexcept {
    return runtime_state_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] float output_level() const noexcept {
    return output_level_.load(std::memory_order_relaxed);
  }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::wstring reference_endpoint_id_;
  std::atomic<bool> aec_enabled_{true};
  std::atomic<float> aec_strength_{1.0F};
  std::atomic<bool> noise_enabled_{false};
  std::atomic<float> noise_strength_{1.0F};
  std::uint32_t sample_rate_ = 48'000;
  std::filesystem::path plugin_path_;
  std::atomic<PluginRuntimeState> runtime_state_{PluginRuntimeState::idle};
  std::atomic<NoiseRuntimeState> noise_runtime_state_{
      NoiseRuntimeState::disabled};
  std::atomic<PluginErrorReason> aec_error_reason_{PluginErrorReason::none};
  std::atomic<PluginErrorReason> noise_error_reason_{PluginErrorReason::none};
  std::atomic<float> output_level_{0.0F};
};

}  // namespace echonull
