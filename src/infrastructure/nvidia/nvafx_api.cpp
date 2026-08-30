#include "infrastructure/nvidia/nvafx_api.hpp"

#if ECHONULL_HAS_NVAFX

#include <Windows.h>

#include <array>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace echonull {
namespace {

template <typename Function>
Function import(HMODULE module, const char* name) {
  const auto value = GetProcAddress(module, name);
  if (value == nullptr) {
    throw std::runtime_error(std::string("NVAudioEffects.dll is missing ") + name);
  }
  return reinterpret_cast<Function>(value);
}

}  // namespace

struct NvafxApi::Impl {
  using CreateEffect = decltype(&NvAFX_CreateEffect);
  using DestroyEffect = decltype(&NvAFX_DestroyEffect);
  using SetString = decltype(&NvAFX_SetString);
  using SetU32 = decltype(&NvAFX_SetU32);
  using SetFloat = decltype(&NvAFX_SetFloat);
  using GetU32 = decltype(&NvAFX_GetU32);
  using Load = decltype(&NvAFX_Load);
  using Run = decltype(&NvAFX_Run);
  using Reset = decltype(&NvAFX_Reset);

  std::mutex mutex;
  std::vector<HMODULE> modules;
  CreateEffect create_effect = nullptr;
  DestroyEffect destroy_effect = nullptr;
  SetString set_string = nullptr;
  SetU32 set_u32 = nullptr;
  SetFloat set_float = nullptr;
  GetU32 get_u32 = nullptr;
  Load load = nullptr;
  Run run = nullptr;
  Reset reset = nullptr;
};

NvafxApi& NvafxApi::instance() {
  static NvafxApi api;
  return api;
}

NvafxApi::Impl* NvafxApi::implementation() {
  static Impl value;
  return &value;
}

void NvafxApi::load(const std::filesystem::path& runtime_directory) {
  auto* state = implementation();
  std::scoped_lock lock(state->mutex);
  if (state->create_effect != nullptr) return;

  constexpr std::array<const wchar_t*, 9> libraries{
      L"libcrypto-3-x64.dll", L"cublasLt64_12.dll", L"cublas64_12.dll",
      L"cufft64_11.dll", L"nvrtc64_120_0.dll", L"nvinfer_10.dll",
      L"NVAudioEffects.dll", L"nvafxaec.dll", L"nvafxdenoiser.dll"};
  HMODULE core = nullptr;
  for (const auto* name : libraries) {
    const auto path = runtime_directory / name;
    if (!std::filesystem::is_regular_file(path)) continue;
    const HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
                                         LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
      throw std::runtime_error("cannot load packaged NvAFX library: " +
                               path.filename().string());
    }
    state->modules.push_back(module);
    if (_wcsicmp(name, L"NVAudioEffects.dll") == 0) core = module;
  }
  if (core == nullptr) {
    throw std::runtime_error("packaged NVAudioEffects.dll is missing");
  }

  state->create_effect = import<Impl::CreateEffect>(core, "NvAFX_CreateEffect");
  state->destroy_effect = import<Impl::DestroyEffect>(core, "NvAFX_DestroyEffect");
  state->set_string = import<Impl::SetString>(core, "NvAFX_SetString");
  state->set_u32 = import<Impl::SetU32>(core, "NvAFX_SetU32");
  state->set_float = import<Impl::SetFloat>(core, "NvAFX_SetFloat");
  state->get_u32 = import<Impl::GetU32>(core, "NvAFX_GetU32");
  state->load = import<Impl::Load>(core, "NvAFX_Load");
  state->run = import<Impl::Run>(core, "NvAFX_Run");
  state->reset = import<Impl::Reset>(core, "NvAFX_Reset");
}

NvAFX_Status NvafxApi::create_effect(const NvAFX_EffectSelector selector,
                                     NvAFX_Handle* effect) const {
  return implementation()->create_effect(selector, effect);
}
NvAFX_Status NvafxApi::destroy_effect(const NvAFX_Handle effect) const {
  return implementation()->destroy_effect(effect);
}
NvAFX_Status NvafxApi::set_string(const NvAFX_Handle effect,
                                  const NvAFX_ParameterSelector parameter,
                                  const char* value) const {
  return implementation()->set_string(effect, parameter, value);
}
NvAFX_Status NvafxApi::set_u32(const NvAFX_Handle effect,
                               const NvAFX_ParameterSelector parameter,
                               const unsigned int value) const {
  return implementation()->set_u32(effect, parameter, value);
}
NvAFX_Status NvafxApi::set_float(const NvAFX_Handle effect,
                                 const NvAFX_ParameterSelector parameter,
                                 const float value) const {
  return implementation()->set_float(effect, parameter, value);
}
NvAFX_Status NvafxApi::get_u32(const NvAFX_Handle effect,
                               const NvAFX_ParameterSelector parameter,
                               unsigned int* value) const {
  return implementation()->get_u32(effect, parameter, value);
}
NvAFX_Status NvafxApi::load_effect(const NvAFX_Handle effect) const {
  return implementation()->load(effect);
}
NvAFX_Status NvafxApi::run(const NvAFX_Handle effect, const float** input,
                           float** output, const unsigned int samples,
                           const unsigned int channels) const {
  return implementation()->run(effect, input, output, samples, channels);
}
NvAFX_Status NvafxApi::reset(const NvAFX_Handle effect) const {
  return implementation()->reset(effect);
}

}  // namespace echonull
#endif
