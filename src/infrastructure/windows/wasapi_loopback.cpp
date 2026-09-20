#include "infrastructure/windows/wasapi_loopback.hpp"

#include <wrl/client.h>

#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "application/streaming_resampler.hpp"
#include "domain/audio_types.hpp"
#include "infrastructure/windows/device_manager.hpp"
#include "infrastructure/windows/performance_clock.hpp"
#include "infrastructure/windows/realtime_audio_thread.hpp"
#include "infrastructure/windows/wasapi_format.hpp"

namespace echonull {
namespace {

using Microsoft::WRL::ComPtr;

class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE value) : value_(value) {}
  ~ScopedHandle() {
    if (value_ != nullptr) CloseHandle(value_);
  }
  [[nodiscard]] HANDLE get() const { return value_; }

 private:
  HANDLE value_ = nullptr;
};

}  // namespace

WasapiLoopbackCapture::WasapiLoopbackCapture(std::wstring endpoint_id)
    : endpoint_id_(std::move(endpoint_id)) {}

WasapiLoopbackCapture::~WasapiLoopbackCapture() { stop(); }

void WasapiLoopbackCapture::start(PacketHandler handler) {
  stop();
  handler_ = std::move(handler);
  stop_requested_ = false;
  state_.set_starting();
  thread_ = std::thread([this] { run(); });
}

void WasapiLoopbackCapture::stop() noexcept {
  stop_requested_ = true;
  if (thread_.joinable()) thread_.join();
}

std::int64_t WasapiLoopbackCapture::qpc_now_hns() {
  return performance_timestamp_hns();
}

void WasapiLoopbackCapture::run() noexcept {
  ensure_realtime_audio_thread_priority();
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize_com = SUCCEEDED(com_result);
  try {
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
      throw_if_failed(com_result, "CoInitializeEx");
    }
    if (endpoint_id_.empty()) {
      throw std::runtime_error("no playback reference endpoint is selected");
    }

    const auto device = DeviceManager::resolve(eRender, endpoint_id_);
    ComPtr<IAudioClient> audio_client;
    throw_if_failed(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(
                                         audio_client.GetAddressOf())),
                    "IMMDevice::Activate(IAudioClient)");

    WAVEFORMATEX* raw_format = nullptr;
    throw_if_failed(audio_client->GetMixFormat(&raw_format),
                    "IAudioClient::GetMixFormat");
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix_format(
        raw_format, &CoTaskMemFree);
    const WasapiFormat format(*mix_format);

    constexpr REFERENCE_TIME kBufferDurationHns = 200'000;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_LOOPBACK;
    throw_if_failed(audio_client->Initialize(
                        AUDCLNT_SHAREMODE_SHARED, flags, kBufferDurationHns, 0,
                        mix_format.get(), nullptr),
                    "IAudioClient::Initialize(loopback)");

    ScopedHandle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.get() == nullptr) throw std::runtime_error("CreateEventW failed");
    throw_if_failed(audio_client->SetEventHandle(event.get()),
                    "IAudioClient::SetEventHandle");

    ComPtr<IAudioCaptureClient> capture_client;
    throw_if_failed(audio_client->GetService(IID_PPV_ARGS(&capture_client)),
                    "IAudioClient::GetService(IAudioCaptureClient)");
    StreamingResampler resampler(format.sample_rate(), kSampleRate);
    std::vector<float> mono;
    throw_if_failed(audio_client->Start(), "IAudioClient::Start(loopback)");
    state_.set_ready();

    while (!stop_requested_) {
      const DWORD wait_result = WaitForSingleObject(event.get(), 100);
      if (wait_result == WAIT_TIMEOUT) continue;
      if (wait_result != WAIT_OBJECT_0) {
        throw std::runtime_error("loopback capture event wait failed");
      }

      UINT32 next_frames = 0;
      throw_if_failed(capture_client->GetNextPacketSize(&next_frames),
                      "IAudioCaptureClient::GetNextPacketSize");
      while (next_frames > 0) {
        BYTE* data = nullptr;
        UINT32 frame_count = 0;
        DWORD buffer_flags = 0;
        UINT64 device_position = 0;
        UINT64 qpc_position_hns = 0;
        throw_if_failed(capture_client->GetBuffer(
                            &data, &frame_count, &buffer_flags,
                            &device_position, &qpc_position_hns),
                        "IAudioCaptureClient::GetBuffer");

        const bool silent =
            (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const bool discontinuity =
            (buffer_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        const bool timestamp_error =
            (buffer_flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
        mono.assign(frame_count, 0.0F);
        if (!silent) format.decode_mono(data, frame_count, mono);
        if (discontinuity) resampler.reset();

        const auto packet_start_frame = resampler.input_frames_received();
        auto converted = resampler.push(mono);
        const auto packet_timestamp =
            timestamp_error ? qpc_now_hns()
                            : static_cast<std::int64_t>(qpc_position_hns);
        const auto relative_frames = converted.first_input_frame -
                                     static_cast<double>(packet_start_frame);
        const auto converted_timestamp =
            packet_timestamp + static_cast<std::int64_t>(std::llround(
                relative_frames *
                static_cast<double>(kHundredNanosecondsPerSecond) /
                static_cast<double>(format.sample_rate())));
        if (handler_ && !converted.samples.empty()) {
          handler_(converted_timestamp, std::move(converted.samples),
                   discontinuity);
        }
        throw_if_failed(capture_client->ReleaseBuffer(frame_count),
                        "IAudioCaptureClient::ReleaseBuffer");
        throw_if_failed(capture_client->GetNextPacketSize(&next_frames),
                        "IAudioCaptureClient::GetNextPacketSize");
      }
    }
    audio_client->Stop();
    state_.set_stopped();
  } catch (const std::exception& error) {
    state_.fail(error.what());
  }
  if (uninitialize_com) CoUninitialize();
}

}  // namespace echonull
