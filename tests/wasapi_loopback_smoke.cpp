#include <Windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "infrastructure/windows/wasapi_loopback.hpp"

int wmain(const int argc, wchar_t** argv) {
  try {
    const std::wstring endpoint = argc > 1 ? argv[1] : L"default";
    std::atomic<std::uint64_t> packets{0};
    echonull::WasapiLoopbackCapture capture(endpoint);
    capture.start([&](std::int64_t, std::vector<float>, bool) {
      ++packets;
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (!capture.ready() && !capture.failed() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (capture.failed()) throw std::runtime_error(capture.error());
    if (!capture.ready()) {
      throw std::runtime_error("WASAPI loopback did not become ready");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    capture.stop();
    std::cout << "WASAPI loopback ready; packets=" << packets.load() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "WASAPI loopback smoke failure: " << error.what() << '\n';
    return 1;
  }
}
