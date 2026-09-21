#include "infrastructure/windows/gpu_priority.hpp"
#include <Windows.h>
#include <mutex>

namespace echonull {
namespace {
using GetPriority = LONG(WINAPI*)(HANDLE, int*);
using SetPriority = LONG(WINAPI*)(HANDLE, int);
constexpr int kHighGpuPriority = 4;
struct ProcessPriority {
  std::mutex mutex;
  HMODULE module = nullptr;
  GetPriority get = nullptr;
  SetPriority set = nullptr;
  unsigned int users = 0;
  int original = 2;
  int observed = 0;
  LONG status = 0;
  bool changed = false;
};
ProcessPriority& shared() { static ProcessPriority value; return value; }
}
GpuPriority::GpuPriority(const bool enabled) noexcept {
  if (!enabled) return;
  auto& state = shared();
  std::scoped_lock lock(state.mutex);
  if (state.users++ == 0) {
    state.module = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (state.module) {
      state.get = reinterpret_cast<GetPriority>(GetProcAddress(state.module, "D3DKMTGetProcessSchedulingPriorityClass"));
      state.set = reinterpret_cast<SetPriority>(GetProcAddress(state.module, "D3DKMTSetProcessSchedulingPriorityClass"));
    }
    state.changed = false;
    state.observed = 0;
    state.status = static_cast<LONG>(0xC000007A);
    if (state.get && state.set) {
      state.status = state.get(GetCurrentProcess(), &state.original);
      if (state.status == 0) {
        state.observed = state.original;
        if (state.original < kHighGpuPriority) {
          state.status = state.set(GetCurrentProcess(), kHighGpuPriority);
          state.changed = state.status == 0;
          const LONG query = state.get(GetCurrentProcess(), &state.observed);
          if (state.status == 0 && query != 0) state.status = query;
        }
      }
    }
  }
  acquired_ = true;
  status_ = static_cast<std::uint32_t>(state.status);
  observed_ = static_cast<std::uint32_t>(state.observed);
}
GpuPriority::~GpuPriority() {
  if (!acquired_) return;
  auto& state = shared();
  std::scoped_lock lock(state.mutex);
  if (--state.users != 0) return;
  if (state.changed) {
    int current = 0;
    if (state.get(GetCurrentProcess(), &current) == 0 && current == kHighGpuPriority) {
      state.set(GetCurrentProcess(), state.original);
    }
  }
  if (state.module) FreeLibrary(state.module);
  state.module = nullptr;
  state.get = nullptr;
  state.set = nullptr;
}
}  // namespace echonull
