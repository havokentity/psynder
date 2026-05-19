// SPDX-License-Identifier: MIT
// Psynder — WASAPI shared-mode event-driven audio render.

#include "Win32Audio.h"

#if defined(PSYNDER_PLATFORM_WIN32)

#include <algorithm>
#include <cstring>

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

#endif  // PSYNDER_PLATFORM_WIN32
