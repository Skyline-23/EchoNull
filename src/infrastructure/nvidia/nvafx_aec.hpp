#pragma once

#include <filesystem>
#include <memory>

#include "application/ports.hpp"

namespace echonull {

class NvafxAec final : public IAecProcessor {
 public:
  NvafxAec(std::filesystem::path model_path, float intensity,
           std::uint32_t sample_rate = kSampleRate);
  ~NvafxAec() override;

  NvafxAec(const NvafxAec&) = delete;
  NvafxAec& operator=(const NvafxAec&) = delete;

  void initialize() override;
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
