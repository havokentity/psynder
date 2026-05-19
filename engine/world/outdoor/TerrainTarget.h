// SPDX-License-Identifier: MIT
// Psynder — sibling target-bind header for the outdoor heightmap raymarcher.
//
// The Wave-A `TerrainRaymarch` API in the frozen `Terrain.h` is:
//
//     class TerrainRaymarch {
//     public:
//         void set_heightmap(const HeightmapDesc&);
//         void render(const math::Mat4& view, const math::Mat4& proj) const;
//     };
//
// `render()` has nowhere to draw without a framebuffer — but extending
// `TerrainRaymarch` itself would break the frozen header contract. Wave E
// resolves this with a free-function sibling setter that lives in this
// header. Internally the world_outdoor TU keys raymarch state by
// `&TerrainRaymarch` (same trick as `set_heightmap`), so binding a target
// is a TU-local map update.
//
// Usage (sample_06 / runtime):
//
//     world::outdoor::TerrainRaymarch terrain;
//     terrain.set_heightmap(desc);
//     world::outdoor::set_target(terrain, &framebuffer);
//     terrain.render(view, proj);            // paints into framebuffer
//
// Passing `nullptr` clears the binding; subsequent `render()` calls are
// no-ops (and do not crash) — pinned by the Wave-E unit test.

#pragma once

#include "core/Types.h"
#include "world/outdoor/Terrain.h"

namespace psynder::render {
struct Framebuffer;
}

namespace psynder::world::outdoor {

// Bind a framebuffer to a TerrainRaymarch instance. The framebuffer pointer
// is borrowed (lifetime is the caller's responsibility). Pass `nullptr` to
// detach. Safe to call before or after `set_heightmap`.
void set_target(const TerrainRaymarch& rm, render::Framebuffer* fb) noexcept;

}  // namespace psynder::world::outdoor
