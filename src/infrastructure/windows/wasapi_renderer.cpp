#include "infrastructure/windows/wasapi_renderer.hpp"

#include <Avrt.h>
#include <wrl/client.h>

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
    handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
  }
  ~ScopedMmcss() { if (handle_ != nullptr) AvRevertMmThreadCharacteristics(handle_); }
 private:
  HANDLE handle_ = nullptr;
};

}  // namespace

WasapiRenderer::WasapiRenderer(std::wstring selector,
                               const std::uint32_t sample_rate,
                               const std::uint32_t queue_capacity_ms)
    : selector_(std::move(selector)),
      sample_rate_(sample_rate),
      queue_(static_cast<std::size_t>(sample_rate) * queue_capacity_ms / 1000U) {}

WasapiRenderer::~WasapiRenderer() {
  stop();
}

void WasapiRenderer::start() {
  stop();
  queue_.clear();
  stop_requested_ = false;
  packets_ = 0;
  frames_ = 0;
  underruns_ = 0;
  overruns_ = 0;
  state_.set_starting();
  thread_ = std::thread([this] { run(); });
}

void WasapiRenderer::stop() {
  stop_requested_ = true;
  if (thread_.joinable()) thread_.join();
}

std::size_t WasapiRenderer::push(const std::span<const float> samples) {
  const std::size_t dropped = queue_.push(samples);
  if (dropped > 0) overruns_ += dropped;
  return dropped;
}

bool WasapiRenderer::wait_ready(const std::uint32_t timeout_ms) const {
  return state_.wait_ready(timeout_ms);
}

std::size_t WasapiRenderer::queued_samples() const {
  return queue_.size();
}

StreamStatus WasapiRenderer::status() const {
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
  result.counters.underruns = underruns_.load();
  result.counters.overruns = overruns_.load();
  return result;
}

void WasapiRenderer::run() {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize_com = SUCCEEDED(com_result);
  try {
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
      throw_if_failed(com_result, "CoInitializeEx");
    }

    DeviceInfo resolved;
    const auto device = DeviceManager::resolve(eRender, selector_, &resolved);
    {
      std::scoped_lock lock(status_mutex_);
      endpoint_ = AudioEndpoint{AudioFlow::render, resolved.id, resolved.name, resolved.is_default};
    }

    ComPtr<IAudioClient> audio_client;
    throw_if_failed(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(audio_client.GetAddressOf())),
                    "IMMDevice::Activate(IAudioClient)");
    auto format = float_mono_format(sample_rate_);
    constexpr DWORD kFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                             AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    constexpr REFERENCE_TIME kBufferDurationHns = 1'000'000;
    throw_if_failed(audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, kFlags,
                                             kBufferDurationHns, 0,
                                             &format.Format, nullptr),
                    "IAudioClient::Initialize(render)");

    ScopedHandle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.get() == nullptr) throw std::runtime_error("CreateEventW failed");
    throw_if_failed(audio_client->SetEventHandle(event.get()), "IAudioClient::SetEventHandle");

    ComPtr<IAudioRenderClient> render_client;
    throw_if_failed(audio_client->GetService(IID_PPV_ARGS(&render_client)),
                    "IAudioClient::GetService(IAudioRenderClient)");
    UINT32 buffer_frames = 0;
    throw_if_failed(audio_client->GetBufferSize(&buffer_frames), "IAudioClient::GetBufferSize");
    BYTE* initial_data = nullptr;
    throw_if_failed(render_client->GetBuffer(buffer_frames, &initial_data),
                    "IAudioRenderClient::GetBuffer(initial)");
    throw_if_failed(render_client->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT),
                    "IAudioRenderClient::ReleaseBuffer(initial)");

    ScopedMmcss mmcss;
    throw_if_failed(audio_client->Start(), "IAudioClient::Start(render)");
    state_.set_ready();

    while (!stop_requested_) {
      const DWORD wait_result = WaitForSingleObject(event.get(), 250);
      if (wait_result == WAIT_TIMEOUT) continue;
      if (wait_result != WAIT_OBJECT_0) throw std::runtime_error("render event wait failed");

      UINT32 padding = 0;
      throw_if_failed(audio_client->GetCurrentPadding(&padding), "IAudioClient::GetCurrentPadding");
      const UINT32 available = buffer_frames - padding;
      if (available == 0) continue;
      BYTE* raw = nullptr;
      throw_if_failed(render_client->GetBuffer(available, &raw), "IAudioRenderClient::GetBuffer");
      auto output = std::span<float>(reinterpret_cast<float*>(raw), available);
      const std::size_t written = queue_.pop(output);
      if (written < output.size()) ++underruns_;
      throw_if_failed(render_client->ReleaseBuffer(available, 0),
                      "IAudioRenderClient::ReleaseBuffer");
      ++packets_;
      frames_ += available;
    }
    audio_client->Stop();
    state_.set_stopped();
  } catch (const std::exception& error) {
    state_.fail(error.what());
  }
  if (uninitialize_com) CoUninitialize();
}

}  // namespace echonull

