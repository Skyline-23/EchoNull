#pragma once

#include <filesystem>

#if ECHONULL_HAS_NVAFX
#include <nvAudioEffects.h>

namespace echonull {

class NvafxApi {
 public:
  static NvafxApi& instance();

  void load(const std::filesystem::path& runtime_directory);
  [[nodiscard]] NvAFX_Status create_effect(NvAFX_EffectSelector selector,
                                            NvAFX_Handle* effect) const;
  NvAFX_Status destroy_effect(NvAFX_Handle effect) const;
  [[nodiscard]] NvAFX_Status set_string(NvAFX_Handle effect,
                                        NvAFX_ParameterSelector parameter,
                                        const char* value) const;
  [[nodiscard]] NvAFX_Status set_u32(NvAFX_Handle effect,
                                     NvAFX_ParameterSelector parameter,
                                     unsigned int value) const;
  [[nodiscard]] NvAFX_Status set_float(NvAFX_Handle effect,
                                       NvAFX_ParameterSelector parameter,
                                       float value) const;
  [[nodiscard]] NvAFX_Status get_u32(NvAFX_Handle effect,
                                     NvAFX_ParameterSelector parameter,
                                     unsigned int* value) const;
  [[nodiscard]] NvAFX_Status load_effect(NvAFX_Handle effect) const;
  [[nodiscard]] NvAFX_Status run(NvAFX_Handle effect, const float** input,
                                 float** output, unsigned int samples,
                                 unsigned int channels) const;
  NvAFX_Status reset(NvAFX_Handle effect) const;

 private:
  NvafxApi() = default;
  struct Impl;
  static Impl* implementation();
};

}  // namespace echonull
#endif
