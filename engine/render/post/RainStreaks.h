// SPDX-License-Identifier: MIT
// Psynder — rain streaks (alpha-blended quads, lit by the dynamic-light list).
// Lane 09 / Wave B per DESIGN.md §8.4 ("NFS:HS night-rain look").
//
// Implementation shape:
//
//   - StreakSystem owns a pool of streak particles. spawn_burst() seeds the
//     pool above the camera (within a configurable spawn column); step()
//     advances each streak by gravity-driven velocity, recycles when they
//     exit the bottom of the column, and re-spawns at the top.
//
//   - render() composites the streaks into an HDR framebuffer as alpha-
//     blended directional quads. Each streak's colour is the sum over the
//     supplied light list of (light_colour × attenuation × phase), then
//     scaled by alpha. We re-use the FogLight type so callers can pass the
//     same light list to fog and rain — most engines do.
//
// The streak look reads as "vertical hyphens whose head-bias matches the
// dominant light direction." We don't actually need depth-correct quads
// for the retro aesthetic — alpha-over-framebuffer is sufficient and is what
// the §8.4 sentence calls out.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"
#include "render/post/VolumetricFog.h"   // for FogLight reuse

namespace psynder::render::post {

// Streak particle. One streak = a short bright line segment falling under
// gravity, with a per-streak randomness term so the rain doesn't sync.
//
// 32 B = half a cache line; the pool packs tightly.
struct alignas(32) Streak {
    math::Vec3 position{0,0,0};   // world space, head of the streak
    f32        length = 0.4f;     // metres
    math::Vec3 velocity{0,-9.81f,0};  // m/s; gravity-driven
    f32        alpha = 0.5f;
};
static_assert(sizeof(Streak) == 32, "Streak layout must be 32 B");

// Spawn / sim params for a Wave-B rain column.
struct RainParams {
    bool       enabled         = true;
    math::Vec3 spawn_centre{0, 8, 0};    // top of the spawn box (above camera)
    math::Vec3 spawn_extent{16, 0, 16};  // half-extents (xz only — y is fixed)
    f32        kill_height_y   = -4.0f;  // y plane below which streaks recycle
    math::Vec3 base_velocity{0, -12.0f, 0};  // world units per second
    math::Vec3 jitter_velocity{1.0f, 2.0f, 1.0f};
    f32        streak_length   = 0.4f;
    f32        streak_alpha    = 0.6f;
    f32        intensity       = 1.0f;   // scales lit colour
    u32        rng_seed        = 0xA51F00DBu;
};

// Streak system. Owns a fixed-capacity pool; one pool covers the on-screen
// rain. Wave-B target capacity: 4 096 streaks at 1080p ≈ ~0.2 ms render.
class StreakSystem {
public:
    static constexpr u32 kMaxStreaks = 4096;

    void seed(u32 count, const RainParams& params);
    void step(f32 dt, const RainParams& params);

    // Render against an HDR framebuffer + light list. `camera_*` describes
    // the same view used by VolumetricFog. Depth, if non-null, occludes
    // streaks behind geometry (we sample the linear depth at the streak
    // head's projected pixel and skip the streak if it's behind that depth).
    void render(Framebuffer& hdr,
                const FogScene& camera,
                const RainParams& params,
                const f32* depth_linear) const;

    u32 active_count() const noexcept { return count_; }
    const Streak& at(u32 i) const noexcept { return pool_[i]; }

private:
    Streak pool_[kMaxStreaks];
    u32    count_ = 0;
    u32    rng_   = 0xA51F00DBu;

    // Tiny xorshift for spawn jitter — repeatable per seed.
    inline u32 next_u32() noexcept;
    inline f32 unit() noexcept;
    inline f32 signed_unit() noexcept;

    void spawn_one(Streak& s, const RainParams& params) noexcept;
};

}  // namespace psynder::render::post
