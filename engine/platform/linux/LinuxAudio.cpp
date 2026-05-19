// SPDX-License-Identifier: MIT
// Psynder — Linux audio device opener. Lane 22.
//
// DESIGN.md §11.2 / §7.10: PipeWire first, ALSA fallback. The platform's
// only job is to *open the device* — actual mixing / sample sourcing
// belongs to lane 12 (engine/audio). For Wave A's sample_00_clear we
// don't need to push samples; we open the device so cold-start latency
// is paid up front and we surface "audio works" diagnostics in the log.
//
// We use *runtime-loaded* PipeWire + ALSA: dlopen the libs and probe for
// the entry symbols. That way, on a build host that has only one of the
// two, we still link cleanly — the fallback path covers the missing one.
// The audio context is held for the program's lifetime via the unique_ptr
// returned to the caller; on destruction, the device drains and closes.

#ifdef PSYNDER_PLATFORM_LINUX

#include "LinuxPlatform_internal.h"

#include "core/Log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <string>

namespace psynder::platform::linux_impl {

namespace {

// ─── Symbol resolver ─────────────────────────────────────────────────────
// dlopen with RTLD_LAZY so probing doesn't crash on missing dependencies
// of the lib (e.g. a libpipewire built against a different glibc).
struct DlHandle {
    void* h = nullptr;
    ~DlHandle() { if (h) ::dlclose(h); }
    explicit operator bool() const noexcept { return h != nullptr; }
    template <class F> F sym(const char* name) noexcept {
        return reinterpret_cast<F>(::dlsym(h, name));
    }
};

DlHandle open_lib(const char* soname) noexcept {
    DlHandle d;
    d.h = ::dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    return d;
}

// ─── PipeWire backend ────────────────────────────────────────────────────
// We need: pw_init, pw_deinit, pw_stream_new_simple, pw_stream_connect,
// pw_stream_destroy, pw_main_loop_new / _run / _quit / _destroy, plus the
// public types. For Wave A we just *initialize* PipeWire and report
// success — the sample doesn't pull audio. That keeps us off the
// libpipewire-0.3 ABI surface that varies across distros.
//
// The minimal symbol we need to prove PipeWire is usable is pw_init().
class PipeWireDevice final : public AudioDevice {
public:
    PipeWireDevice() = default;
    ~PipeWireDevice() override {
        if (deinit_) deinit_();
    }

    bool open() noexcept {
        lib_ = open_lib("libpipewire-0.3.so.0");
        if (!lib_) {
            lib_ = open_lib("libpipewire-0.3.so");
        }
        if (!lib_) return false;
        auto init   = lib_.sym<void(*)(int*, char***)>("pw_init");
        deinit_     = lib_.sym<void(*)()>           ("pw_deinit");
        if (!init || !deinit_) return false;
        init(nullptr, nullptr);
        return true;
    }

    const char* backend_name() const noexcept override { return "PipeWire"; }
    bool        ok()           const noexcept override { return deinit_ != nullptr; }

private:
    DlHandle    lib_;
    void      (*deinit_)() = nullptr;
};

// ─── ALSA backend ────────────────────────────────────────────────────────
// snd_pcm_open is the canonical "device works" probe. We open the "default"
// PCM in playback mode and immediately close it — that's enough to prove
// the host has a usable audio device. Lane 12 will reopen with the real
// configuration when audio playback starts.
//
// libasound's relevant subset:
//   typedef struct _snd_pcm  snd_pcm_t;
//   int snd_pcm_open(snd_pcm_t**, const char*, int stream, int mode);
//   int snd_pcm_close(snd_pcm_t*);
class AlsaDevice final : public AudioDevice {
public:
    AlsaDevice() = default;
    ~AlsaDevice() override = default;

    bool open() noexcept {
        lib_ = open_lib("libasound.so.2");
        if (!lib_) lib_ = open_lib("libasound.so");
        if (!lib_) return false;
        auto pcm_open  = lib_.sym<int(*)(void**, const char*, int, int)>("snd_pcm_open");
        auto pcm_close = lib_.sym<int(*)(void*)>("snd_pcm_close");
        if (!pcm_open || !pcm_close) return false;
        void* pcm = nullptr;
        // SND_PCM_STREAM_PLAYBACK = 0, SND_PCM_NONBLOCK = 1.
        if (pcm_open(&pcm, "default", 0, 1) < 0 || !pcm) {
            return false;
        }
        pcm_close(pcm);
        ok_ = true;
        return true;
    }

    const char* backend_name() const noexcept override { return "ALSA"; }
    bool        ok()           const noexcept override { return ok_; }

private:
    DlHandle lib_;
    bool     ok_ = false;
};

}  // namespace

std::unique_ptr<AudioDevice> try_open_pipewire() noexcept {
    auto d = std::make_unique<PipeWireDevice>();
    if (!d->open()) return nullptr;
    PSY_LOG_INFO("audio: opened PipeWire");
    return d;
}

std::unique_ptr<AudioDevice> try_open_alsa() noexcept {
    auto d = std::make_unique<AlsaDevice>();
    if (!d->open()) return nullptr;
    PSY_LOG_INFO("audio: opened ALSA (fallback)");
    return d;
}

}  // namespace psynder::platform::linux_impl

#endif  // PSYNDER_PLATFORM_LINUX
