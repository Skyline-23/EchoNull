#include "infrastructure/windows/diagnostic_log.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace echonull {
namespace {

constexpr std::uint64_t kMaximumLogBytes = 2U * 1024U * 1024U;
constexpr std::int64_t kStatisticsIntervalHns = 50'000'000;
std::atomic<std::uint64_t> next_session_id{1};

std::filesystem::path environment_path(const wchar_t* name) {
  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
  if (required == 0) return {};
  std::wstring value(required, L'\0');
  const DWORD written = GetEnvironmentVariableW(
      name, value.data(), static_cast<DWORD>(value.size()));
  if (written == 0 || written >= value.size()) return {};
  value.resize(written);
  return value;
}

bool can_append(const std::filesystem::path& path) {
  const HANDLE file = CreateFileW(
      path.c_str(), FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  CloseHandle(file);
  return true;
}

std::filesystem::path log_path() {
  if (const auto override_root = environment_path(L"ECHONULL_LOG_ROOT");
      !override_root.empty()) {
    std::filesystem::create_directories(override_root);
    const auto candidate = override_root / L"EchoNull.log";
    if (!can_append(candidate)) {
      throw std::runtime_error("could not open the diagnostic log override");
    }
    return candidate;
  }
  auto root = environment_path(L"ProgramData");
  if (!root.empty()) {
    std::error_code error;
    const auto directory = root / L"EchoNull" / L"Logs";
    std::filesystem::create_directories(directory, error);
    const auto candidate = directory / L"EchoNull.log";
    if (!error && can_append(candidate)) return candidate;
  }

  std::wstring temporary(32'768, L'\0');
  const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()),
                                    temporary.data());
  if (length == 0 || length >= temporary.size()) {
    throw std::runtime_error("could not resolve the diagnostic log directory");
  }
  temporary.resize(length);
  const auto directory = std::filesystem::path(temporary) / L"EchoNull" /
                         L"Logs";
  std::filesystem::create_directories(directory);
  const auto candidate = directory / L"EchoNull.log";
  if (!can_append(candidate)) {
    throw std::runtime_error("could not open the diagnostic log");
  }
  return candidate;
}

void rotate_if_needed(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error ||
      std::filesystem::file_size(path, error) < kMaximumLogBytes || error) {
    return;
  }
  const auto previous = path.wstring() + L".1";
  std::filesystem::remove(previous, error);
  error.clear();
  std::filesystem::rename(path, previous, error);
}

std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t value = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_s(&local, &value);
  std::ostringstream output;
  output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
  return output.str();
}

void write_line(std::ofstream& output, const std::string& message) {
  output << timestamp() << " " << message << '\n';
  output.flush();
}

const char* runtime_name(const std::uint32_t value) noexcept {
  switch (value) {
    case 0: return "idle";
    case 1: return "waiting_for_reference";
    case 2: return "aec_active";
    case 3: return "bypassed";
    case 4: return "error";
    case 5: return "overloaded";
    default: return "unknown";
  }
}

const char* noise_state_name(const std::uint32_t value) noexcept {
  switch (value) {
    case 0: return "disabled";
    case 1: return "active";
    case 2: return "error";
    case 3: return "overloaded";
    default: return "unknown";
  }
}

const char* error_name(const std::uint32_t value) noexcept {
  switch (value) {
    case 0: return "none";
    case 1: return "package";
    case 2: return "model_missing";
    case 3: return "runtime_or_gpu";
    case 4: return "reference_capture";
    default: return "unknown";
  }
}

}  // namespace

struct AsyncDiagnosticLog::Impl {
  std::mutex mutex;
  TelemetrySnapshot pending;
  bool has_pending = false;
  bool stop = false;
  HANDLE wake_event = nullptr;
  std::thread writer;
};

AsyncDiagnosticLog::AsyncDiagnosticLog() : impl_(std::make_unique<Impl>()) {}
AsyncDiagnosticLog::~AsyncDiagnosticLog() { close(); }

void AsyncDiagnosticLog::open() {
  close();
  const auto path = log_path();
  const auto identity =
      " pid=" + std::to_string(GetCurrentProcessId()) +
      " session=" + std::to_string(
          next_session_id.fetch_add(1, std::memory_order_relaxed));
  impl_->wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (impl_->wake_event == nullptr) {
    throw std::runtime_error("could not create the diagnostic log event");
  }
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->stop = false;
    impl_->has_pending = false;
  }
  impl_->writer = std::thread([this, path, identity] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    rotate_if_needed(path);
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) return;
    write_line(output, "SESSION started" + identity);

    TelemetrySnapshot previous{};
    std::int64_t last_statistics_hns = 0;
    for (;;) {
      WaitForSingleObject(impl_->wake_event, 1000);
      TelemetrySnapshot snapshot{};
      bool has_snapshot = false;
      bool stopping = false;
      {
        std::scoped_lock lock(impl_->mutex);
        stopping = impl_->stop;
        if (impl_->has_pending) {
          snapshot = impl_->pending;
          impl_->has_pending = false;
          has_snapshot = true;
        }
      }
      if (has_snapshot) {
        if (last_statistics_hns == 0) {
          write_line(output, "GPU_SETUP priority_class=" +
              std::to_string(snapshot.gpu_priority_class) +
              " priority_status=" + std::to_string(snapshot.gpu_priority_status) +
              " shared_cuda_context=" + std::to_string(snapshot.shared_cuda_context) +
              " output_lead_ms=80 cpu_fallback=0 effect_shedding=0" + identity);
        }
        if ((snapshot.aec_error != 0 || snapshot.noise_error != 0) &&
            (snapshot.aec_error != previous.aec_error ||
             snapshot.noise_error != previous.noise_error ||
             snapshot.runtime_state != previous.runtime_state ||
             snapshot.noise_state != previous.noise_state)) {
          write_line(output,
                     "ERROR runtime=" +
                         std::string(runtime_name(snapshot.runtime_state)) +
                         " noise_state=" + noise_state_name(snapshot.noise_state) +
                         " aec_error=" + error_name(snapshot.aec_error) +
                         " noise_error=" + error_name(snapshot.noise_error) +
                         identity);
        }
        // Also write healthy progress, so zero errors cannot be mistaken for
        // a stalled stream or a worker which never ran either effect.
        if (last_statistics_hns == 0 ||
             snapshot.timestamp_hns - last_statistics_hns >=
                 kStatisticsIntervalHns) {
          write_line(output,
                     "REALTIME protected_miss_frames=" +
                         std::to_string(snapshot.protected_miss_frames) +
                         " gpu_deadline_misses=" +
                         std::to_string(snapshot.gpu_deadline_misses) +
                         " queue_overruns=" +
                         std::to_string(snapshot.queue_overruns) +
                         " output_underrun_samples=" +
                         std::to_string(snapshot.output_underrun_samples) +
                         " aec_processed_frames=" + std::to_string(snapshot.aec_processed_frames) +
                         " noise_processed_frames=" + std::to_string(snapshot.noise_processed_frames) +
                         " gpu_run_max_us=" + std::to_string(snapshot.gpu_run_max_us) +
                         identity);
          last_statistics_hns = snapshot.timestamp_hns;
        }
        previous = snapshot;
      }
      if (stopping) break;
    }
    write_line(output, "SESSION stopped" + identity);
  });
}

void AsyncDiagnosticLog::close() noexcept {
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->stop = true;
  }
  if (impl_->wake_event != nullptr) SetEvent(impl_->wake_event);
  if (impl_->writer.joinable()) impl_->writer.join();
  if (impl_->wake_event != nullptr) {
    CloseHandle(impl_->wake_event);
    impl_->wake_event = nullptr;
  }
}

void AsyncDiagnosticLog::publish(
    const TelemetrySnapshot& snapshot) noexcept {
  std::unique_lock lock(impl_->mutex, std::try_to_lock);
  if (!lock.owns_lock() || impl_->stop || impl_->wake_event == nullptr) return;
  impl_->pending = snapshot;
  impl_->has_pending = true;
  SetEvent(impl_->wake_event);
}

}  // namespace echonull
