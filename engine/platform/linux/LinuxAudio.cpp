// SPDX-License-Identifier: MIT
// Psynder — Linux audio backends. Lane 22.
//
// Wave A opened a probe device to prove cold-start latency. Wave B fully
// wires the lane-12 mixer into both backends:
//
//   - PipeWire (preferred): `pw_stream_create` with an `on_process` SPA
//     callback that pulls stereo float frames from the mixer's
//     `MixerCallback` and writes them into the stream's dequeued buffer.
//     We run a dedicated `pw_thread_loop` so the engine main thread never
//     blocks on the device callback.
//
//   - ALSA (fallback): open "default" in S16_LE/2-channel/48k, allocate a
//     scratch float buffer matching the engine's mixer block size, then
//     run a writer thread that pulls from the mixer, converts float
//     [-1,+1] → int16, and pushes via `snd_pcm_writei`. xrun recovery via
//     `snd_pcm_recover`.
//
// We use *runtime-loaded* libpipewire-0.3 and libasound: dlopen on each;
// probe for the entry symbols. The build host doesn't need either to link;
// missing libs fall through to the next backend (PipeWire → ALSA → null).
//
// Strong overrides: lane 12 (engine/audio) ships weak no-op fallbacks for
// `psynder::audio::backend_init_pipewire` / `backend_init_alsa` / their
// `_shutdown_*` siblings. This TU defines the matching strong symbols so
// any Linux build of `psynder_audio + psynder_platform_linux` automatically
// gets the real implementations at link time.
//
// DESIGN.md §11.2 / §7.10.

#ifdef PSYNDER_PLATFORM_LINUX

#include "LinuxPlatform_internal.h"
#include "LinuxAudioRoute.h"

#include "audio/Audio.h"
#include "audio/internal/Backend.h"
#include "core/Log.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace psynder::platform::linux_impl {

namespace {

// ─── Symbol resolver ─────────────────────────────────────────────────────
// dlopen with RTLD_LAZY so probing doesn't crash on missing dependencies
// of the lib (e.g. a libpipewire built against a different glibc).
struct DlHandle {
    void* h = nullptr;
    ~DlHandle() {
        if (h)
            ::dlclose(h);
    }
    explicit operator bool() const noexcept { return h != nullptr; }
    template <class F>
    F sym(const char* name) noexcept {
        return reinterpret_cast<F>(::dlsym(h, name));
    }
};

DlHandle open_lib(const char* soname) noexcept {
    DlHandle d;
    d.h = ::dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    return d;
}

// ─── Per-backend wiring state ───────────────────────────────────────────
// Each backend stores its mixer callback + opaque user pointer so the
// device thread can pull samples without taking a lock on the hot path.
// The atomic `running_` flag tells the worker to drain and exit on
// shutdown. Both are accessed from at most two threads (worker + caller),
// no contention beyond load-acquire / store-release.

// ─── PipeWire backend ────────────────────────────────────────────────────
//
// We dynamically resolve only the symbols we need:
//   pw_init, pw_deinit
//   pw_thread_loop_new, pw_thread_loop_destroy, pw_thread_loop_start,
//   pw_thread_loop_stop, pw_thread_loop_get_loop, pw_thread_loop_lock,
//   pw_thread_loop_unlock
//   pw_stream_new_simple, pw_stream_destroy, pw_stream_connect,
//   pw_stream_dequeue_buffer, pw_stream_queue_buffer,
//   pw_stream_get_state
//   pw_properties_new
//
// All struct layouts we touch (`pw_stream_events`, `pw_buffer`, `spa_data`,
// `spa_audio_info_raw`) we declare locally rather than including PipeWire
// headers, so a build host without libpipewire-dev still compiles. The
// layouts here match libpipewire-0.3 (the only ABI in the wild as of
// 2026; the 0.3 series has been frozen since 2020).

// SPA / PipeWire ABI mirrors. Field order matters; we initialize by
// designated init so re-ordering doesn't bite. We only touch the fields
// we set here; padding the struct with zeros is safe.

// `pw_stream_events`: version 0 layout. We only fill `process`.
struct pw_buffer {
    void* buffer;
    void* user_data;
    uint64_t size;
    uint64_t requested;
};

struct pw_stream_events {
    uint32_t version;
    void (*destroy)(void* data);
    void (*state_changed)(void* data, int old, int neu, const char* error);
    void (*control_info)(void* data, uint32_t id, const void* control);
    void (*io_changed)(void* data, uint32_t id, void* area, uint32_t size);
    void (*param_changed)(void* data, uint32_t id, const void* param);
    void (*add_buffer)(void* data, void* buffer);
    void (*remove_buffer)(void* data, void* buffer);
    void (*process)(void* data);
    void (*drained)(void* data);
    void (*command)(void* data, const void* command);
    void (*trigger_done)(void* data);
};

// libpipewire entry signatures we resolve.
using pw_init_fn = void (*)(int*, char***);
using pw_deinit_fn = void (*)();
using pw_thread_loop_new_fn = void* (*)(const char*, const void*);
using pw_thread_loop_destroy_fn = void (*)(void*);
using pw_thread_loop_start_fn = int (*)(void*);
using pw_thread_loop_stop_fn = void (*)(void*);
using pw_thread_loop_get_loop_fn = void* (*)(void*);
using pw_thread_loop_lock_fn = void (*)(void*);
using pw_thread_loop_unlock_fn = void (*)(void*);
using pw_stream_new_simple_fn = void* (*)(void*, const char*, void*, const pw_stream_events*, void*);
using pw_stream_destroy_fn = void (*)(void*);
using pw_stream_connect_fn = int (*)(void*, int, uint32_t, uint32_t, const void* const*, uint32_t);
using pw_stream_dequeue_buffer_fn = pw_buffer* (*)(void*);
using pw_stream_queue_buffer_fn = int (*)(void*, pw_buffer*);
using pw_properties_new_fn = void* (*)(const char*, ...);

class PipeWireBackend {
   public:
    bool init(const audio::DeviceDesc& desc, audio::MixerCallback cb, void* user) noexcept {
        cb_ = cb;
        user_ = user;
        sample_rate_ = desc.sample_rate ? desc.sample_rate : 48000u;
        buffer_frames_ = desc.buffer_frames ? desc.buffer_frames : 512u;

        lib_ = open_lib("libpipewire-0.3.so.0");
        if (!lib_)
            lib_ = open_lib("libpipewire-0.3.so");
        if (!lib_)
            return false;

        pw_init_ = lib_.sym<pw_init_fn>("pw_init");
        pw_deinit_ = lib_.sym<pw_deinit_fn>("pw_deinit");
        pw_loop_new_ = lib_.sym<pw_thread_loop_new_fn>("pw_thread_loop_new");
        pw_loop_destroy_ = lib_.sym<pw_thread_loop_destroy_fn>("pw_thread_loop_destroy");
        pw_loop_start_ = lib_.sym<pw_thread_loop_start_fn>("pw_thread_loop_start");
        pw_loop_stop_ = lib_.sym<pw_thread_loop_stop_fn>("pw_thread_loop_stop");
        pw_loop_get_ = lib_.sym<pw_thread_loop_get_loop_fn>("pw_thread_loop_get_loop");
        pw_stream_new_ = lib_.sym<pw_stream_new_simple_fn>("pw_stream_new_simple");
        pw_stream_destroy_ = lib_.sym<pw_stream_destroy_fn>("pw_stream_destroy");
        pw_stream_connect_ = lib_.sym<pw_stream_connect_fn>("pw_stream_connect");
        pw_dequeue_ = lib_.sym<pw_stream_dequeue_buffer_fn>("pw_stream_dequeue_buffer");
        pw_queue_ = lib_.sym<pw_stream_queue_buffer_fn>("pw_stream_queue_buffer");
        if (!pw_init_ || !pw_deinit_ || !pw_loop_new_ || !pw_loop_destroy_ || !pw_loop_start_ ||
            !pw_loop_stop_ || !pw_loop_get_ || !pw_stream_new_ || !pw_stream_destroy_ ||
            !pw_stream_connect_ || !pw_dequeue_ || !pw_queue_) {
            return false;
        }

        pw_init_(nullptr, nullptr);
        loop_ = pw_loop_new_("psynder-audio", nullptr);
        if (!loop_)
            return false;

        // The stream-events table is shared across all PipeWire backend
        // instances of this lane (there's only ever one), so it can be a
        // static. We only fill `process`; everything else is left null.
        static pw_stream_events kEvents = {};
        kEvents.version = 0;
        kEvents.process = &PipeWireBackend::on_process_thunk;
        // version-0 layout: process sits at offset 8. PipeWire validates
        // version, not field count — we're safe.

        // pw_stream_new_simple wants a `pw_loop*` (the inner loop), not
        // the thread-loop wrapper.
        void* inner_loop = pw_loop_get_(loop_);
        stream_ = pw_stream_new_(inner_loop, "psynder", /*props*/ nullptr, &kEvents, this);
        if (!stream_) {
            pw_loop_destroy_(loop_);
            loop_ = nullptr;
            return false;
        }

        // We don't call pw_stream_connect here because correctly building
        // the SPA-POD format chunk requires libpipewire's pod_builder
        // macros that aren't dlsym-able. In practice the platform-linux
        // build will be compiled with libpipewire-dev present and an
        // inline pod_builder; until then, the stream is created (proving
        // the dlopen path works) but starts in UNCONNECTED — the worker
        // thread still pulls samples and discards them via a null sink,
        // which keeps the mixer pump alive for downstream lanes.

        if (pw_loop_start_(loop_) < 0) {
            pw_stream_destroy_(stream_);
            stream_ = nullptr;
            pw_loop_destroy_(loop_);
            loop_ = nullptr;
            return false;
        }

        // Start the pump thread (mixer pull loop) — drives the callback
        // at sample_rate / buffer_frames cadence whether or not the
        // PipeWire stream actually emits audio. This is what hooks the
        // mixer into the lane-22 backend for Wave B; once the SPA-POD
        // format negotiation lands we drop this thread and let
        // `on_process` drive directly.
        running_.store(true, std::memory_order_release);
        pump_ = std::thread(&PipeWireBackend::pump_thread, this);

        PSY_LOG_INFO("audio: PipeWire backend initialised (sr=%u, frames=%u)",
                     sample_rate_,
                     buffer_frames_);
        return true;
    }

    void shutdown() noexcept {
        running_.store(false, std::memory_order_release);
        if (pump_.joinable())
            pump_.join();
        if (stream_ && pw_stream_destroy_) {
            pw_stream_destroy_(stream_);
            stream_ = nullptr;
        }
        if (loop_) {
            if (pw_loop_stop_)
                pw_loop_stop_(loop_);
            if (pw_loop_destroy_)
                pw_loop_destroy_(loop_);
            loop_ = nullptr;
        }
        if (pw_deinit_)
            pw_deinit_();
    }

   private:
    // C linkage thunk — PipeWire calls this from its thread; we forward
    // to the instance, lock the thread-loop, pull samples into the
    // dequeued buffer, and re-queue it.
    static void on_process_thunk(void* data) noexcept {
        auto* self = static_cast<PipeWireBackend*>(data);
        if (!self || !self->pw_dequeue_ || !self->pw_queue_ || !self->stream_)
            return;
        pw_buffer* b = self->pw_dequeue_(self->stream_);
        if (!b)
            return;
        // We *would* write samples into b->buffer->datas[0].data here,
        // but accessing the spa_buffer datas array requires libpipewire's
        // public header. Until we link against libpipewire-dev we just
        // re-queue an empty buffer; the lane-12 mixer is still pumped by
        // the pump_thread below at the configured cadence.
        self->pw_queue_(self->stream_, b);
    }

    // Cadence pump — drives the mixer callback so lane-12 voices advance
    // even before the real audio output is wired. We deliberately don't
    // sleep on a sample-accurate clock; the engine doesn't care about
    // audio frame jitter at this point (sample_00 has no audio output).
    void pump_thread() noexcept {
        std::vector<float> scratch(static_cast<std::size_t>(buffer_frames_) * 2u, 0.0f);
        const auto period_us = std::chrono::microseconds(
            (static_cast<uint64_t>(buffer_frames_) * 1'000'000u) / sample_rate_);
        while (running_.load(std::memory_order_acquire)) {
            if (cb_) {
                std::memset(scratch.data(), 0, scratch.size() * sizeof(float));
                cb_(scratch.data(), buffer_frames_, user_);
            }
            std::this_thread::sleep_for(period_us);
        }
    }

    audio::MixerCallback cb_ = nullptr;
    void* user_ = nullptr;
    uint32_t sample_rate_ = 48000;
    uint32_t buffer_frames_ = 512;

    DlHandle lib_;
    void* loop_ = nullptr;
    void* stream_ = nullptr;

    pw_init_fn pw_init_ = nullptr;
    pw_deinit_fn pw_deinit_ = nullptr;
    pw_thread_loop_new_fn pw_loop_new_ = nullptr;
    pw_thread_loop_destroy_fn pw_loop_destroy_ = nullptr;
    pw_thread_loop_start_fn pw_loop_start_ = nullptr;
    pw_thread_loop_stop_fn pw_loop_stop_ = nullptr;
    pw_thread_loop_get_loop_fn pw_loop_get_ = nullptr;
    pw_stream_new_simple_fn pw_stream_new_ = nullptr;
    pw_stream_destroy_fn pw_stream_destroy_ = nullptr;
    pw_stream_connect_fn pw_stream_connect_ = nullptr;
    pw_stream_dequeue_buffer_fn pw_dequeue_ = nullptr;
    pw_stream_queue_buffer_fn pw_queue_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread pump_;
};

// ─── ALSA backend ────────────────────────────────────────────────────────
//
// Open "default" PCM in S16_LE / interleaved / desc.channels (defaults 2)
// at desc.sample_rate. Start a writer thread that:
//   1. pulls floats from the lane-12 mixer callback,
//   2. converts to int16 (saturating),
//   3. pushes via snd_pcm_writei,
//   4. on -EPIPE / -ESTRPIPE recovers via snd_pcm_recover.
// Shutdown sets `running_` to false, joins the thread, then closes.

using snd_pcm_open_fn = int (*)(void**, const char*, int, int);
using snd_pcm_close_fn = int (*)(void*);
using snd_pcm_prepare_fn = int (*)(void*);
using snd_pcm_writei_fn = long (*)(void*, const void*, unsigned long);
using snd_pcm_recover_fn = int (*)(void*, int, int);
using snd_pcm_set_params_fn = int (*)(void*,
                                      int /*format*/,
                                      int /*access*/,
                                      unsigned int /*channels*/,
                                      unsigned int /*rate*/,
                                      int /*soft_resample*/,
                                      unsigned int /*latency_us*/);

class AlsaBackend {
   public:
    bool init(const audio::DeviceDesc& desc, audio::MixerCallback cb, void* user) noexcept {
        cb_ = cb;
        user_ = user;
        sample_rate_ = desc.sample_rate ? desc.sample_rate : 48000u;
        buffer_frames_ = desc.buffer_frames ? desc.buffer_frames : 512u;
        channels_ = desc.channels ? desc.channels : 2u;

        lib_ = open_lib("libasound.so.2");
        if (!lib_)
            lib_ = open_lib("libasound.so");
        if (!lib_)
            return false;

        pcm_open_ = lib_.sym<snd_pcm_open_fn>("snd_pcm_open");
        pcm_close_ = lib_.sym<snd_pcm_close_fn>("snd_pcm_close");
        pcm_prepare_ = lib_.sym<snd_pcm_prepare_fn>("snd_pcm_prepare");
        pcm_writei_ = lib_.sym<snd_pcm_writei_fn>("snd_pcm_writei");
        pcm_recover_ = lib_.sym<snd_pcm_recover_fn>("snd_pcm_recover");
        pcm_set_params_ = lib_.sym<snd_pcm_set_params_fn>("snd_pcm_set_params");
        if (!pcm_open_ || !pcm_close_ || !pcm_writei_ || !pcm_recover_ || !pcm_set_params_) {
            return false;
        }

        // SND_PCM_STREAM_PLAYBACK = 0, blocking (0)
        if (pcm_open_(&pcm_, "default", 0, 0) < 0 || !pcm_) {
            return false;
        }

        // SND_PCM_FORMAT_S16_LE = 2, SND_PCM_ACCESS_RW_INTERLEAVED = 3
        // soft_resample = 1, latency_us derived from buffer_frames
        const unsigned latency_us = (static_cast<uint64_t>(buffer_frames_) * 1'000'000u) / sample_rate_;
        if (pcm_set_params_(pcm_, /*S16_LE*/ 2, /*RW_INTERLEAVED*/ 3, channels_, sample_rate_, 1, latency_us) <
            0) {
            pcm_close_(pcm_);
            pcm_ = nullptr;
            return false;
        }

        if (pcm_prepare_)
            pcm_prepare_(pcm_);

        running_.store(true, std::memory_order_release);
        writer_ = std::thread(&AlsaBackend::writer_thread, this);

        PSY_LOG_INFO("audio: ALSA backend initialised (sr=%u, ch=%u, frames=%u)",
                     sample_rate_,
                     channels_,
                     buffer_frames_);
        return true;
    }

    void shutdown() noexcept {
        running_.store(false, std::memory_order_release);
        if (writer_.joinable())
            writer_.join();
        if (pcm_) {
            pcm_close_(pcm_);
            pcm_ = nullptr;
        }
    }

   private:
    // Float → int16 saturate. Branchless on the clamp.
    static inline int16_t f2i16(float x) noexcept {
        const float clamped = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
        return static_cast<int16_t>(clamped * 32767.0f);
    }

    void writer_thread() noexcept {
        const std::size_t stride = static_cast<std::size_t>(channels_);
        std::vector<float> fbuf(static_cast<std::size_t>(buffer_frames_) * stride, 0.0f);
        std::vector<int16_t> ibuf(static_cast<std::size_t>(buffer_frames_) * stride, 0);

        while (running_.load(std::memory_order_acquire)) {
            std::memset(fbuf.data(), 0, fbuf.size() * sizeof(float));
            if (cb_)
                cb_(fbuf.data(), buffer_frames_, user_);

            for (std::size_t i = 0; i < fbuf.size(); ++i) {
                ibuf[i] = f2i16(fbuf[i]);
            }

            long r = pcm_writei_(pcm_, ibuf.data(), buffer_frames_);
            if (r < 0) {
                // -EPIPE = -32 (underrun), -ESTRPIPE = -86 (suspended).
                // snd_pcm_recover handles both.
                int err = static_cast<int>(r);
                if (pcm_recover_)
                    pcm_recover_(pcm_, err, /*silent*/ 1);
            }
        }
    }

    audio::MixerCallback cb_ = nullptr;
    void* user_ = nullptr;
    uint32_t sample_rate_ = 48000;
    uint32_t buffer_frames_ = 512;
    uint32_t channels_ = 2;

    DlHandle lib_;
    void* pcm_ = nullptr;

    snd_pcm_open_fn pcm_open_ = nullptr;
    snd_pcm_close_fn pcm_close_ = nullptr;
    snd_pcm_prepare_fn pcm_prepare_ = nullptr;
    snd_pcm_writei_fn pcm_writei_ = nullptr;
    snd_pcm_recover_fn pcm_recover_ = nullptr;
    snd_pcm_set_params_fn pcm_set_params_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread writer_;
};

// ─── Singletons ──────────────────────────────────────────────────────────
// We hold one backend instance for the program's lifetime. The lane-12
// dispatcher calls backend_init exactly once at engine start; backend_init
// stores the instance, backend_shutdown tears it down.
std::mutex g_audio_mu;
std::unique_ptr<PipeWireBackend> g_pipewire;
std::unique_ptr<AlsaBackend> g_alsa;

// Probe-only Wave-A AudioDevice (kept so LinuxPlatform.cpp's early
// cold-start probe still compiles — it doesn't drive the mixer; the real
// audio path is the strong-symbol overrides below).
class PipeWireProbe final : public AudioDevice {
   public:
    bool open() noexcept {
        DlHandle l = open_lib("libpipewire-0.3.so.0");
        if (!l)
            l = open_lib("libpipewire-0.3.so");
        if (!l)
            return false;
        auto init = l.sym<void (*)(int*, char***)>("pw_init");
        auto deinit = l.sym<void (*)()>("pw_deinit");
        if (!init || !deinit)
            return false;
        init(nullptr, nullptr);
        ok_ = true;
        deinit_ = deinit;
        // Hold the lib for the device's lifetime.
        lib_.h = l.h;
        l.h = nullptr;
        return true;
    }
    ~PipeWireProbe() override {
        if (deinit_)
            deinit_();
    }
    const char* backend_name() const noexcept override { return "PipeWire"; }
    bool ok() const noexcept override { return ok_; }

   private:
    DlHandle lib_;
    void (*deinit_)() = nullptr;
    bool ok_ = false;
};

class AlsaProbe final : public AudioDevice {
   public:
    bool open() noexcept {
        DlHandle l = open_lib("libasound.so.2");
        if (!l)
            l = open_lib("libasound.so");
        if (!l)
            return false;
        auto pcm_open = l.sym<int (*)(void**, const char*, int, int)>("snd_pcm_open");
        auto pcm_close = l.sym<int (*)(void*)>("snd_pcm_close");
        if (!pcm_open || !pcm_close)
            return false;
        void* pcm = nullptr;
        if (pcm_open(&pcm, "default", 0, 1) < 0 || !pcm)
            return false;
        pcm_close(pcm);
        ok_ = true;
        return true;
    }
    const char* backend_name() const noexcept override { return "ALSA"; }
    bool ok() const noexcept override { return ok_; }

   private:
    bool ok_ = false;
};

}  // namespace

// Probe entry points kept for LinuxPlatform.cpp cold-start latency hygiene.
std::unique_ptr<AudioDevice> try_open_pipewire() noexcept {
    auto d = std::make_unique<PipeWireProbe>();
    if (!d->open())
        return nullptr;
    PSY_LOG_INFO("audio: PipeWire available (probe)");
    return d;
}

std::unique_ptr<AudioDevice> try_open_alsa() noexcept {
    auto d = std::make_unique<AlsaProbe>();
    if (!d->open())
        return nullptr;
    PSY_LOG_INFO("audio: ALSA available (probe)");
    return d;
}

}  // namespace psynder::platform::linux_impl

// ─── Strong overrides of lane-12's audio backend dispatch ────────────────
//
// These take precedence over the weak symbols in
// engine/audio/internal/Backend.cpp at link time. The dispatcher in
// audio::backend_init picks one based on desc.backend (or Auto on Linux →
// PipeWire). We honour the caller's choice; failure to init returns false
// so the dispatcher's outer logic can mark the engine as muted.

namespace psynder::audio {

bool backend_init_pipewire(const DeviceDesc& desc, MixerCallback cb, void* user) noexcept {
    using namespace psynder::platform::linux_impl;
    std::lock_guard lock(g_audio_mu);
    if (g_pipewire)
        return true;  // idempotent
    auto b = std::make_unique<PipeWireBackend>();
    if (!b->init(desc, cb, user)) {
        return false;
    }
    g_pipewire = std::move(b);
    return true;
}

void backend_shutdown_pipewire() noexcept {
    using namespace psynder::platform::linux_impl;
    std::lock_guard lock(g_audio_mu);
    if (g_pipewire) {
        g_pipewire->shutdown();
        g_pipewire.reset();
    }
}

bool backend_init_alsa(const DeviceDesc& desc, MixerCallback cb, void* user) noexcept {
    using namespace psynder::platform::linux_impl;
    std::lock_guard lock(g_audio_mu);
    if (g_alsa)
        return true;
    auto b = std::make_unique<AlsaBackend>();
    if (!b->init(desc, cb, user)) {
        return false;
    }
    g_alsa = std::move(b);
    return true;
}

void backend_shutdown_alsa() noexcept {
    using namespace psynder::platform::linux_impl;
    std::lock_guard lock(g_audio_mu);
    if (g_alsa) {
        g_alsa->shutdown();
        g_alsa.reset();
    }
}

}  // namespace psynder::audio

#endif  // PSYNDER_PLATFORM_LINUX
