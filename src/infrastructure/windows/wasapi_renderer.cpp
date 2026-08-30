#include "infrastructure/windows/wasapi_renderer.hpp"

#include <Avrt.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "application/streaming_resampler.hpp"
#include "infrastructure/windows/device_manager.hpp"
#include "infrastructure/windows/wasapi_format.hpp"

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
    result.device_sample_rate = device_sample_rate_;
    result.device_channels = device_channels_;
    result.device_format = device_format_;
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
      endpoint_ = AudioEndpoint{AudioFlow::render, resolved.id, resolved.apo_guid,
                                resolved.name, resolved.is_default};
    }

    ComPtr<IAudioClient> audio_client;
    throw_if_failed(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(audio_client.GetAddressOf())),
                    "IMMDevice::Activate(IAudioClient)");
    WAVEFORMATEX* mix_format_raw = nullptr;
    throw_if_failed(audio_client->GetMixFormat(&mix_format_raw), "IAudioClient::GetMixFormat");
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix_format(
        mix_format_raw, &CoTaskMemFree);
    const WasapiFormat device_format(*mix_format);
    {
      std::scoped_lock lock(status_mutex_);
      device_sample_rate_ = device_format.sample_rate();
      device_channels_ = device_format.channels();
      device_format_ = device_format.description();
    }
    constexpr DWORD kFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    constexpr REFERENCE_TIME kBufferDurationHns = 200'000;
    throw_if_failed(audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, kFlags,
                                             kBufferDurationHns, 0,
                                             mix_format.get(), nullptr),
                    "IAudioClient::Initialize(render)");
    StreamingResampler resampler(sample_rate_, device_format.sample_rate());
    std::deque<float> converted_queue;

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
    const bool watch_default = selector_.empty() || _wcsicmp(selector_.c_str(), L"default") == 0;
    auto next_default_check = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (!stop_requested_) {
      const DWORD wait_result = WaitForSingleObject(event.get(), 250);
      if (wait_result == WAIT_TIMEOUT) continue;
      if (wait_result != WAIT_OBJECT_0) throw std::runtime_error("render event wait failed");

      const auto now = std::chrono::steady_clock::now();
      if (watch_default && now >= next_default_check) {
        DeviceInfo current;
        static_cast<void>(DeviceManager::resolve(eRender, L"default", &current));
        if (current.id != resolved.id) throw std::runtime_error("default render endpoint changed");
        next_default_check = now + std::chrono::seconds(1);
      }

      UINT32 padding = 0;
      throw_if_failed(audio_client->GetCurrentPadding(&padding), "IAudioClient::GetCurrentPadding");
      const UINT32 available = buffer_frames - padding;
      if (available == 0) continue;
      BYTE* raw = nullptr;
      throw_if_failed(render_client->GetBuffer(available, &raw), "IAudioRenderClient::GetBuffer");
      bool underrun = false;
      while (converted_queue.size() < available) {
        const auto missing = available - converted_queue.size();
        const auto estimated_input = static_cast<std::size_t>(std::ceil(
            static_cast<double>(missing) * static_cast<double>(sample_rate_) /
            static_cast<double>(device_format.sample_rate())));
        std::vector<float> pipeline_input(std::max<std::size_t>(estimated_input + 34, 64));
        const std::size_t written = queue_.pop(pipeline_input);
        underrun = underrun || written < pipeline_input.size();
        auto converted = resampler.push(pipeline_input);
        converted_queue.insert(converted_queue.end(), converted.samples.begin(),
                               converted.samples.end());
        if (converted.samples.empty() && pipeline_input.empty()) break;
      }
      std::vector<float> device_mono(available, 0.0F);
      const auto copy_count = std::min<std::size_t>(available, converted_queue.size());
      for (std::size_t index = 0; index < copy_count; ++index) {
        device_mono[index] = converted_queue.front();
        converted_queue.pop_front();
      }
      if (copy_count < available) underrun = true;
      if (underrun) ++underruns_;
      device_format.encode_mono(device_mono, raw, available);
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
