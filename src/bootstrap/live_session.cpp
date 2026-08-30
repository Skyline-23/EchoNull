#include "bootstrap/live_session.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <thread>
#include <utility>

#include "infrastructure/nvidia/nvafx_aec.hpp"
#include "infrastructure/windows/wasapi_capture.hpp"
#include "infrastructure/windows/wasapi_renderer.hpp"

namespace echonull {

LiveSession::LiveSession(Config config, SnapshotHandler snapshot_handler, EngineHooks hooks)
    : config_(std::move(config)),
      snapshot_handler_(std::move(snapshot_handler)),
      hooks_(std::move(hooks)) {}

LiveSession::~LiveSession() {
  stop();
}

void LiveSession::start() {
  stop();
  stop_requested_ = false;
  running_ = true;
  thread_ = std::thread([this] { worker(); });
}

void LiveSession::stop() {
  stop_requested_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

EngineSnapshot LiveSession::snapshot() const {
  std::scoped_lock lock(snapshot_mutex_);
  return snapshot_;
}

void LiveSession::deliver(const EngineSnapshot& snapshot) {
  {
    std::scoped_lock lock(snapshot_mutex_);
    snapshot_ = snapshot;
  }
  if (snapshot_handler_) snapshot_handler_(snapshot);
}

void LiveSession::worker() {
  while (!stop_requested_) {
    try {
      WasapiCapture microphone(config_.devices.microphone, false, config_.sample_rate);
      WasapiCapture reference(config_.devices.reference, true, config_.sample_rate);
      WasapiRenderer output(config_.devices.output, config_.sample_rate);
      NvafxAec aec(config_.resolve_model_path(), config_.intensity, config_.sample_rate);
      Engine engine(config_.engine_settings(), microphone, reference, output, aec,
                    [this](const EngineSnapshot& value) { deliver(value); });
      engine.run(stop_requested_, hooks_);
    } catch (const std::exception& error) {
      EngineSnapshot failed = snapshot();
      failed.running = false;
      failed.message = error.what();
      deliver(failed);
      if (stop_requested_) break;
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(config_.reconnect_delay_ms);
      while (!stop_requested_ && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }
  running_ = false;
}

}  // namespace echonull

