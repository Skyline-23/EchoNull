#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

#include "domain/audio_types.hpp"

namespace echonull {

struct PluginEditorSettings {
  bool aec_enabled = true;
  float aec_strength = 1.0F;
  bool noise_enabled = false;
  float noise_strength = 1.0F;
  std::wstring playback_endpoint_id;
};

class PluginEditor {
 public:
  static constexpr int kWidth = 700;
  static constexpr int kHeight = 580;

  using SettingsHandler = std::function<void(const PluginEditorSettings&)>;

  PluginEditor(HINSTANCE module, PluginEditorSettings settings,
               SettingsHandler settings_handler);
  ~PluginEditor();

  PluginEditor(const PluginEditor&) = delete;
  PluginEditor& operator=(const PluginEditor&) = delete;

  [[nodiscard]] bool open(HWND parent);
  void close() noexcept;
  void idle(float output_level, const std::wstring& runtime_status,
            bool runtime_ok);
  void set_settings(const PluginEditorSettings& settings);

 private:
  static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                      WPARAM wparam, LPARAM lparam);
  LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

  [[nodiscard]] int px(int logical) const;
  [[nodiscard]] RECT scaled_rect(int left, int top, int right, int bottom) const;
  [[nodiscard]] RECT aec_toggle_rect() const;
  [[nodiscard]] RECT noise_toggle_rect() const;
  [[nodiscard]] RECT aec_slider_rect() const;
  [[nodiscard]] RECT noise_slider_rect() const;

  void create_controls();
  void refresh();
  void apply();
  void paint();
  void draw_item(const DRAWITEMSTRUCT& item);
  void draw_toggle(HDC dc, const RECT& rect, bool enabled) const;
  void draw_slider(HDC dc, const RECT& rect, float value, bool enabled) const;
  void update_slider(int slider, int mouse_x);
  void notify_settings();
  void set_status(std::wstring status, bool ok);

  HINSTANCE module_ = nullptr;
  PluginEditorSettings settings_;
  SettingsHandler settings_handler_;
  HWND window_ = nullptr;
  HWND playback_combo_ = nullptr;
  HWND refresh_button_ = nullptr;
  HWND apply_button_ = nullptr;
  HFONT header_font_ = nullptr;
  HFONT heading_font_ = nullptr;
  HFONT body_font_ = nullptr;
  HFONT small_font_ = nullptr;
  HBRUSH background_brush_ = nullptr;
  HBRUSH control_brush_ = nullptr;
  std::vector<AudioEndpoint> endpoints_;
  std::wstring status_ = L"Plug-in ready";
  std::wstring runtime_status_ = L"IDLE";
  float output_level_ = 0.0F;
  UINT dpi_ = 96;
  int dragging_slider_ = 0;
  bool status_ok_ = true;
  bool runtime_ok_ = true;
  bool com_initialized_ = false;
};

}  // namespace echonull
