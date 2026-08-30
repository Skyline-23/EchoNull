#pragma once

#include <Windows.h>

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
    message << action << " failed (HRESULT 0x" << std::hex
            << static_cast<unsigned long>(result) << ')';
    throw std::runtime_error(message.str());
  }
}

class WorkerState {
 public:
  void set_starting() {
    std::scoped_lock lock(mutex_);
    ready_ = false;
    failed_ = false;
    error_.clear();
  }

  void set_ready() {
    std::scoped_lock lock(mutex_);
    ready_ = true;
    failed_ = false;
    error_.clear();
  }

  void set_stopped() {
    std::scoped_lock lock(mutex_);
    ready_ = false;
  }

  void fail(std::string error) {
    std::scoped_lock lock(mutex_);
    ready_ = false;
    failed_ = true;
    error_ = std::move(error);
  }

  [[nodiscard]] bool ready() const {
    std::scoped_lock lock(mutex_);
    return ready_;
  }

  [[nodiscard]] bool failed() const {
    std::scoped_lock lock(mutex_);
    return failed_;
  }

  [[nodiscard]] std::string error() const {
    std::scoped_lock lock(mutex_);
    return error_;
  }

 private:
  mutable std::mutex mutex_;
  bool ready_ = false;
  bool failed_ = false;
  std::string error_;
};

}  // namespace echonull
