#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
#include <span>
#include <string>

#include "domain/audio_types.hpp"

namespace echonull {

struct DenoiserStatus {
  bool ready = false;
  std::uint32_t input_sample_rate = 0;
  std::uint32_t output_sample_rate = 0;
  std::uint32_t input_channels = 0;
  std::uint32_t output_channels = 0;
  std::uint32_t input_frame_samples = 0;
  std::uint32_t output_frame_samples = 0;
  std::string error;
};

class NvafxDenoiser {
 public:
  NvafxDenoiser(std::vector<std::filesystem::path> model_paths, float intensity,
                std::uint32_t sample_rate = kSampleRate);
  ~NvafxDenoiser();

  NvafxDenoiser(const NvafxDenoiser&) = delete;
  NvafxDenoiser& operator=(const NvafxDenoiser&) = delete;

  void initialize();
  void reset() noexcept;
  void set_intensity(float intensity);
  void process(std::span<const float> input, std::span<float> output);
  [[nodiscard]] DenoiserStatus status() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace echonull
