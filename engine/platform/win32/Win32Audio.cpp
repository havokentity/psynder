// SPDX-License-Identifier: MIT
// Psynder — WASAPI shared-mode event-driven audio render.

#include "Win32Audio.h"

#if defined(PSYNDER_PLATFORM_WIN32)

#include "audio/internal/Backend.h"
#include "core/Log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace psynder::platform::win32 {

namespace {

// IIDs we resolve at runtime. C++ binding gives nice IIDs at compile time via
// __uuidof, but linking against mmdevapi.lib also provides them.
constexpr REFERENCE_TIME kHnsPerMs    = 10000LL;
constexpr REFERENCE_TIME kBufferDurationHns = 30 * kHnsPerMs;  // 30 ms target

// Zeroes a float buffer.
inline void zero_float(f32* p, u32 frames, u32 channels) {
    std::memset(p, 0, static_cast<usize>(frames) * channels * sizeof(f32));
}

}  // namespace

Win32Audio& audio_singleton() {
    static Win32Audio g_audio{};
    return g_audio;
}

void audio_set_mixer(MixerCallback cb, void* user) {
    audio_singleton().set_mixer(cb, user);
}

// ── Wave-B WASAPI ↔ lane-12 audio bridge ────────────────────────────────
//
// Lane 12's MixerCallback signature is `void(f32* stereo_interleaved, u32
// frames, void* user)` — stereo only. Lane 21's WASAPI thread expects
// `u32(f32* out, u32 frames, u32 channels, u32 sample_rate, void*)` and
// may run at any device channel count.
//
// The bridge holds:
//   * a stereo scratch buffer reused across every audio-thread pull (no
//     allocation in the hot path — `start()` reserves a high-water mark
//     and the bridge only grows it monotonically),
//   * the lane-12 callback + user pointer (atomic so a late install /
//     swap from the game thread is race-free).
//
// On each pull we run lane-12 to produce stereo, then copy/upmix into the
// device buffer. When the device is already stereo this is a single
// `memcpy` per pull.
struct WasapiBridge {
    std::atomic<psynder::audio::MixerCallback> cb{nullptr};
    std::atomic<void*>                          user{nullptr};
    // Stereo scratch — owned by the audio thread, sized once by start().
    // `std::vector` is used here only at start()-time; the hot path indexes
    // .data() and never reallocates because we reserve up-front.
    std::vector<f32> scratch_stereo;
};

WasapiBridge& bridge() {
    static WasapiBridge b;
    return b;
}

// 5-arg Win32 callback that adapts to lane 12's 2-arg stereo callback.
u32 bridge_adapter(f32*  out,
                   u32   frames,
                   u32   channels,
                   u32   /*sample_rate*/,
                   void* /*user*/) {
    auto& b = bridge();
    auto  cb = b.cb.load(std::memory_order_acquire);
    if (!cb) return 0u;  // leave the WASAPI-side silence in place

    const usize stereo_floats = static_cast<usize>(frames) * 2u;
    // Grow once if the device handed us a larger block than start() saw.
    // The audio thread is the only writer, so this is race-free.
    if (b.scratch_stereo.size() < stereo_floats) {
        b.scratch_stereo.resize(stereo_floats);
    }
    f32* stereo = b.scratch_stereo.data();
    void* u = b.user.load(std::memory_order_acquire);
    cb(stereo, frames, u);

    if (channels == 2u) {
        std::memcpy(out, stereo, stereo_floats * sizeof(f32));
    } else if (channels == 1u) {
        for (u32 i = 0; i < frames; ++i) {
            out[i] = 0.5f * (stereo[2u * i] + stereo[2u * i + 1u]);
        }
    } else {
        // Many-channel device (5.1/7.1): place stereo on front L/R, leave
        // the rest as the WASAPI-side zero. Channel layout for shared-mode
        // float on Windows is interleaved L, R, C, LFE, ...
        for (u32 i = 0; i < frames; ++i) {
            out[channels * i + 0u] = stereo[2u * i + 0u];
            out[channels * i + 1u] = stereo[2u * i + 1u];
        }
    }
    return frames;
}

void Win32Audio::set_mixer(MixerCallback cb, void* user) {
    mixer_cb_.store(cb, std::memory_order_release);
    mixer_user_.store(user, std::memory_order_release);
}

bool Win32Audio::start(u32 /*prefer_sample_rate*/, u32 /*prefer_channels*/) {
    if (running_.load()) return true;

    HRESULT hr = ::CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator_.GetAddressOf()));
    if (!psy_hr_ok(hr, "CoCreateInstance(MMDeviceEnumerator)")) return false;

    if (!psy_hr_ok(enumerator_->GetDefaultAudioEndpoint(
            eRender, eConsole, device_.GetAddressOf()),
            "GetDefaultAudioEndpoint")) return false;

    if (!psy_hr_ok(device_->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf())),
            "IMMDevice::Activate(IAudioClient)")) return false;

    // Pull the device's mix format — we render in float32 native format.
    WAVEFORMATEX* mix_fmt = nullptr;
    if (!psy_hr_ok(client_->GetMixFormat(&mix_fmt), "IAudioClient::GetMixFormat")) {
        return false;
    }
    // Hold a deleter for mix_fmt — Windows owns the memory until CoTaskMemFree.
    struct FmtGuard {
        WAVEFORMATEX* p;
        ~FmtGuard() { if (p) ::CoTaskMemFree(p); }
    } guard{mix_fmt};

    sample_rate_ = mix_fmt->nSamplesPerSec;
    channels_    = mix_fmt->nChannels;

    // Initialize the audio client in shared mode, event-driven.
    hr = client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        kBufferDurationHns,
        0,
        mix_fmt,
        nullptr);
    if (!psy_hr_ok(hr, "IAudioClient::Initialize")) return false;

    UINT32 frames = 0;
    if (!psy_hr_ok(client_->GetBufferSize(&frames),
                   "IAudioClient::GetBufferSize")) return false;
    buffer_frames_ = static_cast<u32>(frames);

    event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        PSY_LOG_ERROR("[win32] CreateEventW for WASAPI failed");
        return false;
    }
    if (!psy_hr_ok(client_->SetEventHandle(event_),
                   "IAudioClient::SetEventHandle")) {
        ::CloseHandle(event_);
        event_ = nullptr;
        return false;
    }

    if (!psy_hr_ok(client_->GetService(
            __uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(render_.GetAddressOf())),
            "IAudioClient::GetService(IAudioRenderClient)")) {
        return false;
    }

    // Prime with silence — required by WASAPI before Start().
    BYTE* data = nullptr;
    if (psy_hr_ok(render_->GetBuffer(buffer_frames_, &data), "RenderClient::GetBuffer(prime)")) {
        std::memset(data, 0, static_cast<usize>(buffer_frames_) * channels_ * sizeof(f32));
        render_->ReleaseBuffer(buffer_frames_, 0);
    }

    if (!psy_hr_ok(client_->Start(), "IAudioClient::Start")) return false;

    running_.store(true);
    stop_flag_.store(false);
    thread_ = std::thread([this] { thread_main(); });
    return true;
}

void Win32Audio::stop() {
    if (!running_.load()) return;
    stop_flag_.store(true);
    if (event_) ::SetEvent(event_);
    if (thread_.joinable()) thread_.join();

    if (client_) client_->Stop();
    render_.Reset();
    if (event_) {
        ::CloseHandle(event_);
        event_ = nullptr;
    }
    client_.Reset();
    device_.Reset();
    enumerator_.Reset();
    running_.store(false);
}

void Win32Audio::thread_main() {
    // The audio thread must call CoInitialize since WASAPI COM objects are
    // queried on this thread.
    ComScope com_scope;

    while (!stop_flag_.load(std::memory_order_acquire)) {
        const DWORD wait = ::WaitForSingleObject(event_, 200);
        if (wait != WAIT_OBJECT_0) continue;
        if (stop_flag_.load(std::memory_order_acquire)) break;

        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) continue;
        const u32 frames_avail = buffer_frames_ - padding;
        if (frames_avail == 0) continue;

        BYTE* raw = nullptr;
        if (FAILED(render_->GetBuffer(frames_avail, &raw))) continue;
        auto* out = reinterpret_cast<f32*>(raw);

        // Always start with silence — if the mixer under-delivers or there
        // is no mixer installed yet, this keeps the device happy with no clicks.
        zero_float(out, frames_avail, channels_);

        if (auto cb = mixer_cb_.load(std::memory_order_acquire)) {
            void* user = mixer_user_.load(std::memory_order_acquire);
            cb(out, frames_avail, channels_, sample_rate_, user);
        }

        render_->ReleaseBuffer(frames_avail, 0);
    }
}

}  // namespace psynder::platform::win32

// ── Strong overrides for lane 12's weak fallbacks ────────────────────────
// Lane 12 declares `audio::backend_init_wasapi` / `_shutdown_wasapi` weak so
// that when this TU is linked into the final binary our strong definitions
// take precedence and the lane-12 mixer pull is driven by real WASAPI.
namespace psynder::audio {

bool backend_init_wasapi(const DeviceDesc& desc, MixerCallback cb, void* user) noexcept {
    auto& b = psynder::platform::win32::bridge();
    b.cb.store(cb,   std::memory_order_release);
    b.user.store(user, std::memory_order_release);
    // Reserve enough stereo scratch up-front so the audio thread's first
    // pull doesn't allocate. `buffer_frames` is the WASAPI period in lane
    // 12's caller terms; the bridge adapter grows it monotonically if the
    // device hands us a larger block.
    const usize reserve = static_cast<usize>(desc.buffer_frames ? desc.buffer_frames : 512u) * 2u;
    if (b.scratch_stereo.size() < reserve) {
        b.scratch_stereo.resize(reserve);
    }
    auto& audio = psynder::platform::win32::audio_singleton();
    audio.set_mixer(&psynder::platform::win32::bridge_adapter, nullptr);
    if (!audio.running()) {
        // The window factory normally boots audio on first window. If
        // engine code starts audio without a window (e.g. headless tests
        // on a real Windows box), start it here too.
        if (!audio.start(desc.sample_rate ? desc.sample_rate : 48000u,
                         desc.channels    ? desc.channels    : 2u)) {
            PSY_LOG_WARN("[audio] WASAPI start failed inside backend_init_wasapi");
            return false;
        }
    }
    return true;
}

void backend_shutdown_wasapi() noexcept {
    auto& b = psynder::platform::win32::bridge();
    b.cb.store(nullptr,  std::memory_order_release);
    b.user.store(nullptr, std::memory_order_release);
    // Detach from the singleton so the audio thread stops calling the
    // bridge adapter; we don't tear the device down here — that happens
    // at process exit via the singleton's destructor.
    psynder::platform::win32::audio_singleton().set_mixer(nullptr, nullptr);
}

}  // namespace psynder::audio

#endif  // PSYNDER_PLATFORM_WIN32
