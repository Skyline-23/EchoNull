#pragma once

#include <span>
#include <vector>

#include "domain/audio_types.hpp"

namespace echonull {

class IAecProcessor {
 public:
  virtual ~IAecProcessor() = default;
  virtual void initialize() = 0;
  virtual void reset() = 0;
  virtual void process(std::span<const float> near_end,
                       std::span<const float> far_end,
                       std::span<float> output) = 0;
  [[nodiscard]] virtual AecStatus status() const = 0;
};

class IDeviceCatalog {
 public:
  virtual ~IDeviceCatalog() = default;
  [[nodiscard]] virtual std::vector<AudioEndpoint> list(AudioFlow flow) const = 0;
};

}  // namespace echonull
