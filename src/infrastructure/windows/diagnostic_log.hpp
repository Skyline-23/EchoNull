#pragma once

#include <memory>

#include "infrastructure/windows/telemetry_bus.hpp"

namespace echonull {

// Persists coalesced runtime diagnostics on a low-priority worker. publish()
// never waits for the writer and is safe to call from the audio callback.
class AsyncDiagnosticLog {
 public:
  AsyncDiagnosticLog();
  ~AsyncDiagnosticLog();

  AsyncDiagnosticLog(const AsyncDiagnosticLog&) = delete;
  AsyncDiagnosticLog& operator=(const AsyncDiagnosticLog&) = delete;

  void open();
  void close() noexcept;
  void publish(const TelemetrySnapshot& snapshot) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace echonull
