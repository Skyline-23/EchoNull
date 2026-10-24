#include "infrastructure/windows/capture_endpoint_hook.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>
#include <mmdeviceapi.h>

namespace echonull {
namespace {
constexpr wchar_t kPre[] = L"{EACD2258-FCAC-4FF4-B36D-419E924A6D79}";
constexpr wchar_t kLfx[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1";
constexpr wchar_t kSfx[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";
constexpr wchar_t kEfx[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";
constexpr wchar_t kMultiEfx[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15";
constexpr wchar_t kModes[] = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},7";
constexpr wchar_t kCapture[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture";
constexpr wchar_t kMetadata[] = L"SOFTWARE\\EqualizerAPO\\Child APOs\\";
constexpr wchar_t kOwnership[] = L"SOFTWARE\\EchoNull\\CaptureEndpointHooks\\";

void check(LSTATUS status) {
  if (status != ERROR_SUCCESS)
    throw std::runtime_error("Capture endpoint registration failed (Windows error " +
                             std::to_string(status) + ")");
}
struct Value { DWORD type; std::vector<BYTE> bytes; };
using MaybeValue = std::optional<Value>;
struct Key {
  HKEY handle = nullptr;
  Key(HKEY root, const std::wstring& path, bool create = false, bool writable = false) {
    const auto access = KEY_READ | (create ? KEY_WRITE : writable ? KEY_SET_VALUE : 0) | KEY_WOW64_64KEY;
    if (create) check(RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0,
                                     access, nullptr, &handle, nullptr));
    else {
      auto status = RegOpenKeyExW(root, path.c_str(), 0, access, &handle);
      if (status != ERROR_FILE_NOT_FOUND) check(status);
    }
  }
  ~Key() { if (handle) RegCloseKey(handle); }
  MaybeValue read(const wchar_t* name) const {
    if (!handle) return {};
    DWORD type = 0, count = 0;
    auto status = RegQueryValueExW(handle, name, nullptr, &type, nullptr, &count);
    if (status == ERROR_FILE_NOT_FOUND) return {};
    check(status);
    Value value{type, std::vector<BYTE>(count)};
    check(RegQueryValueExW(handle, name, nullptr, &type, value.bytes.data(), &count));
    return value;
  }
  void write(const wchar_t* name, const MaybeValue& value) const {
    if (value) check(RegSetValueExW(handle, name, 0, value->type,
                                    value->bytes.data(), static_cast<DWORD>(value->bytes.size())));
    else {
      auto status = RegDeleteValueW(handle, name);
      if (status != ERROR_FILE_NOT_FOUND) check(status);
    }
  }
};
Value text_value(const std::wstring& text, DWORD type = REG_SZ) {
  Value result{type, std::vector<BYTE>((text.size() + 1) * sizeof(wchar_t))};
  std::memcpy(result.bytes.data(), text.c_str(), result.bytes.size());
  return result;
}
bool equals(const MaybeValue& value, const wchar_t* text) {
  if (!value || value->type != REG_SZ || value->bytes.size() < sizeof(wchar_t)) return false;
  std::wstring stored(value->bytes.size() / sizeof(wchar_t), L'\0');
  std::memcpy(stored.data(), value->bytes.data(), stored.size() * sizeof(wchar_t));
  while (!stored.empty() && stored.back() == L'\0') stored.pop_back();
  return _wcsicmp(stored.c_str(), text) == 0;
}
bool empty_value(const MaybeValue& value) {
  return !value || equals(value, L"") || equals(value, L"{00000000-0000-0000-0000-000000000000}");
}
void save(Key& state, const wchar_t* name, const MaybeValue& value) {
  state.write((std::wstring(name) + L".present").c_str(), text_value(value ? L"1" : L"0"));
  state.write(name, value);
}
MaybeValue saved(Key& state, const wchar_t* name) {
  if (!equals(state.read((std::wstring(name) + L".present").c_str()), L"1")) return {};
  return state.read(name);
}
std::vector<std::wstring> children(HKEY root, const std::wstring& path) {
  Key key(root, path);
  std::vector<std::wstring> result;
  if (!key.handle) return result;
  for (DWORD index = 0;; ++index) {
    std::array<wchar_t, 256> name{};
    DWORD size = static_cast<DWORD>(name.size());
    auto status = RegEnumKeyExW(key.handle, index, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    check(status);
    result.emplace_back(name.data(), size);
  }
  return result;
}
}  // namespace

bool install_capture_endpoint_hook(HKEY root, const std::wstring& endpoint_path,
    const std::wstring& metadata_path, const std::wstring& ownership_path) {
  Key view(root, endpoint_path), metadata_view(root, metadata_path);
  if (!view.handle || !metadata_view.handle) return false;
  Key existing(root, ownership_path);
  if (existing.handle && equals(existing.read(L"Active"), L"1")) {
    if (!equals(view.read(kEfx), kPre) || equals(view.read(kSfx), kPre) || equals(view.read(kLfx), kPre))
      throw std::runtime_error("Microphone APO registration changed outside EchoNull. Restore its endpoint hook before updating.");
    return true;
  }
  if (!equals(view.read(kSfx), kPre) && !equals(view.read(kLfx), kPre)) return false;
  if (!empty_value(view.read(kEfx)) || view.read(kMultiEfx))
    throw std::runtime_error("Microphone already has an endpoint APO. EchoNull will not overwrite it.");

  Key fx(root, endpoint_path, false, true), metadata(root, metadata_path, false, true);
  const auto lfx = fx.read(kLfx), sfx = fx.read(kSfx), efx = fx.read(kEfx), modes = fx.read(kModes);
  const auto child = metadata.read(L"PreMixChild");
  auto original_sfx = metadata.read(kSfx);
  if (equals(original_sfx, L"!VALUE") || equals(original_sfx, L"!KEY")) original_sfx.reset();
  if (!original_sfx && child && !empty_value(child)) original_sfx = child;
  if (equals(original_sfx, kPre)) throw std::runtime_error("Recursive microphone APO chain detected.");
  Key state(root, ownership_path, true);
  save(state, L"Lfx", lfx);
  save(state, L"Sfx", sfx); save(state, L"Efx", efx);
  save(state, L"Modes", modes); save(state, L"PreMixChild", child);
  try {
    // The original vendor SFX stays in its original stage. Equalizer APO is
    // instantiated once, at EFX before the capture tee, including RAW clients.
    fx.write(kSfx, original_sfx);
    if (equals(lfx, kPre)) {
      auto original_lfx = metadata.read(kLfx);
      if (equals(original_lfx, L"!VALUE") || equals(original_lfx, L"!KEY")) original_lfx.reset();
      fx.write(kLfx, original_lfx);
    }
    metadata.write(L"PreMixChild", text_value(L""));
    std::wstring supported = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";
    supported.push_back(L'\0');
    supported += L"{9E90EA20-B493-4FD1-A1A8-7E1361A956CF}";
    supported.push_back(L'\0');
    fx.write(kModes, text_value(supported, REG_MULTI_SZ));
    fx.write(kEfx, text_value(kPre));
    state.write(L"Active", text_value(L"1"));
  } catch (...) {
    fx.write(kLfx, lfx); fx.write(kSfx, sfx); fx.write(kEfx, efx); fx.write(kModes, modes);
    metadata.write(L"PreMixChild", child);
    throw;
  }
  return true;
}

void restore_capture_endpoint_hook(HKEY root, const std::wstring& endpoint_path,
    const std::wstring& metadata_path, const std::wstring& ownership_path) {
  Key state(root, ownership_path, false, true);
  if (!state.handle || !equals(state.read(L"Active"), L"1")) return;
  Key fx(root, endpoint_path, false, true), metadata(root, metadata_path, false, true);
  if (!fx.handle || !metadata.handle || !equals(fx.read(kEfx), kPre))
    throw std::runtime_error("Microphone endpoint APO changed; its new registration was not overwritten.");
  fx.write(kEfx, saved(state, L"Efx"));
  fx.write(kModes, saved(state, L"Modes"));
  fx.write(kLfx, saved(state, L"Lfx"));
  fx.write(kSfx, saved(state, L"Sfx"));
  metadata.write(L"PreMixChild", saved(state, L"PreMixChild"));
  state.write(L"Active", text_value(L"0"));
}

unsigned install_capture_endpoint_hooks() {
  unsigned installed = 0;
  for (const auto& id : children(HKEY_LOCAL_MACHINE, kCapture)) {
    const auto path = std::wstring(kCapture) + L"\\" + id;
    Key endpoint(HKEY_LOCAL_MACHINE, path);
    const auto state = endpoint.read(L"DeviceState");
    if (!state || state->type != REG_DWORD || state->bytes.size() != sizeof(DWORD)) continue;
    DWORD active = 0; std::memcpy(&active, state->bytes.data(), sizeof(active));
    if (active != DEVICE_STATE_ACTIVE) continue;
    installed += install_capture_endpoint_hook(HKEY_LOCAL_MACHINE, path + L"\\FxProperties",
        std::wstring(kMetadata) + id, std::wstring(kOwnership) + id) ? 1U : 0U;
  }
  if (installed == 0)
    throw std::runtime_error("No supported Equalizer APO microphone is selected. Enable the microphone in Equalizer APO Device Selector first.");
  return installed;
}
void restore_capture_endpoint_hooks() {
  for (const auto& id : children(HKEY_LOCAL_MACHINE, kOwnership))
    restore_capture_endpoint_hook(HKEY_LOCAL_MACHINE,
        std::wstring(kCapture) + L"\\" + id + L"\\FxProperties",
        std::wstring(kMetadata) + id, std::wstring(kOwnership) + id);
}
bool capture_endpoint_hooks_present() {
  for (const auto& id : children(HKEY_LOCAL_MACHINE, kOwnership)) {
    Key state(HKEY_LOCAL_MACHINE, std::wstring(kOwnership) + id);
    if (!equals(state.read(L"Active"), L"1")) continue;
    Key fx(HKEY_LOCAL_MACHINE, std::wstring(kCapture) + L"\\" + id + L"\\FxProperties");
    if (fx.handle && equals(fx.read(kEfx), kPre) && !equals(fx.read(kSfx), kPre) && !equals(fx.read(kLfx), kPre)) return true;
  }
  return false;
}
}  // namespace echonull
