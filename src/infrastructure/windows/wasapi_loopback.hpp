#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "infrastructure/windows/wasapi_common.hpp"

namespace echonull {

class WasapiLoopbackCapture {
 public:
  using PacketHandler =
      std::function<void(std::int64_t, std::vector<float>, bool)>;

  explicit WasapiLoopbackCapture(std::wstring endpoint_id);
  ~WasapiLoopbackCapture();

  WasapiLoopbackCapture(const WasapiLoopbackCapture&) = delete;
  WasapiLoopbackCapture& operator=(const WasapiLoopbackCapture&) = delete;

  void start(PacketHandler handler);
  void stop() noexcept;
  [[nodiscard]] bool ready() const { return state_.ready(); }
  [[nodiscard]] bool failed() const { return state_.failed(); }
  [[nodiscard]] std::string error() const { return state_.error(); }

 private:
  void run() noexcept;
  static std::int64_t qpc_now_hns();

  std::wstring endpoint_id_;
  PacketHandler handler_;
  std::thread thread_;
  std::atomic<bool> stop_requested_{false};
  WorkerState state_;
};

}  // namespace echonull
