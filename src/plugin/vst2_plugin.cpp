#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <cstdio>
#include <string>

#include <vst.h>

#include "plugin/plugin_editor.hpp"
#include "plugin/plugin_processor.hpp"
#include "infrastructure/windows/telemetry_bus.hpp"

namespace {

HMODULE g_module = nullptr;

std::filesystem::path module_path() {
  std::wstring path(32'768, L'\0');
  const DWORD length = GetModuleFileNameW(g_module, path.data(),
                                          static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return L"EchoNullPlugin.dll";
  path.resize(length);
  return path;
}

void copy_text(void* target, const std::size_t capacity, const char* value) {
  if (target == nullptr || capacity == 0) return;
  strncpy_s(static_cast<char*>(target), capacity, value, _TRUNCATE);
}

struct EffectInstance {
  vst_effect_t effect{};
  vst_host_callback_t host = nullptr;
  echonull::PluginProcessor processor;
  echonull::TelemetryBusReader telemetry_reader;
  std::unique_ptr<echonull::PluginEditor> editor;
  vst_rect_t editor_rect{0, 0, echonull::PluginEditor::kHeight,
                         echonull::PluginEditor::kWidth};
  float mode = 1.0F;
  float aec_enabled = 1.0F;
  float aec_strength = 1.0F;
  float noise_enabled = 0.0F;
  float noise_strength = 1.0F;
  bool started = false;
};

EffectInstance* instance(vst_effect_t* effect) {
  return effect == nullptr ? nullptr : static_cast<EffectInstance*>(effect->effect_internal);
}

void start(EffectInstance& value) noexcept {
  if (value.started) return;
  try {
    value.processor.start(module_path());
    value.started = true;
  } catch (...) {
    value.started = false;
  }
}

void stop(EffectInstance& value) noexcept {
  value.processor.stop();
  value.started = false;
}

std::pair<std::wstring, bool> runtime_status_values(
    const float mode, const float aec_enabled, const float noise_enabled,
    const echonull::PluginRuntimeState runtime_state,
    const echonull::NoiseRuntimeState noise_state,
    const echonull::PluginErrorReason aec_error,
    const echonull::PluginErrorReason noise_error) {
  if (mode < 0.5F) return {L"REFERENCE TAP ACTIVE", true};
  const bool noise_on = noise_enabled >= 0.5F;
  if (aec_error == echonull::PluginErrorReason::package ||
      noise_error == echonull::PluginErrorReason::package) {
    return {L"PACKAGE ERROR · BYPASS", false};
  }
  if (aec_enabled < 0.5F) {
    if (!noise_on) return {L"ALL EFFECTS BYPASSED", true};
    if (noise_state == echonull::NoiseRuntimeState::active) {
      return {L"NOISE REMOVAL ACTIVE", true};
    }
    return {noise_error == echonull::PluginErrorReason::model_missing
                ? L"NOISE MODEL MISSING · BYPASS"
                : L"NVIDIA RUNTIME / GPU ERROR · BYPASS",
            false};
  }
  switch (runtime_state) {
    case echonull::PluginRuntimeState::reference_active:
      return {L"REFERENCE TAP ACTIVE", true};
    case echonull::PluginRuntimeState::waiting_for_reference:
      if (noise_on && noise_state == echonull::NoiseRuntimeState::error) {
        return {noise_error == echonull::PluginErrorReason::model_missing
                    ? L"WAITING FOR PLAYBACK · NOISE MODEL MISSING"
                    : L"WAITING FOR PLAYBACK · NOISE RUNTIME ERROR",
                false};
      }
      return {noise_on ? L"WAITING FOR PLAYBACK · NOISE ON"
                       : L"WAITING FOR PLAYBACK", true};
    case echonull::PluginRuntimeState::aec_active:
      if (noise_on && noise_state == echonull::NoiseRuntimeState::error) {
        return {noise_error == echonull::PluginErrorReason::model_missing
                    ? L"RTX AEC ACTIVE · NOISE MODEL MISSING"
                    : L"RTX AEC ACTIVE · NOISE RUNTIME ERROR",
                false};
      }
      return {noise_on ? L"RTX AEC + NOISE ACTIVE" : L"RTX AEC ACTIVE", true};
    case echonull::PluginRuntimeState::bypassed:
      return {L"AEC BYPASSED", true};
    case echonull::PluginRuntimeState::error:
      return {aec_error == echonull::PluginErrorReason::model_missing
                  ? L"AEC MODEL MISSING · BYPASS"
                  : L"NVIDIA RUNTIME / GPU ERROR · BYPASS",
              false};
    case echonull::PluginRuntimeState::idle:
    default:
      return {L"IDLE", true};
  }
}

std::pair<std::wstring, bool> runtime_status(const EffectInstance& value) {
  return runtime_status_values(
      value.mode, value.aec_enabled, value.noise_enabled,
      value.processor.runtime_state(), value.processor.noise_runtime_state(),
      value.processor.aec_error_reason(), value.processor.noise_error_reason());
}

std::pair<std::wstring, bool> runtime_status(
    const EffectInstance& value, const echonull::TelemetrySnapshot& telemetry) {
  return runtime_status_values(
      value.mode, value.aec_enabled, value.noise_enabled,
      static_cast<echonull::PluginRuntimeState>(telemetry.runtime_state),
      static_cast<echonull::NoiseRuntimeState>(telemetry.noise_state),
      static_cast<echonull::PluginErrorReason>(telemetry.aec_error),
      static_cast<echonull::PluginErrorReason>(telemetry.noise_error));
}

echonull::PluginEditorSettings editor_settings(const EffectInstance& value) {
  return echonull::PluginEditorSettings{
      value.aec_enabled >= 0.5F, value.aec_strength,
      value.noise_enabled >= 0.5F, value.noise_strength};
}

void apply_editor_settings(EffectInstance& value,
                           const echonull::PluginEditorSettings& settings) {
  const float parameters[4] = {
      settings.aec_enabled ? 1.0F : 0.0F,
      std::clamp(settings.aec_strength, 0.0F, 1.0F),
      settings.noise_enabled ? 1.0F : 0.0F,
      std::clamp(settings.noise_strength, 0.0F, 1.0F)};
  float* destinations[4] = {&value.aec_enabled, &value.aec_strength,
                            &value.noise_enabled, &value.noise_strength};
  for (int offset = 0; offset < 4; ++offset) {
    if (*destinations[offset] == parameters[offset]) continue;
    *destinations[offset] = parameters[offset];
    if (value.host != nullptr) {
      value.host(&value.effect, VST_HOST_OPCODE_AUTOMATE, offset + 1, 0,
                 nullptr, parameters[offset]);
    }
  }
  value.processor.set_aec_enabled(value.aec_enabled >= 0.5F);
  value.processor.set_aec_strength(value.aec_strength);
  value.processor.set_noise_enabled(value.noise_enabled >= 0.5F);
  value.processor.set_noise_strength(value.noise_strength);
}

intptr_t VST_FUNCTION_INTERFACE control(vst_effect_t* effect, const int32_t opcode,
                                        const int32_t index, const intptr_t value,
                                        void* pointer, const float option) {
  auto* self = instance(effect);
  if (self == nullptr) return 0;
  try {
    switch (opcode) {
    case VST_EFFECT_OPCODE_INITIALIZE:
      return 0;
    case VST_EFFECT_OPCODE_DESTROY:
      self->editor.reset();
      stop(*self);
      delete self;
      return 0;
    case VST_EFFECT_OPCODE_SET_SAMPLE_RATE:
      self->processor.set_sample_rate(
          static_cast<std::uint32_t>(std::max(1.0F, option)));
      return 1;
    case VST_EFFECT_OPCODE_SET_BLOCK_SIZE:
      return 1;
    case VST_EFFECT_OPCODE_SUSPEND_RESUME:
      if (value != 0) start(*self);
      else stop(*self);
      return 1;
    case VST_EFFECT_OPCODE_PROCESS_BEGIN:
      start(*self);
      return 1;
    case VST_EFFECT_OPCODE_PROCESS_END:
      stop(*self);
      return 1;
    case VST_EFFECT_OPCODE_EDITOR_GET_RECT:
      if (pointer == nullptr) return 0;
      *static_cast<vst_rect_t**>(pointer) = &self->editor_rect;
      return 1;
    case VST_EFFECT_OPCODE_EDITOR_OPEN:
      if (pointer == nullptr) return 0;
      if (!self->editor) {
        self->editor = std::make_unique<echonull::PluginEditor>(
            g_module, module_path(), editor_settings(*self),
            [self](const echonull::PluginEditorSettings& settings) {
              apply_editor_settings(*self, settings);
            });
      }
      return self->editor->open(static_cast<HWND>(pointer)) ? 1 : 0;
    case VST_EFFECT_OPCODE_EDITOR_CLOSE:
      self->editor.reset();
      return 0;
    case VST_EFFECT_OPCODE_EDITOR_KEEP_ALIVE:
      if (self->editor) {
        if (const auto telemetry = self->telemetry_reader.read_latest()) {
          const auto [status, ok] = runtime_status(*self, *telemetry);
          self->editor->idle(telemetry->output_level, status, ok);
        } else {
          const auto [status, ok] = runtime_status(*self);
          const bool audio_engine_instance =
              echonull::is_windows_audio_engine_process();
          self->editor->idle(self->processor.output_level(),
                             audio_engine_instance && self->started
                                 ? status
                                 : L"AUDIO ENGINE OFFLINE",
                             audio_engine_instance && self->started && ok);
        }
      }
      return 0;
    case VST_EFFECT_OPCODE_PARAM_NAME:
      if (index == 0) copy_text(pointer, VST_BUFFER_SIZE_PARAM_NAME, "Mode");
      else if (index == 1) copy_text(pointer, VST_BUFFER_SIZE_PARAM_NAME, "AEC");
      else if (index == 2) copy_text(pointer, VST_BUFFER_SIZE_PARAM_NAME, "AEC Str");
      else if (index == 3) copy_text(pointer, VST_BUFFER_SIZE_PARAM_NAME, "Noise");
      else if (index == 4) copy_text(pointer, VST_BUFFER_SIZE_PARAM_NAME, "N.Str");
      return 0;
    case VST_EFFECT_OPCODE_PARAM_LABEL:
      if (index == 0) copy_text(pointer, VST_BUFFER_SIZE_PARAM_LABEL, "");
      return 0;
    case VST_EFFECT_OPCODE_PARAM_VALUE:
      if (index == 0) {
        copy_text(pointer, VST_BUFFER_SIZE_PARAM_VALUE,
                  self->mode < 0.5F ? "Ref" : "AEC");
      }
      else if (index == 1) {
        copy_text(pointer, VST_BUFFER_SIZE_PARAM_VALUE,
                  self->aec_enabled < 0.5F ? "Off" : "On");
      }
      else if (index == 2 || index == 4) {
        char display[16]{};
        const float strength = index == 2 ? self->aec_strength : self->noise_strength;
        sprintf_s(display, "%d%%", static_cast<int>(std::lround(strength * 100.0F)));
        copy_text(pointer, VST_BUFFER_SIZE_PARAM_VALUE, display);
      }
      else if (index == 3) {
        copy_text(pointer, VST_BUFFER_SIZE_PARAM_VALUE,
                  self->noise_enabled < 0.5F ? "Off" : "On");
      }
      return 0;
    case VST_EFFECT_OPCODE_CATEGORY:
      return VST_EFFECT_CATEGORY_RESTORATION;
    case VST_EFFECT_OPCODE_GETNAME:
      copy_text(pointer, VST_BUFFER_SIZE_EFFECT_NAME, "EchoNull AEC");
      return 1;
    case VST_EFFECT_OPCODE_VENDOR_NAME:
      copy_text(pointer, VST_BUFFER_SIZE_VENDOR_NAME, "EchoNull");
      return 1;
    case VST_EFFECT_OPCODE_PRODUCT_NAME:
      copy_text(pointer, VST_BUFFER_SIZE_PRODUCT_NAME, "EchoNull AEC");
      return 1;
    case VST_EFFECT_OPCODE_VENDOR_VERSION:
      return 100;
    case VST_EFFECT_OPCODE_VST_VERSION:
      return VST_VERSION_2_4_0_0;
    case VST_EFFECT_OPCODE_SUPPORTS:
      return 0;
      default:
        return 0;
    }
  } catch (...) {
    return 0;
  }
}

void VST_FUNCTION_INTERFACE set_parameter(vst_effect_t* effect,
                                          const uint32_t index,
                                          const float value) {
  auto* self = instance(effect);
  if (self == nullptr || index > 4) return;
  try {
    if (index == 0) {
      self->mode = value < 0.5F ? 0.0F : 1.0F;
      self->processor.set_mode(self->mode < 0.5F
                                   ? echonull::PluginMode::reference
                                   : echonull::PluginMode::aec);
    } else if (index == 1) {
      self->aec_enabled = value < 0.5F ? 0.0F : 1.0F;
      self->processor.set_aec_enabled(self->aec_enabled >= 0.5F);
    } else if (index == 2) {
      self->aec_strength = std::clamp(value, 0.0F, 1.0F);
      self->processor.set_aec_strength(self->aec_strength);
    } else if (index == 3) {
      self->noise_enabled = value < 0.5F ? 0.0F : 1.0F;
      self->processor.set_noise_enabled(self->noise_enabled >= 0.5F);
    } else if (index == 4) {
      self->noise_strength = std::clamp(value, 0.0F, 1.0F);
      self->processor.set_noise_strength(self->noise_strength);
    }
    if (self->editor) self->editor->set_settings(editor_settings(*self));
  } catch (...) {
  }
}

float VST_FUNCTION_INTERFACE get_parameter(vst_effect_t* effect,
                                           const uint32_t index) {
  const auto* self = instance(effect);
  if (self == nullptr) return 0.0F;
  if (index == 0) return self->mode;
  if (index == 1) return self->aec_enabled;
  if (index == 2) return self->aec_strength;
  if (index == 3) return self->noise_enabled;
  if (index == 4) return self->noise_strength;
  return 0.0F;
}

void VST_FUNCTION_INTERFACE process(vst_effect_t* effect,
                                    const float* const* inputs, float** outputs,
                                    const int32_t samples) {
  auto* self = instance(effect);
  if (self == nullptr) return;
  std::uint32_t channels = 2;
  if (self->host != nullptr &&
      self->host(effect, VST_HOST_OPCODE_04, 1, 0, nullptr, 0.0F) != 0) {
    channels = 1;
  }
  self->processor.process(inputs, outputs, samples, channels);
}

}  // namespace

extern "C" __declspec(dllexport) vst_effect_t* VSTPluginMain(
    const vst_host_callback_t callback) {
  if (callback == nullptr ||
      callback(nullptr, VST_HOST_OPCODE_VST_VERSION, 0, 0, nullptr, 0.0F) == 0) {
    return nullptr;
  }

  auto* self = new (std::nothrow) EffectInstance();
  if (self == nullptr) return nullptr;
  self->host = callback;
  self->effect.magic_number = VST_MAGICNUMBER;
  self->effect.control = &control;
  self->effect.process = &process;
  self->effect.set_parameter = &set_parameter;
  self->effect.get_parameter = &get_parameter;
  self->effect.num_programs = 1;
  self->effect.num_params = 5;
  self->effect.num_inputs = 2;
  self->effect.num_outputs = 2;
  self->effect.flags = VST_EFFECT_FLAG_SUPPORTS_FLOAT | VST_EFFECT_FLAG_EDITOR;
  self->effect.input_output_ratio = 1.0F;
  self->effect.effect_internal = self;
  self->effect.unique_id = static_cast<int32_t>(VST_FOURCC('E', 'N', 'A', 'C'));
  self->effect.version = 100;
  self->effect.process_float = &process;
  return &self->effect;
}

extern "C" __declspec(dllexport) int EchoNullRuntimeSelfTest() {
  try {
    echonull::PluginProcessor processor;
    processor.set_noise_enabled(false);
    processor.start(module_path());
    const auto reason = processor.aec_error_reason();
    const auto state = processor.runtime_state();
    processor.stop();
    if (reason == echonull::PluginErrorReason::none &&
        state == echonull::PluginRuntimeState::waiting_for_reference) {
      return 0;
    }
    if (reason == echonull::PluginErrorReason::package) return 1;
    if (reason == echonull::PluginErrorReason::model_missing) return 2;
    return 3;
  } catch (...) {
    return 4;
  }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    DisableThreadLibraryCalls(module);
  }
  return TRUE;
}
