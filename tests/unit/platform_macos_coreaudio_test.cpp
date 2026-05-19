// SPDX-License-Identifier: MIT
// Psynder — lane 23 / platform-macos CoreAudio + gamepad smoke.
//
// These tests open the default audio device just long enough to confirm
// the AUHAL wiring is sound (start succeeds, the render callback fires
// at least once, stop is idempotent). CoreAudio is a real device on CI
// macs so we keep the test windowed in time and tolerant of unavailable
// hardware (e.g. headless runners): the test passes if start() either
// fully succeeds or cleanly refuses, never crashing.

#if defined(PSYNDER_PLATFORM_MACOS)

#include "platform/macos/MacPlatform_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace pm = psynder::platform::macos;

namespace {
std::atomic<unsigned> g_render_calls{0};
std::atomic<unsigned> g_total_frames{0};

void test_render_cb(void* /*user*/, psynder::f32* out, psynder::u32 frame_count,
                    psynder::u32 channel_count, psynder::u32 /*sample_rate*/) {
    g_render_calls.fetch_add(1, std::memory_order_relaxed);
    g_total_frames.fetch_add(frame_count, std::memory_order_relaxed);
    // Emit silence — we only want to confirm the callback is wired
    for (psynder::u32 f = 0; f < frame_count; ++f)
        for (psynder::u32 c = 0; c < channel_count; ++c)
            out[f * channel_count + c] = 0.0f;
}
}  // namespace

TEST_CASE("platform_macos: audio_start / audio_stop is safe", "[platform_macos][audio]") {
    g_render_calls.store(0);
    g_total_frames.store(0);
    pm::AudioDeviceDesc desc{};
    desc.sample_rate   = 48000;
    desc.channels      = 2;
    desc.buffer_frames = 512;
    const bool ok = pm::audio_start(desc, &test_render_cb, nullptr);
    // We do not REQUIRE ok — CI macs may not have an audio device available.
    // What we DO require: stop is a no-op if start failed, and the running
    // flag is consistent with the return value.
    REQUIRE(pm::audio_running() == ok);
    if (ok) {
        // Wait up to ~250 ms for the callback to fire — 48k/512 = ~10.6 ms/cb
        for (int i = 0; i < 25 && g_render_calls.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(g_render_calls.load() > 0);
        REQUIRE(g_total_frames.load() > 0);
        REQUIRE(pm::audio_actual_sample_rate() == 48000);
        REQUIRE(pm::audio_actual_channels()    == 2);
    }
    pm::audio_stop();
    REQUIRE_FALSE(pm::audio_running());
    // Double-stop must be safe (idempotent shutdown)
    pm::audio_stop();
    REQUIRE_FALSE(pm::audio_running());
}

TEST_CASE("platform_macos: gamepad_arm is idempotent + count is queryable",
          "[platform_macos][gamepad]") {
    pm::gamepad_arm();
    pm::gamepad_arm();   // second call must not crash / re-register
    // No controller is required for the test to pass; we just confirm the
    // count is reachable and stable across calls.
    auto a = pm::gamepad_count();
    auto b = pm::gamepad_count();
    REQUIRE(b >= a);   // count never goes backwards within a frame
}

#endif  // PSYNDER_PLATFORM_MACOS
