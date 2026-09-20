#pragma once

namespace echonull {

// Registers the calling thread with MMCSS once for the lifetime of that thread.
// The registration is automatically reverted when the thread exits.
void ensure_realtime_audio_thread_priority() noexcept;

}  // namespace echonull
