#pragma once

#include <Windows.h>
#include <string>

namespace echonull {

// Reuse the user's installed Equalizer APO; do not install an audio driver or
// another endpoint. Only microphones already selected in Equalizer APO qualify.
unsigned install_capture_endpoint_hooks();
void restore_capture_endpoint_hooks();
bool capture_endpoint_hooks_present();

// Explicit registry roots allow the migration and undo to be tested in HKCU.
bool install_capture_endpoint_hook(HKEY root, const std::wstring& endpoint_path,
    const std::wstring& metadata_path, const std::wstring& ownership_path);
void restore_capture_endpoint_hook(HKEY root, const std::wstring& endpoint_path,
    const std::wstring& metadata_path, const std::wstring& ownership_path);

}  // namespace echonull
