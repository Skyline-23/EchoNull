#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "application/ports.hpp"

namespace echonull {

class NvafxAec final : public IAecProcessor {
 public:
  NvafxAec(std::vector<std::filesystem::path> model_paths, float intensity,
           std::uint32_t sample_rate = kSampleRate);
  ~NvafxAec() override;

  NvafxAec(const NvafxAec&) = delete;
  NvafxAec& operator=(const NvafxAec&) = delete;

  void initialize() override;
  void initialize(bool use_current_cuda_context);
  void reset() override;
  void set_intensity(float intensity);
  void process(std::span<const float> near_end,
               std::span<const float> far_end,
               std::span<float> output) override;
  [[nodiscard]] AecStatus status() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace echonull
