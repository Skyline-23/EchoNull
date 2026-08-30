#include "infrastructure/windows/wasapi_capture.hpp"

#include <Avrt.h>
#include <wrl/client.h>

#include <chrono>
#include <span>
#include <stdexcept>
#include <utility>

#include "infrastructure/windows/device_manager.hpp"

namespace echonull {
namespace {

using Microsoft::WRL::ComPtr;

class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE value = nullptr) : value_(value) {}
  ~ScopedHandle() { if (value_ != nullptr) CloseHandle(value_); }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  [[nodiscard]] HANDLE get() const { return value_; }
 private:
  HANDLE value_ = nullptr;
};

class ScopedMmcss {
 public:
  ScopedMmcss() {
    DWORD task_index = 0;
    handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
  }
  ~ScopedMmcss() { if (handle_ != nullptr) AvRevertMmThreadCharacteristics(handle_); }
 private:
  HANDLE handle_ = nullptr;
};

}  // namespace

WasapiCapture::WasapiCapture(std::wstring selector, const bool loopback,
                             const std::uint32_t sample_rate)
    : selector_(std::move(selector)), loopback_(loopback), sample_rate_(sample_rate) {}

WasapiCapture::~WasapiCapture() {
  stop();
}

void WasapiCapture::start(CaptureHandler handler) {
  stop();
  handler_ = std::move(handler);
  stop_requested_ = false;
  packets_ = 0;
  frames_ = 0;
  discontinuities_ = 0;
  state_.set_starting();
  thread_ = std::thread([this] { run(); });
}

void WasapiCapture::stop() {
  stop_requested_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool WasapiCapture::wait_ready(const std::uint32_t timeout_ms) const {
  return state_.wait_ready(timeout_ms);
}

StreamStatus WasapiCapture::status() const {
  StreamStatus result;
  result.ready = state_.ready();
  result.failed = state_.failed();
  result.sample_rate = sample_rate_;
  result.error = state_.error();
  {
    std::scoped_lock lock(status_mutex_);
    result.endpoint = endpoint_;
  }
  result.counters.packets = packets_.load();
  result.counters.frames = frames_.load();
  result.counters.discontinuities = discontinuities_.load();
  return result;
}

std::int64_t WasapiCapture::qpc_now_hns() {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<std::int64_t>(
      static_cast<long double>(counter.QuadPart) * kHundredNanosecondsPerSecond /
      static_cast<long double>(frequency.QuadPart));
}

void WasapiCapture::run() {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize_com = SUCCEEDED(com_result);
  try {
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
      throw_if_failed(com_result, "CoInitializeEx");
    }

    DeviceInfo resolved;
    const auto device = DeviceManager::resolve(loopback_ ? eRender : eCapture, selector_, &resolved);
    {
      std::scoped_lock lock(status_mutex_);
      endpoint_ = AudioEndpoint{loopback_ ? AudioFlow::render : AudioFlow::capture,
                                resolved.id, resolved.name, resolved.is_default};
    }

    ComPtr<IAudioClient> audio_client;
    throw_if_failed(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(audio_client.GetAddressOf())),
                    "IMMDevice::Activate(IAudioClient)");

    auto format = float_mono_format(sample_rate_);
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (loopback_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    constexpr REFERENCE_TIME kBufferDurationHns = 200'000;
    throw_if_failed(audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                             kBufferDurationHns, 0,
                                             &format.Format, nullptr),
                    "IAudioClient::Initialize(capture)");

    ScopedHandle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.get() == nullptr) throw std::runtime_error("CreateEventW failed");
    throw_if_failed(audio_client->SetEventHandle(event.get()), "IAudioClient::SetEventHandle");

    ComPtr<IAudioCaptureClient> capture_client;
    throw_if_failed(audio_client->GetService(IID_PPV_ARGS(&capture_client)),
                    "IAudioClient::GetService(IAudioCaptureClient)");
    ScopedMmcss mmcss;
    throw_if_failed(audio_client->Start(), "IAudioClient::Start(capture)");
    state_.set_ready();
    const bool watch_default = selector_.empty() || _wcsicmp(selector_.c_str(), L"default") == 0;
    auto next_default_check = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (!stop_requested_) {
      const DWORD wait_result = WaitForSingleObject(event.get(), 250);
      if (wait_result == WAIT_TIMEOUT) continue;
      if (wait_result != WAIT_OBJECT_0) throw std::runtime_error("capture event wait failed");

      const auto now = std::chrono::steady_clock::now();
      if (watch_default && now >= next_default_check) {
        DeviceInfo current;
        static_cast<void>(DeviceManager::resolve(loopback_ ? eRender : eCapture, L"default", &current));
        if (current.id != resolved.id) throw std::runtime_error("default capture endpoint changed");
        next_default_check = now + std::chrono::seconds(1);
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
        throw_if_failed(capture_client->GetBuffer(&data, &frame_count, &buffer_flags,
                                                  &device_position, &qpc_position_hns),
                        "IAudioCaptureClient::GetBuffer");
        const bool silent = (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const bool discontinuity = (buffer_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        const bool timestamp_error = (buffer_flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
        std::vector<float> silence;
        std::span<const float> samples;
        if (silent) {
          silence.assign(frame_count, 0.0F);
          samples = silence;
        } else {
          samples = {reinterpret_cast<const float*>(data), frame_count};
        }
        if (discontinuity) ++discontinuities_;
        if (handler_) {
          handler_(CapturePacket{
              timestamp_error ? qpc_now_hns() : static_cast<std::int64_t>(qpc_position_hns),
              samples,
              discontinuity});
        }
        ++packets_;
        frames_ += frame_count;
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
