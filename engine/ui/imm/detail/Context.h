// SPDX-License-Identifier: MIT
// Psynder — immediate-mode UI per-frame context. Lane 16.
//
// The IMM lives in a single global context (DESIGN.md §10.6 explicitly
// keeps the surface "small"). The context tracks: the active framebuffer
// for the frame, pointer input state, the active+hot widget IDs, and a
// short ring of perf samples used by `graph()`.
//
// The context is exposed via internal headers; the public `Imm.h` surface
// hides it behind free-function wrappers. The test suite includes this
// header directly to drive button hit-tests without engine boot.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace psynder::ui::imm::detail {

inline constexpr u32 kPerfGraphSamples = 128;

struct InputState {
    math::Vec2 mouse{0.0f, 0.0f};
    bool       mouse_down      = false;
    bool       mouse_down_prev = false;

    constexpr bool just_pressed()  const noexcept { return mouse_down && !mouse_down_prev; }
    constexpr bool just_released() const noexcept { return !mouse_down && mouse_down_prev; }
};

// Bag of state the imm context carries across the frame. Per DESIGN.md
// §3.4 no shared_ptr / no per-frame new-delete — this is a plain POD held
// as a function-local static.
struct Context {
    render::Framebuffer* target = nullptr;
    InputState           input{};

    // Hot = under the cursor, active = pressed/dragging. Identifiers are
    // fingerprinted from widget position + label. We keep them as u64 for
    // headroom in case Lane 18 adds nested panels later.
    u64 hot_id    = 0;
    u64 active_id = 0;

    // Ring buffer for the perf graph. Filled in by `graph()`.
    std::array<f32, kPerfGraphSamples> perf_samples{};
    u32                                 perf_head    = 0;
    u32                                 perf_count   = 0;

    bool frame_open = false;
};

inline Context& context() noexcept {
    static Context ctx{};
    return ctx;
}

// Cheap FNV-1a-ish fingerprint of position + label, stable across frames
// so the hot/active tracking works correctly. We don't need cryptographic
// strength — only collision avoidance among the handful of overlay
// widgets a frame ever draws.
inline u64 widget_id(math::Vec2 pos,
                     math::Vec2 size,
                     const char* label,
                     usize label_len) noexcept {
    constexpr u64 kFnvOffset = 0xCBF29CE484222325ULL;
    constexpr u64 kFnvPrime  = 0x100000001B3ULL;
    u64 h = kFnvOffset;
    auto mix_f32 = [&h](f32 v) {
        std::uint32_t bits = 0;
        // Bit-cast without UB — memcpy is the std-blessed channel.
        std::memcpy(&bits, &v, sizeof(bits));
        for (int byte = 0; byte < 4; ++byte) {
            h ^= (bits >> (byte * 8)) & 0xFFu;
            h *= kFnvPrime;
        }
    };
    mix_f32(pos.x);
    mix_f32(pos.y);
    mix_f32(size.x);
    mix_f32(size.y);
    for (usize i = 0; i < label_len; ++i) {
        h ^= static_cast<u8>(label[i]);
        h *= kFnvPrime;
    }
    // Force a non-zero result so 0 stays sentinel for "no widget".
    return h == 0 ? 1ULL : h;
}

}  // namespace psynder::ui::imm::detail
