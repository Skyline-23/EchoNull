#pragma once

#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace echonull {

inline void throw_if_failed(const HRESULT result, const char* action) {
  if (FAILED(result)) {
    std::ostringstream message;
    message << action << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ')';
    throw std::runtime_error(message.str());
  }
}

class WorkerState {
 public:
  enum class Status { stopped, starting, ready, failed };

  void set_ready() {
    {
      std::scoped_lock lock(mutex_);
      status_ = Status::ready;
      error_.clear();
    }
    condition_.notify_all();
  }

  void set_starting() {
    std::scoped_lock lock(mutex_);
    status_ = Status::starting;
    error_.clear();
  }

  void set_stopped() {
    {
      std::scoped_lock lock(mutex_);
      if (status_ != Status::failed) status_ = Status::stopped;
    }
    condition_.notify_all();
  }

  void fail(std::string error) {
    {
      std::scoped_lock lock(mutex_);
      status_ = Status::failed;
      error_ = std::move(error);
    }
    condition_.notify_all();
  }

  bool wait_ready(const std::uint32_t timeout_ms) const {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
      return status_ == Status::ready || status_ == Status::failed;
    });
    return status_ == Status::ready;
  }

  [[nodiscard]] bool healthy() const {
    std::scoped_lock lock(mutex_);
    return status_ == Status::ready || status_ == Status::starting;
  }

  [[nodiscard]] bool ready() const {
    std::scoped_lock lock(mutex_);
    return status_ == Status::ready;
  }

  [[nodiscard]] bool failed() const {
    std::scoped_lock lock(mutex_);
    return status_ == Status::failed;
  }

  [[nodiscard]] std::string error() const {
    std::scoped_lock lock(mutex_);
    return error_;
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  Status status_ = Status::stopped;
  std::string error_;
};

}  // namespace echonull
