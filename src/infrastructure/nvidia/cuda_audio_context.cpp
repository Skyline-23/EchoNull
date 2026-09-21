#include "infrastructure/nvidia/cuda_audio_context.hpp"
#include <Windows.h>
#include <stdexcept>
#include <string>

namespace echonull {
namespace {
template <typename T> T resolve(HMODULE module, const char* name) {
  const auto address = GetProcAddress(module, name);
  if (!address) throw std::runtime_error(std::string("CUDA driver is missing ") + name);
  return reinterpret_cast<T>(address);
}
void check(const int result, const char* operation) {
  if (result != 0) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}
}
struct CudaAudioContext::Impl {
  using Init = int(WINAPI*)(unsigned int);
  using DeviceGet = int(WINAPI*)(int*, int);
  using Create = int(WINAPI*)(void**, unsigned int, int);
  using Destroy = int(WINAPI*)(void*);
  using SetCurrent = int(WINAPI*)(void*);
  HMODULE driver = nullptr;
  void* context = nullptr;
  Destroy destroy = nullptr;
  SetCurrent set_current = nullptr;
  ~Impl() {
    if (context && destroy) destroy(context);
    if (driver) FreeLibrary(driver);
  }
};
CudaAudioContext::CudaAudioContext() : impl_(std::make_unique<Impl>()) {
  impl_->driver = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!impl_->driver) throw std::runtime_error("NVIDIA CUDA driver is unavailable");
  const auto init = resolve<Impl::Init>(impl_->driver, "cuInit");
  const auto device_get = resolve<Impl::DeviceGet>(impl_->driver, "cuDeviceGet");
  const auto create = resolve<Impl::Create>(impl_->driver, "cuCtxCreate_v2");
  impl_->destroy = resolve<Impl::Destroy>(impl_->driver, "cuCtxDestroy_v2");
  impl_->set_current = resolve<Impl::SetCurrent>(impl_->driver, "cuCtxSetCurrent");
  check(init(0), "cuInit");
  int device = 0;
  check(device_get(&device, 0), "cuDeviceGet");
  // CU_CTX_SCHED_BLOCKING_SYNC avoids busy-spinning a critical CPU thread.
  check(create(&impl_->context, 4U, device), "cuCtxCreate");
}
CudaAudioContext::~CudaAudioContext() = default;
void CudaAudioContext::make_current() {
  check(impl_->set_current(impl_->context), "cuCtxSetCurrent");
}
}  // namespace echonull
