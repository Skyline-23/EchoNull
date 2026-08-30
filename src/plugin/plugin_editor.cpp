#include "plugin/plugin_editor.hpp"

#include <CommCtrl.h>
#include <Uxtheme.h>
#include <Windowsx.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "infrastructure/config.hpp"
#include "infrastructure/windows/equalizer_apo.hpp"

namespace echonull {
namespace {

constexpr wchar_t kWindowClass[] = L"EchoNullPluginEditor";
constexpr int kPlaybackCombo = 1001;
constexpr int kRefreshButton = 1002;
constexpr int kApplyButton = 1003;

constexpr COLORREF kBackground = RGB(24, 24, 24);
constexpr COLORREF kControl = RGB(37, 37, 37);
constexpr COLORREF kControlHover = RGB(48, 48, 48);
constexpr COLORREF kDivider = RGB(52, 52, 52);
constexpr COLORREF kText = RGB(239, 239, 239);
constexpr COLORREF kMuted = RGB(176, 176, 176);
constexpr COLORREF kGreen = RGB(118, 185, 0);
constexpr COLORREF kGreenBright = RGB(139, 219, 0);
constexpr COLORREF kError = RGB(235, 94, 94);

std::wstring exception_message(const std::exception& error) {
  try {
    return utf8_to_wide(error.what());
  } catch (...) {
    return L"Unknown error";
  }
}

void fill_rect(HDC dc, const RECT& rect, const COLORREF color) {
  const HBRUSH brush = CreateSolidBrush(color);
  FillRect(dc, &rect, brush);
  DeleteObject(brush);
}

void draw_line(HDC dc, const int left, const int top, const int right,
               const COLORREF color) {
  const HPEN pen = CreatePen(PS_SOLID, 1, color);
  const HGDIOBJ previous = SelectObject(dc, pen);
  MoveToEx(dc, left, top, nullptr);
  LineTo(dc, right, top);
  SelectObject(dc, previous);
  DeleteObject(pen);
}

void draw_label(HDC dc, const wchar_t* text, RECT rect, HFONT font,
                const COLORREF color, const UINT format = DT_LEFT | DT_SINGLELINE) {
  const HGDIOBJ previous = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  DrawTextW(dc, text, -1, &rect, format);
  SelectObject(dc, previous);
}

}  // namespace

PluginEditor::PluginEditor(HINSTANCE module, std::filesystem::path plugin_path,
                           PluginEditorSettings settings,
                           SettingsHandler settings_handler)
    : module_(module),
      plugin_path_(std::move(plugin_path)),
      settings_(settings),
      settings_handler_(std::move(settings_handler)) {}

PluginEditor::~PluginEditor() { close(); }

int PluginEditor::px(const int logical) const {
  return MulDiv(logical, static_cast<int>(dpi_), 96);
}

RECT PluginEditor::scaled_rect(const int left, const int top, const int right,
                               const int bottom) const {
  return RECT{px(left), px(top), px(right), px(bottom)};
}

RECT PluginEditor::aec_toggle_rect() const {
  return scaled_rect(616, 216, 670, 242);
}

RECT PluginEditor::noise_toggle_rect() const {
  return scaled_rect(616, 334, 670, 360);
}

RECT PluginEditor::aec_slider_rect() const {
  return scaled_rect(210, 276, 650, 298);
}

RECT PluginEditor::noise_slider_rect() const {
  return scaled_rect(210, 394, 650, 416);
}

bool PluginEditor::open(HWND parent) {
  if (window_ != nullptr) return true;

  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  com_initialized_ = SUCCEEDED(com);

  const DPI_AWARENESS_CONTEXT previous_dpi =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // VST editor bounds are physical pixels. Per-monitor awareness keeps GDI text
  // crisp while fixed pixel coordinates keep the child inside the host's rect.
  dpi_ = 96;

  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = &PluginEditor::window_proc;
  window_class.hInstance = module_;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    if (previous_dpi != nullptr) SetThreadDpiAwarenessContext(previous_dpi);
    if (com_initialized_) CoUninitialize();
    com_initialized_ = false;
    return false;
  }

  window_ = CreateWindowExW(
      0, kWindowClass, L"EchoNull", WS_CHILD | WS_CLIPCHILDREN,
      0, 0, px(kWidth), px(kHeight), parent, nullptr, module_, this);
  if (window_ == nullptr) {
    if (previous_dpi != nullptr) SetThreadDpiAwarenessContext(previous_dpi);
    if (com_initialized_) CoUninitialize();
    com_initialized_ = false;
    return false;
  }

  background_brush_ = CreateSolidBrush(kBackground);
  control_brush_ = CreateSolidBrush(kControl);
  const auto create_font = [&](const int height, const int weight) {
    return CreateFontW(-px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  };
  header_font_ = create_font(24, FW_SEMIBOLD);
  heading_font_ = create_font(20, FW_SEMIBOLD);
  body_font_ = create_font(17, FW_NORMAL);
  small_font_ = create_font(14, FW_SEMIBOLD);
  create_controls();
  refresh();
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  if (previous_dpi != nullptr) SetThreadDpiAwarenessContext(previous_dpi);
  return true;
}

void PluginEditor::close() noexcept {
  if (window_ != nullptr) {
    DestroyWindow(window_);
    window_ = nullptr;
  }
  for (HFONT font : {header_font_, heading_font_, body_font_, small_font_}) {
    if (font != nullptr) DeleteObject(font);
  }
  header_font_ = nullptr;
  heading_font_ = nullptr;
  body_font_ = nullptr;
  small_font_ = nullptr;
  if (background_brush_ != nullptr) DeleteObject(background_brush_);
  if (control_brush_ != nullptr) DeleteObject(control_brush_);
  background_brush_ = nullptr;
  control_brush_ = nullptr;
  playback_combo_ = nullptr;
  refresh_button_ = nullptr;
  apply_button_ = nullptr;
  if (com_initialized_) CoUninitialize();
  com_initialized_ = false;
}

void PluginEditor::idle(const float output_level,
                        const std::wstring& runtime_status,
                        const bool runtime_ok) {
  const float level = std::clamp(output_level, 0.0F, 1.0F);
  if (std::abs(level - output_level_) > 0.01F ||
      runtime_status != runtime_status_ || runtime_ok != runtime_ok_) {
    output_level_ = level;
    runtime_status_ = runtime_status;
    runtime_ok_ = runtime_ok;
    if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
  }
}

void PluginEditor::set_settings(const PluginEditorSettings& settings) {
  settings_ = settings;
  settings_.aec_strength = std::clamp(settings_.aec_strength, 0.0F, 1.0F);
  settings_.noise_strength = std::clamp(settings_.noise_strength, 0.0F, 1.0F);
  if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
}

void PluginEditor::create_controls() {
  playback_combo_ = CreateWindowExW(
      0, WC_COMBOBOXW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
          CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
      px(28), px(88), px(644), px(230), window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlaybackCombo)), module_, nullptr);
  SendMessageW(playback_combo_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(34));
  SendMessageW(playback_combo_, CB_SETITEMHEIGHT, 0, px(30));
  SendMessageW(playback_combo_, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_), TRUE);
  SetWindowTheme(playback_combo_, L"DarkMode_CFD", nullptr);

  const auto create_button = [&](const wchar_t* text, const int id, const int x,
                                 const int width) {
    HWND button = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        px(x), px(143), px(width), px(38), window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module_, nullptr);
    SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(small_font_), TRUE);
    return button;
  };
  refresh_button_ = create_button(L"REFRESH", kRefreshButton, 28, 100);
  apply_button_ = create_button(L"APPLY REFERENCE", kApplyButton, 488, 184);
}

void PluginEditor::refresh() {
  SendMessageW(playback_combo_, CB_RESETCONTENT, 0, 0);
  endpoints_.clear();
  try {
    endpoints_ = EqualizerApoIntegration::enabled_playback_endpoints();
    for (std::size_t index = 0; index < endpoints_.size(); ++index) {
      std::wstring label = endpoints_[index].name;
      if (endpoints_[index].is_default) label += L"  ·  Default";
      const LRESULT item = SendMessageW(
          playback_combo_, CB_ADDSTRING, 0,
          reinterpret_cast<LPARAM>(label.c_str()));
      if (item != CB_ERR && item != CB_ERRSPACE) {
        SendMessageW(playback_combo_, CB_SETITEMDATA,
                     static_cast<WPARAM>(item), static_cast<LPARAM>(index));
      }
    }
    if (endpoints_.empty()) {
      EnableWindow(apply_button_, FALSE);
      set_status(L"No playback endpoint has Equalizer APO post-mix enabled.", false);
    } else {
      SendMessageW(playback_combo_, CB_SETCURSEL, 0, 0);
      EnableWindow(apply_button_, TRUE);
      set_status(L"Only Equalizer APO-enabled playback endpoints are shown.", true);
    }
  } catch (const std::exception& error) {
    EnableWindow(apply_button_, FALSE);
    set_status(exception_message(error), false);
  }
}

void PluginEditor::apply() {
  const LRESULT selected = SendMessageW(playback_combo_, CB_GETCURSEL, 0, 0);
  if (selected == CB_ERR) return;
  const LRESULT endpoint_index = SendMessageW(
      playback_combo_, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
  if (endpoint_index < 0 ||
      static_cast<std::size_t>(endpoint_index) >= endpoints_.size()) {
    return;
  }
  try {
    const auto& endpoint = endpoints_[static_cast<std::size_t>(endpoint_index)];
    EqualizerApoIntegration::apply_reference_endpoint(endpoint, plugin_path_);
    set_status(L"Reference applied: " + endpoint.name, true);
  } catch (const std::exception& error) {
    const auto message = exception_message(error);
    set_status(message, false);
    MessageBoxW(window_, message.c_str(), L"EchoNull", MB_OK | MB_ICONERROR);
  }
}

void PluginEditor::draw_toggle(HDC dc, const RECT& rect, const bool enabled) const {
  const HBRUSH toggle_brush = CreateSolidBrush(enabled ? kGreen : RGB(92, 92, 92));
  const HGDIOBJ previous_brush = SelectObject(dc, toggle_brush);
  const HGDIOBJ previous_pen = SelectObject(dc, GetStockObject(NULL_PEN));
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, px(26), px(26));
  const int knob_left = enabled ? rect.right - px(24) : rect.left + px(3);
  const HBRUSH knob_brush = CreateSolidBrush(RGB(245, 245, 245));
  SelectObject(dc, knob_brush);
  Ellipse(dc, knob_left, rect.top + px(3), knob_left + px(20), rect.bottom - px(3));
  SelectObject(dc, previous_pen);
  SelectObject(dc, previous_brush);
  DeleteObject(knob_brush);
  DeleteObject(toggle_brush);
}

void PluginEditor::draw_slider(HDC dc, const RECT& rect, const float value,
                               const bool enabled) const {
  const int center = (rect.top + rect.bottom) / 2;
  RECT base{rect.left, center - px(2), rect.right, center + px(2)};
  fill_rect(dc, base, RGB(105, 105, 105));
  const int knob_x = rect.left + static_cast<int>(
      std::lround(std::clamp(value, 0.0F, 1.0F) * (rect.right - rect.left)));
  RECT fill{rect.left, center - px(2), knob_x, center + px(2)};
  fill_rect(dc, fill, enabled ? kGreen : RGB(90, 90, 90));
  const HBRUSH knob = CreateSolidBrush(enabled ? kGreenBright : RGB(150, 150, 150));
  const HGDIOBJ previous_brush = SelectObject(dc, knob);
  const HGDIOBJ previous_pen = SelectObject(dc, GetStockObject(NULL_PEN));
  Ellipse(dc, knob_x - px(7), center - px(7), knob_x + px(7), center + px(7));
  SelectObject(dc, previous_pen);
  SelectObject(dc, previous_brush);
  DeleteObject(knob);
}

void PluginEditor::paint() {
  PAINTSTRUCT paint{};
  const HDC dc = BeginPaint(window_, &paint);
  RECT client{};
  GetClientRect(window_, &client);
  FillRect(dc, &client, background_brush_);

  draw_label(dc, L"ECHONULL MICROPHONE EFFECTS", scaled_rect(28, 20, 650, 50),
             header_font_, kGreenBright);
  draw_label(dc, L"PLAYBACK REFERENCE", scaled_rect(28, 61, 350, 82),
             small_font_, kMuted);
  draw_line(dc, px(28), px(198), px(672), kDivider);

  draw_label(dc, L"Playback echo cancellation", scaled_rect(28, 216, 540, 246),
             heading_font_, kText);
  draw_label(dc, L"Removes speaker playback from the microphone",
             scaled_rect(28, 248, 580, 271), body_font_, kMuted);
  draw_label(dc, L"Reference strength", scaled_rect(28, 278, 190, 301),
             body_font_, kText);
  draw_toggle(dc, aec_toggle_rect(), settings_.aec_enabled);
  draw_slider(dc, aec_slider_rect(), settings_.aec_strength,
              settings_.aec_enabled);

  draw_line(dc, px(28), px(316), px(672), kDivider);
  draw_label(dc, L"Noise removal", scaled_rect(28, 334, 540, 364),
             heading_font_, kText);
  draw_label(dc, L"Reduces steady background noise while preserving speech",
             scaled_rect(28, 366, 600, 389), body_font_, kMuted);
  draw_label(dc, L"Strength", scaled_rect(28, 396, 190, 419),
             body_font_, kText);
  draw_toggle(dc, noise_toggle_rect(), settings_.noise_enabled);
  draw_slider(dc, noise_slider_rect(), settings_.noise_strength,
              settings_.noise_enabled);

  draw_line(dc, px(28), px(438), px(672), kDivider);
  draw_label(dc, L"MICROPHONE OUTPUT", scaled_rect(28, 458, 280, 481),
             small_font_, kMuted);
  draw_label(dc, runtime_status_.c_str(), scaled_rect(330, 458, 672, 481),
             small_font_, runtime_ok_ ? kGreenBright : kError,
             DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);

  constexpr int meter_blocks = 26;
  constexpr int gap = 5;
  constexpr int block_width = 19;
  const int lit = static_cast<int>(std::ceil(output_level_ * meter_blocks));
  for (int index = 0; index < meter_blocks; ++index) {
    RECT block = scaled_rect(28 + index * (block_width + gap), 490,
                             28 + index * (block_width + gap) + block_width, 502);
    fill_rect(dc, block, index < lit ? kGreen : RGB(91, 91, 91));
  }

  draw_label(dc, status_.c_str(), scaled_rect(28, 528, 672, 568), body_font_,
             status_ok_ ? kMuted : kError,
             DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
  EndPaint(window_, &paint);
}

void PluginEditor::draw_item(const DRAWITEMSTRUCT& item) {
  if (item.CtlType == ODT_COMBOBOX) {
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    fill_rect(item.hDC, item.rcItem, selected ? kControlHover : kControl);
    wchar_t label[512]{};
    if (item.itemID != static_cast<UINT>(-1)) {
      SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID,
                   reinterpret_cast<LPARAM>(label));
    } else {
      wcscpy_s(label, L"No compatible playback endpoint");
    }
    RECT text_rect = item.rcItem;
    text_rect.left += px(12);
    text_rect.right -= px(12);
    draw_label(item.hDC, label, text_rect, body_font_, kText,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    return;
  }

  if (item.CtlType == ODT_BUTTON) {
    const int id = GetDlgCtrlID(item.hwndItem);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = id == kApplyButton;
    const COLORREF background =
        disabled ? RGB(58, 58, 58)
                 : (primary ? (pressed ? RGB(98, 154, 0) : kGreen)
                            : (pressed ? kControlHover : kControl));
    fill_rect(item.hDC, item.rcItem, background);
    const HPEN border = CreatePen(
        PS_SOLID, 1, primary && !disabled ? kGreenBright : RGB(74, 74, 74));
    const HGDIOBJ previous_pen = SelectObject(item.hDC, border);
    const HGDIOBJ previous_brush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, item.rcItem.left, item.rcItem.top,
              item.rcItem.right, item.rcItem.bottom);
    SelectObject(item.hDC, previous_brush);
    SelectObject(item.hDC, previous_pen);
    DeleteObject(border);

    wchar_t label[128]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    RECT text_rect = item.rcItem;
    draw_label(item.hDC, label, text_rect, small_font_,
               disabled ? RGB(130, 130, 130)
                        : (primary ? RGB(18, 18, 18) : kText),
               DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }
}

void PluginEditor::update_slider(const int slider, const int mouse_x) {
  const RECT rect = slider == 1 ? aec_slider_rect() : noise_slider_rect();
  const float value = std::clamp(
      static_cast<float>(mouse_x - rect.left) /
          static_cast<float>(std::max<LONG>(1, rect.right - rect.left)),
      0.0F, 1.0F);
  if (slider == 1) settings_.aec_strength = value;
  else settings_.noise_strength = value;
  notify_settings();
  InvalidateRect(window_, &rect, FALSE);
}

void PluginEditor::notify_settings() {
  if (settings_handler_) settings_handler_(settings_);
}

void PluginEditor::set_status(std::wstring status, const bool ok) {
  status_ = std::move(status);
  status_ok_ = ok;
  if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
}

LRESULT CALLBACK PluginEditor::window_proc(HWND window, const UINT message,
                                           const WPARAM wparam,
                                           const LPARAM lparam) {
  PluginEditor* editor = reinterpret_cast<PluginEditor*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    editor = static_cast<PluginEditor*>(create->lpCreateParams);
    editor->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
  }
  if (editor != nullptr) return editor->handle_message(message, wparam, lparam);
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT PluginEditor::handle_message(const UINT message, const WPARAM wparam,
                                     const LPARAM lparam) {
  switch (message) {
    case WM_PAINT:
      paint();
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_DRAWITEM:
      if (lparam != 0) draw_item(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
      return TRUE;
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
      const HDC dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, kText);
      SetBkColor(dc, kControl);
      return reinterpret_cast<LRESULT>(control_brush_);
    }
    case WM_COMMAND:
      if (HIWORD(wparam) == BN_CLICKED) {
        switch (LOWORD(wparam)) {
          case kRefreshButton: refresh(); break;
          case kApplyButton: apply(); break;
          default: break;
        }
      }
      return 0;
    case WM_LBUTTONDOWN: {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      RECT rect = aec_toggle_rect();
      if (PtInRect(&rect, point)) {
        settings_.aec_enabled = !settings_.aec_enabled;
        notify_settings();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
      }
      rect = noise_toggle_rect();
      if (PtInRect(&rect, point)) {
        settings_.noise_enabled = !settings_.noise_enabled;
        notify_settings();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
      }
      rect = aec_slider_rect();
      if (settings_.aec_enabled && PtInRect(&rect, point)) dragging_slider_ = 1;
      rect = noise_slider_rect();
      if (settings_.noise_enabled && PtInRect(&rect, point)) dragging_slider_ = 2;
      if (dragging_slider_ != 0) {
        SetCapture(window_);
        update_slider(dragging_slider_, point.x);
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (dragging_slider_ != 0 && (wparam & MK_LBUTTON) != 0) {
        update_slider(dragging_slider_, GET_X_LPARAM(lparam));
      }
      return 0;
    case WM_LBUTTONUP:
      if (dragging_slider_ != 0) {
        update_slider(dragging_slider_, GET_X_LPARAM(lparam));
        dragging_slider_ = 0;
        ReleaseCapture();
      }
      return 0;
    case WM_CAPTURECHANGED:
      dragging_slider_ = 0;
      return 0;
    case WM_DESTROY:
      SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
      return 0;
    default:
      return DefWindowProcW(window_, message, wparam, lparam);
  }
}

}  // namespace echonull
