#include <Windows.h>
#include <CommCtrl.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bootstrap/live_session.hpp"
#include "infrastructure/config.hpp"
#include "infrastructure/windows/device_manager.hpp"

namespace {

constexpr wchar_t kWindowClass[] = L"EchoNullMainWindow";
constexpr UINT kSnapshotMessage = WM_APP + 1;
constexpr int kMicrophoneCombo = 1001;
constexpr int kReferenceCombo = 1002;
constexpr int kOutputCombo = 1003;
constexpr int kRefreshButton = 1101;
constexpr int kStartButton = 1102;
constexpr int kStopButton = 1103;

std::wstring lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return value;
}

class ComApartment {
 public:
  ComApartment() {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    initialized_ = SUCCEEDED(result);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
      throw std::runtime_error("COM initialization failed");
    }
  }
  ~ComApartment() { if (initialized_) CoUninitialize(); }
 private:
  bool initialized_ = false;
};

class MainWindow {
 public:
  explicit MainWindow(echonull::Config config) : config_(std::move(config)) {}
  ~MainWindow() { stop_session(); }

  bool create(const HINSTANCE instance) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &MainWindow::window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return false;
    }

    window_ = CreateWindowExW(0, kWindowClass, L"EchoNull - NVIDIA NvAFX AEC",
                              WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 780, 540,
                              nullptr, nullptr, instance, this);
    if (window_ == nullptr) return false;
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    return true;
  }

 private:
  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      self = static_cast<MainWindow*>(create->lpCreateParams);
      self->window_ = window;
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    try {
      return self->handle_message(message, wparam, lparam);
    } catch (const std::exception& error) {
      MessageBoxA(window, error.what(), "EchoNull", MB_ICONERROR | MB_OK);
      return 0;
    }
  }

  HWND create_label(const wchar_t* text) const {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                           0, 0, 0, 0, window_, nullptr, nullptr, nullptr);
  }

  HWND create_combo(const int id) const {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                           0, 0, 0, 240, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           nullptr, nullptr);
  }

  HWND create_button(const wchar_t* text, const int id) const {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                           0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           nullptr, nullptr);
  }

  void initialize_controls() {
    title_ = create_label(L"AEC only. Noise removal stays downstream in NVIDIA Broadcast.");
    microphone_label_ = create_label(L"Physical microphone");
    reference_label_ = create_label(L"Speaker loopback reference");
    output_label_ = create_label(L"Virtual cable output");
    microphone_combo_ = create_combo(kMicrophoneCombo);
    reference_combo_ = create_combo(kReferenceCombo);
    output_combo_ = create_combo(kOutputCombo);
    refresh_button_ = create_button(L"Refresh devices", kRefreshButton);
    start_button_ = create_button(L"Start AEC", kStartButton);
    stop_button_ = create_button(L"Stop", kStopButton);
    status_label_ = create_label(L"Status: stopped");
    metrics_label_ = create_label(L"Delay: --   Processing: --   ERLE: --");
    counters_label_ = create_label(L"Frames: 0   Reference underruns: 0   Output drops: 0");
    endpoints_label_ = create_label(L"");
    note_label_ = create_label(
        L"The app reconnects after device removal/default-device changes. "
        L"Use the CLI validate command for recorded ERLE and double-talk checks.");

    font_ = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    bold_font_ = CreateFontW(-21, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    for (HWND control : {microphone_label_, reference_label_, output_label_, microphone_combo_,
                         reference_combo_, output_combo_, refresh_button_, start_button_, stop_button_,
                         status_label_, metrics_label_, counters_label_, endpoints_label_, note_label_}) {
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(bold_font_), TRUE);
    EnableWindow(stop_button_, FALSE);
    refresh_devices();
  }

  void layout(const int width) const {
    constexpr int margin = 24;
    constexpr int label_width = 200;
    constexpr int row_height = 31;
    const int combo_x = margin + label_width;
    const int combo_width = std::max(300, width - combo_x - margin);
    MoveWindow(title_, margin, 20, width - 2 * margin, 34, TRUE);
    MoveWindow(microphone_label_, margin, 76, label_width - 10, row_height, TRUE);
    MoveWindow(microphone_combo_, combo_x, 72, combo_width, 300, TRUE);
    MoveWindow(reference_label_, margin, 121, label_width - 10, row_height, TRUE);
    MoveWindow(reference_combo_, combo_x, 117, combo_width, 300, TRUE);
    MoveWindow(output_label_, margin, 166, label_width - 10, row_height, TRUE);
    MoveWindow(output_combo_, combo_x, 162, combo_width, 300, TRUE);
    MoveWindow(refresh_button_, margin, 218, 130, 36, TRUE);
    MoveWindow(start_button_, margin + 144, 218, 120, 36, TRUE);
    MoveWindow(stop_button_, margin + 278, 218, 100, 36, TRUE);
    MoveWindow(status_label_, margin, 282, width - 2 * margin, 28, TRUE);
    MoveWindow(metrics_label_, margin, 318, width - 2 * margin, 28, TRUE);
    MoveWindow(counters_label_, margin, 350, width - 2 * margin, 28, TRUE);
    MoveWindow(endpoints_label_, margin, 388, width - 2 * margin, 44, TRUE);
    MoveWindow(note_label_, margin, 446, width - 2 * margin, 46, TRUE);
  }

  static void fill_combo(HWND combo, const std::vector<echonull::AudioEndpoint>& endpoints,
                         const std::wstring& selector) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const auto selector_lower = lower(selector);
    int selection = -1;
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
      std::wstring display = endpoints[index].is_default ? L"★ " : L"";
      display += endpoints[index].name;
      const LRESULT item = SendMessageW(combo, CB_ADDSTRING, 0,
                                        reinterpret_cast<LPARAM>(display.c_str()));
      SendMessageW(combo, CB_SETITEMDATA, item, static_cast<LPARAM>(index));
      const bool default_match = selector_lower == L"default" && endpoints[index].is_default;
      const bool text_match = lower(endpoints[index].id) == selector_lower ||
                              lower(endpoints[index].name).find(selector_lower) != std::wstring::npos;
      if (selection < 0 && (default_match || text_match)) selection = static_cast<int>(item);
    }
    if (selection >= 0) SendMessageW(combo, CB_SETCURSEL, selection, 0);
  }

  void refresh_devices() {
    capture_endpoints_ = catalog_.list(echonull::AudioFlow::capture);
    render_endpoints_ = catalog_.list(echonull::AudioFlow::render);
    fill_combo(microphone_combo_, capture_endpoints_, config_.devices.microphone);
    fill_combo(reference_combo_, render_endpoints_, config_.devices.reference);
    fill_combo(output_combo_, render_endpoints_, config_.devices.output);
    if (SendMessageW(output_combo_, CB_GETCURSEL, 0, 0) == CB_ERR) {
      SetWindowTextW(status_label_, L"Status: no configured virtual cable output was found");
    }
  }

  static const echonull::AudioEndpoint& selected(HWND combo,
                                                  const std::vector<echonull::AudioEndpoint>& endpoints) {
    const LRESULT selected_item = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected_item == CB_ERR) throw std::runtime_error("select all three audio endpoints first");
    const LRESULT index = SendMessageW(combo, CB_GETITEMDATA, selected_item, 0);
    if (index < 0 || static_cast<std::size_t>(index) >= endpoints.size()) {
      throw std::runtime_error("selected audio endpoint is invalid");
    }
    return endpoints[static_cast<std::size_t>(index)];
  }

  void start_session() {
    if (session_) return;
    auto runtime_config = config_;
    runtime_config.devices.microphone = selected(microphone_combo_, capture_endpoints_).id;
    runtime_config.devices.reference = selected(reference_combo_, render_endpoints_).id;
    runtime_config.devices.output = selected(output_combo_, render_endpoints_).id;
    if (runtime_config.resolve_model_path().empty()) {
      throw std::runtime_error(
          "NvAFX AEC model was not found. Install/extract the AFX SDK and set AFX_SDK_ROOT, "
          "or set model in config/echonull.ini.");
    }

    const HWND target = window_;
    session_ = std::make_unique<echonull::LiveSession>(
        runtime_config,
        [target](const echonull::EngineSnapshot& snapshot) {
          auto* copy = new echonull::EngineSnapshot(snapshot);
          if (!PostMessageW(target, kSnapshotMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
        });
    session_->start();
    set_controls_running(true);
    SetWindowTextW(status_label_, L"Status: starting NvAFX and audio endpoints...");
  }

  void stop_session() {
    if (session_) {
      session_->stop();
      session_.reset();
    }
    MSG pending{};
    while (window_ != nullptr &&
           PeekMessageW(&pending, window_, kSnapshotMessage, kSnapshotMessage, PM_REMOVE)) {
      delete reinterpret_cast<echonull::EngineSnapshot*>(pending.lParam);
    }
    if (window_ != nullptr) {
      set_controls_running(false);
      SetWindowTextW(status_label_, L"Status: stopped");
    }
  }

  void set_controls_running(const bool running) const {
    for (HWND control : {microphone_combo_, reference_combo_, output_combo_, refresh_button_, start_button_}) {
      EnableWindow(control, !running);
    }
    EnableWindow(stop_button_, running);
  }

  void update_snapshot(const echonull::EngineSnapshot& snapshot) const {
    const std::wstring message = echonull::utf8_to_wide(snapshot.message);
    std::wstring status = snapshot.running ? L"Status: running" : L"Status: waiting/reconnecting";
    if (!message.empty()) status += L" — " + message;
    SetWindowTextW(status_label_, status.c_str());

    std::wostringstream metrics;
    metrics.setf(std::ios::fixed);
    metrics.precision(2);
    metrics << L"Delay: " << snapshot.delay_ms << L" ms (confidence "
            << snapshot.delay_confidence << L")   Processing: "
            << snapshot.processing_latency_ms << L" ms   ERLE: ";
    if (std::isfinite(snapshot.erle_db)) metrics << snapshot.erle_db << L" dB";
    else metrics << L"--";
    SetWindowTextW(metrics_label_, metrics.str().c_str());

    std::wostringstream counters;
    counters << L"Frames: " << snapshot.processed_frames
             << L"   Mic underruns: " << snapshot.mic_underruns
             << L"   Reference underruns: " << snapshot.reference_underruns
             << L"   Output drops: " << snapshot.output_overruns;
    SetWindowTextW(counters_label_, counters.str().c_str());

    std::wstring endpoints;
    if (!snapshot.microphone.endpoint.name.empty()) endpoints += L"Mic: " + snapshot.microphone.endpoint.name;
    if (!snapshot.reference.endpoint.name.empty()) endpoints += L"   Ref: " + snapshot.reference.endpoint.name;
    if (!snapshot.output.endpoint.name.empty()) endpoints += L"   Out: " + snapshot.output.endpoint.name;
    SetWindowTextW(endpoints_label_, endpoints.c_str());
  }

  LRESULT handle_message(const UINT message, const WPARAM wparam, const LPARAM lparam) {
    switch (message) {
      case WM_CREATE:
        initialize_controls();
        return 0;
      case WM_SIZE:
        layout(LOWORD(lparam));
        return 0;
      case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED) {
          switch (LOWORD(wparam)) {
            case kRefreshButton: refresh_devices(); return 0;
            case kStartButton: start_session(); return 0;
            case kStopButton: stop_session(); return 0;
            default: break;
          }
        }
        break;
      case kSnapshotMessage: {
        std::unique_ptr<echonull::EngineSnapshot> snapshot(
            reinterpret_cast<echonull::EngineSnapshot*>(lparam));
        update_snapshot(*snapshot);
        return 0;
      }
      case WM_CLOSE:
        stop_session();
        DestroyWindow(window_);
        return 0;
      case WM_DESTROY:
        if (font_ != nullptr) DeleteObject(font_);
        if (bold_font_ != nullptr) DeleteObject(bold_font_);
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;
      default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
  }

  echonull::Config config_;
  echonull::WasapiDeviceCatalog catalog_;
  std::vector<echonull::AudioEndpoint> capture_endpoints_;
  std::vector<echonull::AudioEndpoint> render_endpoints_;
  std::unique_ptr<echonull::LiveSession> session_;
  HWND window_ = nullptr;
  HWND title_ = nullptr;
  HWND microphone_label_ = nullptr;
  HWND reference_label_ = nullptr;
  HWND output_label_ = nullptr;
  HWND microphone_combo_ = nullptr;
  HWND reference_combo_ = nullptr;
  HWND output_combo_ = nullptr;
  HWND refresh_button_ = nullptr;
  HWND start_button_ = nullptr;
  HWND stop_button_ = nullptr;
  HWND status_label_ = nullptr;
  HWND metrics_label_ = nullptr;
  HWND counters_label_ = nullptr;
  HWND endpoints_label_ = nullptr;
  HWND note_label_ = nullptr;
  HFONT font_ = nullptr;
  HFONT bold_font_ = nullptr;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  try {
    ComApartment apartment;
    auto config = echonull::Config::load(L"config/echonull.ini");
    MainWindow window(std::move(config));
    if (!window.create(instance)) throw std::runtime_error("failed to create the EchoNull window");

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
  } catch (const std::exception& error) {
    MessageBoxA(nullptr, error.what(), "EchoNull", MB_ICONERROR | MB_OK);
    return 1;
  }
}
