#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include "application/engine.hpp"
#include "infrastructure/config.hpp"

namespace echonull {

class LiveSession {
 public:
  LiveSession(Config config, SnapshotHandler snapshot_handler, EngineHooks hooks = {});
  ~LiveSession();

  LiveSession(const LiveSession&) = delete;
  LiveSession& operator=(const LiveSession&) = delete;

  void start();
  void stop();
  [[nodiscard]] bool running() const { return running_; }
  [[nodiscard]] EngineSnapshot snapshot() const;

 private:
  void worker();
  void deliver(const EngineSnapshot& snapshot);

  Config config_;
  SnapshotHandler snapshot_handler_;
  EngineHooks hooks_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::thread thread_;
  mutable std::mutex snapshot_mutex_;
  EngineSnapshot snapshot_;
};

}  // namespace echonull

