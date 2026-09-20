#include "infrastructure/windows/realtime_audio_thread.hpp"

#include <Windows.h>
#include <Avrt.h>

namespace echonull {
namespace {

class MmcssAudioRegistration {
 public:
  MmcssAudioRegistration() noexcept {
    DWORD task_index = 0;
    handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (handle_ == nullptr) {
      task_index = 0;
      handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
    }
    if (handle_ != nullptr) {
      AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL);
    }
  }

  ~MmcssAudioRegistration() {
    if (handle_ != nullptr) {
      AvRevertMmThreadCharacteristics(handle_);
    }
  }

  MmcssAudioRegistration(const MmcssAudioRegistration&) = delete;
  MmcssAudioRegistration& operator=(const MmcssAudioRegistration&) = delete;

 private:
  HANDLE handle_ = nullptr;
};

}  // namespace

void ensure_realtime_audio_thread_priority() noexcept {
  thread_local MmcssAudioRegistration registration;
  (void)registration;
}

}  // namespace echonull
