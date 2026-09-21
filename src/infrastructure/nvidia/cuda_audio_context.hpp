#pragma once
#include <memory>

namespace echonull {
// One worker-owned context for both effects. All NvAFX handles must be destroyed
// on that worker before this context. Uses only the installed NVIDIA driver.
class CudaAudioContext {
 public:
  CudaAudioContext();
  ~CudaAudioContext();
  CudaAudioContext(const CudaAudioContext&) = delete;
  CudaAudioContext& operator=(const CudaAudioContext&) = delete;
  void make_current();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace echonull
