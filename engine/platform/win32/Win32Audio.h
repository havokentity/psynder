// SPDX-License-Identifier: MIT
// Psynder — WASAPI shared-mode event-driven audio render.
//
// Spins a worker thread that waits on the device buffer's "ready" event,
// asks a mixer-callback for `n` frames of interleaved float32 PCM, and
// writes them into the WASAPI buffer. The mixer callback is provided by
// lane 12 (audio); until lane 12 is implemented we install a silent
// fallback so the platform layer is exercised end-to-end.

#pragma once

#if defined(PSYNDER_PLATFORM_WIN32)

#include "Win32Common.h"

#include <atomic>
#include <thread>

namespace psynder::platform::win32 {

// Mixer callback signature — float32 interleaved, stride = channels.
// Returns the number of frames actually filled (must be <= frames). The
// audio thread will zero the remainder if the callback under-delivers.
using MixerCallback = u32 (*)(f32* out, u32 frames, u32 channels, u32 sample_rate, void* user);

class Win32Audio {
   public:
    Win32Audio() = default;
    ~Win32Audio() { stop(); }

    Win32Audio(const Win32Audio&) = delete;
    Win32Audio& operator=(const Win32Audio&) = delete;

    // Opens the default render endpoint in shared mode, starts the audio
    // thread. `prefer_sample_rate` is a hint; the actual format comes from
    // the device's mix format. Returns true on success.
    bool start(u32 prefer_sample_rate = 48000, u32 prefer_channels = 2);

    // Stops the thread, releases the WASAPI client.
    void stop();

    // Installs the mixer callback used by the audio thread. Setting it to
    // nullptr returns the device to silent fallback.
    void set_mixer(MixerCallback cb, void* user);

    bool running() const noexcept { return running_.load(std::memory_order_relaxed); }
    u32 sample_rate() const noexcept { return sample_rate_; }
    u32 channels() const noexcept { return channels_; }

   private:
    void thread_main();

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    HANDLE event_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};
    std::thread thread_;

    std::atomic<MixerCallback> mixer_cb_{nullptr};
    std::atomic<void*> mixer_user_{nullptr};

    u32 sample_rate_ = 0;
    u32 channels_ = 0;
    u32 buffer_frames_ = 0;
};

// Process-wide singleton. The shim's create_window_impl boots audio on
// first use; lane 12 installs the mixer callback via audio_set_mixer().
Win32Audio& audio_singleton();

// Lane-12 facing API — installs / clears the mixer callback. Safe to call
// before audio is started (the callback is latched and used once start()
// completes).
void audio_set_mixer(MixerCallback cb, void* user);

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
