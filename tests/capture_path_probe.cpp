#include <Windows.h>
#include <Audioclient.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include "infrastructure/windows/device_manager.hpp"
#include "infrastructure/windows/telemetry_bus.hpp"
#include "infrastructure/windows/wasapi_format.hpp"
#include "infrastructure/windows/capture_endpoint_hook.hpp"

// Measures counters/levels only. Audio samples are discarded, never saved.
int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  try {
    const bool raw = argc > 1 && std::wstring(argv[1]) == L"raw";
    const auto device = echonull::DeviceManager::resolve(eCapture, L"default");
    Microsoft::WRL::ComPtr<IAudioClient2> client;
    auto check = [](HRESULT result) {
      if (FAILED(result)) throw std::runtime_error("WASAPI HRESULT=" + std::to_string(static_cast<unsigned>(result)));
    };
    check(device->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf())));
    AudioClientProperties properties{};
    properties.cbSize = sizeof(properties);
    properties.eCategory = AudioCategory_Communications;
    properties.Options = raw ? AUDCLNT_STREAMOPTIONS_RAW : AUDCLNT_STREAMOPTIONS_NONE;
    check(client->SetClientProperties(&properties));
    WAVEFORMATEX* wave = nullptr;
    check(client->GetMixFormat(&wave));
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(wave, CoTaskMemFree);
    echonull::WasapiFormat decoder(*wave);
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            1'000'000, 0, wave, nullptr));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const auto close = std::unique_ptr<void, decltype(&CloseHandle)>(event, CloseHandle);
    if (!event) throw std::runtime_error("CreateEvent");
    check(client->SetEventHandle(event));
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
    check(client->GetService(IID_PPV_ARGS(&capture)));
    check(client->Start());
    echonull::TelemetryBusReader telemetry;
    telemetry.open();
    const auto before = telemetry.read_latest();
    std::uint64_t samples = 0;
    double energy = 0;
    float peak = 0;
    std::vector<float> mono;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until) {
      if (WaitForSingleObject(event, 100) != WAIT_OBJECT_0) continue;
      UINT32 count = 0;
      check(capture->GetNextPacketSize(&count));
      while (count) {
        BYTE* data = nullptr; DWORD flags = 0;
        check(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr));
        mono.assign(count, 0);
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) decoder.decode_mono(data, count, mono);
        for (const auto value : mono) { peak = std::max(peak, std::abs(value)); energy += value * value; }
        samples += count;
        check(capture->ReleaseBuffer(count));
        check(capture->GetNextPacketSize(&count));
      }
    }
    const auto after = telemetry.read_latest();
    client->Stop();
    if (!after) throw std::runtime_error("no current EchoNull telemetry from audio engine");
    const auto noise_delta = after->noise_processed_frames - (before ? before->noise_processed_frames : 0);
    std::cout << "{\"mode\":\"" << (raw ? "raw" : "default")
        << "\",\"endpoint_hook\":" << echonull::capture_endpoint_hooks_present()
        << ",\"samples\":" << samples << ",\"rms\":" << std::sqrt(energy / std::max<std::uint64_t>(samples, 1))
        << ",\"peak\":" << peak << ",\"noise_frames_delta\":" << noise_delta
        << ",\"aec_frames\":" << after->aec_processed_frames
        << ",\"runtime\":" << after->runtime_state << ",\"aec_error\":" << after->aec_error
        << ",\"noise_error\":" << after->noise_error << "}\n";
    if (!samples || !noise_delta || after->aec_error || after->noise_error)
      throw std::runtime_error("capture path did not show healthy processing");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
