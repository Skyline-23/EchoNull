#include <Windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "application/engine.hpp"
#include "bootstrap/live_session.hpp"
#include "infrastructure/config.hpp"
#include "infrastructure/nvidia/nvafx_aec.hpp"
#include "infrastructure/wav.hpp"
#include "infrastructure/windows/device_manager.hpp"
#include "infrastructure/windows/wasapi_capture.hpp"
#include "infrastructure/windows/wasapi_renderer.hpp"

namespace {

std::atomic<bool> g_stop_requested{false};

BOOL WINAPI console_handler(const DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
      event == CTRL_SHUTDOWN_EVENT) {
    g_stop_requested = true;
    return TRUE;
  }
  return FALSE;
}

struct ComApartment {
  ComApartment() {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    initialized = SUCCEEDED(result);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
      throw std::runtime_error("CoInitializeEx failed");
    }
  }
  ~ComApartment() { if (initialized) CoUninitialize(); }
  bool initialized = false;
};

void print_help() {
  std::cout
      << "EchoNull - NvAFX AEC-only Windows audio bridge\n\n"
      << "Usage:\n"
      << "  echonull devices\n"
      << "  echonull doctor [--config <path>]\n"
      << "  echonull run [--config <path>]\n"
      << "  echonull validate --test-wav <path> [--config <path>] [--output <dir>]\n\n"
      << "The run pipeline is microphone + render loopback -> NvAFX AEC -> VB-CABLE.\n"
      << "Noise removal, AGC, gates, dereverb, and denoising are intentionally absent.\n";
}

bool endpoint_matches(const std::vector<echonull::AudioEndpoint>& endpoints,
                      const std::wstring& selector) {
  std::wstring selector_lower = selector;
  std::transform(selector_lower.begin(), selector_lower.end(), selector_lower.begin(),
                 [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  for (const auto& endpoint : endpoints) {
    std::wstring name = endpoint.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (endpoint.id == selector || name.find(selector_lower) != std::wstring::npos ||
        (selector_lower == L"default" && endpoint.is_default)) {
      return true;
    }
  }
  return false;
}

int doctor(const echonull::Config& config) {
  ComApartment apartment;
  echonull::WasapiDeviceCatalog catalog;
  const auto capture = catalog.list(echonull::AudioFlow::capture);
  const auto render = catalog.list(echonull::AudioFlow::render);
  bool healthy = true;
  const auto check = [&healthy](const bool passed, const std::string& name,
                                const std::string& detail) {
    std::cout << (passed ? "[pass] " : "[fail] ") << name << ": " << detail << '\n';
    healthy = healthy && passed;
  };

#if ECHONULL_HAS_NVAFX
  check(true, "NvAFX build", "official SDK header and import library linked");
#else
  check(false, "NvAFX build", "reconfigure with -DAFX_SDK_ROOT=<SDK path>");
#endif
  const auto model = config.resolve_model_path();
  check(!model.empty() && std::filesystem::exists(model), "AEC model",
        model.empty() ? "aec_48k.trtpkg was not found" : model.string());
  check(endpoint_matches(capture, config.devices.microphone), "Microphone",
        echonull::wide_to_utf8(config.devices.microphone));
  check(endpoint_matches(render, config.devices.reference), "Loopback reference",
        echonull::wide_to_utf8(config.devices.reference));
  check(endpoint_matches(render, config.devices.output), "Virtual cable output",
        echonull::wide_to_utf8(config.devices.output));
  check(endpoint_matches(capture, L"NVIDIA Broadcast"), "Broadcast microphone",
        "required only for the downstream noise-removal stage");
  return healthy ? 0 : 2;
}

std::filesystem::path option_value(const int argc, wchar_t** argv,
                                   const std::wstring& option,
                                   const std::filesystem::path& fallback = {}) {
  for (int index = 2; index + 1 < argc; ++index) {
    if (argv[index] == option) return argv[index + 1];
  }
  return fallback;
}

void print_devices() {
  ComApartment apartment;
  echonull::WasapiDeviceCatalog catalog;
  const auto print_flow = [&catalog](const echonull::AudioFlow flow, const char* heading) {
    std::wcout << L"\n" << echonull::utf8_to_wide(heading) << L"\n";
    for (const auto& endpoint : catalog.list(flow)) {
      std::wcout << (endpoint.is_default ? L"* " : L"  ") << endpoint.name << L"\n"
                 << L"    " << endpoint.id << L"\n";
    }
  };
  print_flow(echonull::AudioFlow::capture, "Capture endpoints");
  print_flow(echonull::AudioFlow::render, "Render endpoints");
}

void print_snapshot(const echonull::EngineSnapshot& snapshot) {
  std::cout << std::fixed << std::setprecision(2)
            << "[status] running=" << (snapshot.running ? "true" : "false")
            << " delay_ms=" << snapshot.delay_ms
            << " delay_confidence=" << snapshot.delay_confidence
            << " process_ms=" << snapshot.processing_latency_ms
            << " frames=" << snapshot.processed_frames
            << " ref_underruns=" << snapshot.reference_underruns
            << " out_dropped=" << snapshot.output_overruns;
  if (std::isfinite(snapshot.erle_db)) std::cout << " erle_db=" << snapshot.erle_db;
  if (!snapshot.microphone.device_format.empty()) {
    std::cout << " mic_native=\"" << snapshot.microphone.device_format << '"';
  }
  if (!snapshot.reference.device_format.empty()) {
    std::cout << " ref_native=\"" << snapshot.reference.device_format << '"';
  }
  if (!snapshot.output.device_format.empty()) {
    std::cout << " out_native=\"" << snapshot.output.device_format << '"';
  }
  if (!snapshot.message.empty()) std::cout << " message=\"" << snapshot.message << '"';
  std::cout << '\n';
}

struct Recorders {
  std::mutex mutex;
  echonull::WavWriter near_writer;
  echonull::WavWriter far_writer;
  echonull::WavWriter output_writer;

  explicit Recorders(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    near_writer = echonull::WavWriter(directory / "near-before.wav", echonull::kSampleRate);
    far_writer = echonull::WavWriter(directory / "far-reference.wav", echonull::kSampleRate);
    output_writer = echonull::WavWriter(directory / "aec-output.wav", echonull::kSampleRate);
  }

  void write(const echonull::ProcessedFrame& frame) {
    std::scoped_lock lock(mutex);
    near_writer.write(frame.near_end);
    far_writer.write(frame.far_end);
    output_writer.write(frame.output);
  }
};

std::unique_ptr<Recorders> make_recorders(const std::filesystem::path& directory) {
  return directory.empty() ? nullptr : std::make_unique<Recorders>(directory);
}

echonull::EngineSnapshot run_once(const echonull::Config& config,
                                  const echonull::EngineHooks& hooks = {}) {
  echonull::WasapiCapture microphone(config.devices.microphone, false, config.sample_rate);
  echonull::WasapiCapture reference(config.devices.reference, true, config.sample_rate);
  echonull::WasapiRenderer output(config.devices.output, config.sample_rate);
  echonull::NvafxAec aec(config.resolve_model_path(), config.intensity, config.sample_rate);
  echonull::Engine engine(config.engine_settings(), microphone, reference, output, aec, print_snapshot);
  engine.run(g_stop_requested, hooks);
  return engine.snapshot();
}

int run_bridge(const echonull::Config& config) {
  auto recorders = make_recorders(config.record_directory);
  echonull::EngineHooks hooks;
  if (recorders) {
    hooks.on_frame = [&recorders](const echonull::ProcessedFrame& frame) { recorders->write(frame); };
  }

  echonull::LiveSession session(config, print_snapshot, hooks);
  session.start();
  while (!g_stop_requested) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  session.stop();
  return 0;
}

int validate(const echonull::Config& config,
             const std::filesystem::path& test_wav_path,
             const std::filesystem::path& output_directory) {
  if (test_wav_path.empty()) {
    throw std::runtime_error("validate requires --test-wav <path>");
  }
  auto test_audio = echonull::read_wav_mono(test_wav_path);
  if (test_audio.sample_rate != echonull::kSampleRate) {
    throw std::runtime_error("validation WAV must use a 48 kHz sample rate");
  }

  const auto duration_ms = static_cast<std::uint32_t>(
      1000ULL * test_audio.mono_samples.size() / echonull::kSampleRate);
  echonull::WasapiRenderer speaker(config.devices.reference, config.sample_rate, duration_ms + 3000);
  speaker.start();
  if (!speaker.wait_ready(5000)) {
    throw std::runtime_error("validation speaker failed to start: " + speaker.status().error);
  }

  Recorders recorders(output_directory);
  std::atomic<bool> playback_started{false};
  std::thread timer([&] {
    while (!playback_started && !g_stop_requested) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!g_stop_requested) {
      std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms + 1000));
      g_stop_requested = true;
    }
  });

  echonull::EngineHooks hooks;
  hooks.on_started = [&] {
    speaker.push(test_audio.mono_samples);
    playback_started = true;
    std::cout << "[validation] playback started; remain silent for an ERLE-only run, "
                 "or speak to capture a double-talk audit.\n";
  };
  hooks.on_frame = [&recorders](const echonull::ProcessedFrame& frame) { recorders.write(frame); };

  echonull::EngineSnapshot result;
  try {
    result = run_once(config, hooks);
  } catch (...) {
    g_stop_requested = true;
    if (timer.joinable()) timer.join();
    speaker.stop();
    throw;
  }
  if (timer.joinable()) timer.join();
  speaker.stop();

  std::cout << "[validation] recordings: " << output_directory.string() << '\n';
  if (std::isfinite(result.erle_db)) {
    std::cout << "[validation] measured ERLE: " << std::fixed << std::setprecision(2)
              << result.erle_db << " dB\n";
    if (result.erle_db < 20.0) {
      std::cout << "[validation] below the 20 dB target; inspect delay alignment and room geometry.\n";
      return 2;
    }
  } else {
    std::cout << "[validation] ERLE unavailable because no active far-end interval was measured.\n";
    return 2;
  }
  return 0;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
  SetConsoleCtrlHandler(console_handler, TRUE);
  try {
    if (argc < 2 || std::wstring(argv[1]) == L"help" || std::wstring(argv[1]) == L"--help") {
      print_help();
      return 0;
    }

    const std::wstring command = argv[1];
    if (command == L"devices") {
      print_devices();
      return 0;
    }

    const auto config_path = option_value(argc, argv, L"--config", L"config/echonull.ini");
    const auto config = echonull::Config::load(config_path);
    if (command == L"doctor") return doctor(config);
    if (command == L"run") return run_bridge(config);
    if (command == L"validate") {
      const auto test_wav = option_value(argc, argv, L"--test-wav");
      const auto output = option_value(argc, argv, L"--output", L"out/validation");
      return validate(config, test_wav, output);
    }

    print_help();
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << '\n';
    return 1;
  }
}
