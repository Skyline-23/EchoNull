#include <Windows.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include "infrastructure/windows/capture_endpoint_hook.hpp"

namespace {
constexpr auto sfx = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";
constexpr auto efx = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";
constexpr auto pre = L"{EACD2258-FCAC-4FF4-B36D-419E924A6D79}";
constexpr auto vendor = L"{62dc1a93-ae24-464c-a43e-452f824c4250}";
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void set(const std::wstring& path, const wchar_t* name, const wchar_t* value) {
  HKEY key = nullptr;
  require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
      KEY_ALL_ACCESS, nullptr, &key, nullptr) == 0, "create test key");
  auto status = RegSetValueExW(key, name, 0, REG_SZ,
      reinterpret_cast<const BYTE*>(value), static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  require(status == 0, "set test value");
}
std::wstring get(const std::wstring& path, const wchar_t* name) {
  wchar_t value[512]{};
  DWORD bytes = sizeof(value);
  auto status = RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name,
      RRF_RT_REG_SZ, nullptr, value, &bytes);
  return status == ERROR_SUCCESS ? value : L"<missing>";
}
struct Cleanup {
  std::wstring path;
  ~Cleanup() { RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
};
}
int main() {
  const std::wstring base = L"Software\\EchoNullTests\\Endpoint-" +
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  Cleanup cleanup{base};
  try {
    const auto fx = base + L"\\Fx", meta = base + L"\\Metadata", state = base + L"\\State";
    set(fx, sfx, pre); set(meta, sfx, vendor); set(meta, L"PreMixChild", vendor);
    set(fx, L"Unrelated", L"retained");
    for (int update = 0; update < 20; ++update)
      require(echonull::install_capture_endpoint_hook(HKEY_CURRENT_USER, fx, meta, state), "hook installation");
    require(get(fx, efx) == pre, "endpoint APO was not registered");
    require(get(fx, sfx) == vendor, "vendor SFX was not preserved");
    require(get(meta, L"PreMixChild").empty(), "vendor SFX would run twice");
    require(get(fx, L"Unrelated") == L"retained", "unrelated value modified");
    echonull::restore_capture_endpoint_hook(HKEY_CURRENT_USER, fx, meta, state);
    require(get(fx, efx) == L"<missing>" && get(fx, sfx) == pre &&
            get(meta, L"PreMixChild") == vendor, "original registration was not restored");
    echonull::restore_capture_endpoint_hook(HKEY_CURRENT_USER, fx, meta, state);
    set(fx, efx, vendor);
    bool rejected = false;
    try { echonull::install_capture_endpoint_hook(HKEY_CURRENT_USER, fx, meta, state); }
    catch (const std::exception&) { rejected = true; }
    require(rejected && get(fx, efx) == vendor && get(fx, sfx) == pre,
            "existing third-party endpoint effect was overwritten");
    set(fx, efx, L""); set(fx, sfx, vendor);
    require(!echonull::install_capture_endpoint_hook(HKEY_CURRENT_USER, fx, meta, state),
            "unselected microphone was modified");
    constexpr auto lfx = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1";
    set(fx, lfx, pre); set(meta, lfx, vendor);
    require(echonull::install_capture_endpoint_hook(HKEY_CURRENT_USER, fx, meta, state), "legacy hook installation");
    require(get(fx, lfx) == vendor && get(fx, efx) == pre, "legacy APO migration duplicated capture");
    echonull::restore_capture_endpoint_hook(HKEY_CURRENT_USER, fx, meta, state);
    require(get(fx, lfx) == pre && get(fx, sfx) == vendor, "legacy undo mismatch");
    std::cout << "Capture endpoint migration, repeated updates, undo and ownership tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
