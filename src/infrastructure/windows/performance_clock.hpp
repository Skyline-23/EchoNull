#pragma once

#include <Windows.h>

#include <cstdint>

namespace echonull {

inline std::int64_t performance_timestamp_hns() noexcept {
  static const LONGLONG frequency = [] {
    LARGE_INTEGER value{};
    return QueryPerformanceFrequency(&value) ? value.QuadPart : 0LL;
  }();
  if (frequency == 0) return 0;

  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<std::int64_t>(
      static_cast<long double>(counter.QuadPart) * 10'000'000.0L /
      static_cast<long double>(frequency));
}

}  // namespace echonull
