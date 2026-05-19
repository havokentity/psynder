// SPDX-License-Identifier: MIT
// Psynder — test-mesh helpers for sample_01_triangle + raster tests.

#include "TestMesh.h"

#include "core/Types.h"
#include "math/Math.h"

namespace psynder::render::raster::test_mesh {

namespace {

constexpr u32 pack_rgba8(u8 r, u8 g, u8 b, u8 a) noexcept {
    return static_cast<u32>(r)
         | (static_cast<u32>(g) << 8)
         | (static_cast<u32>(b) << 16)
         | (static_cast<u32>(a) << 24);
}

// Single colored triangle in NDC-ish coords. Vertex order is BL → TOP → BR
// so the triangle stays front-facing after the rasterizer's viewport
// y-flip (DESIGN.md §10.1 right-handed coordinates → screen y-down).
constexpr Vertex kTriangleVerts[] = {
    // BL (red)
    Vertex{ math::Vec3{-0.6f, -0.5f, 0.0f}, math::Vec3{0,0,1}, math::Vec2{0.0f, 1.0f}, math::Vec2{0,0}, pack_rgba8(255,  64,  64, 255) },
    // TOP (blue)
    Vertex{ math::Vec3{ 0.0f,  0.6f, 0.0f}, math::Vec3{0,0,1}, math::Vec2{0.5f, 0.0f}, math::Vec2{0,0}, pack_rgba8( 64,  64, 255, 255) },
    // BR (green)
    Vertex{ math::Vec3{ 0.6f, -0.5f, 0.0f}, math::Vec3{0,0,1}, math::Vec2{1.0f, 1.0f}, math::Vec2{0,0}, pack_rgba8( 64, 255,  64, 255) },
};
constexpr u32 kTriangleIndices[] = { 0, 1, 2 };

// Two-triangle quad spanning roughly the whole framebuffer in NDC.
// Indices wound so both triangles are front-facing post-flip.
constexpr Vertex kQuadVerts[] = {
    Vertex{ math::Vec3{-0.95f, -0.95f, 0.0f}, math::Vec3{0,0,1}, math::Vec2{0,1}, math::Vec2{0,0}, 0xFFFFFFFFu },  // 0 BL
    Vertex{ math::Vec3{ 0.95f, -0.95f, 0.0f}, math::Vec3{0,0,1}, math::Vec2{1,1}, math::Vec2{0,0}, 0xFFFFFFFFu },  // 1 BR
    Vertex{ math::Vec3{ 0.95f,  0.95f, 0.0f}, math::Vec3{0,0,1}, math::Vec2{1,0}, math::Vec2{0,0}, 0xFFFFFFFFu },  // 2 TR
    Vertex{ math::Vec3{-0.95f,  0.95f, 0.0f}, math::Vec3{0,0,1}, math::Vec2{0,0}, math::Vec2{0,0}, 0xFFFFFFFFu },  // 3 TL
};
// (BL, TL, BR) + (BR, TL, TR) — CCW in screen y-down
constexpr u32 kQuadIndices[] = { 0, 3, 1,  1, 3, 2 };

// 8×8 checkerboard
constexpr u32 kCheckerW = 8;
constexpr u32 kCheckerH = 8;
u32 g_checker[kCheckerW * kCheckerH];
bool g_checker_init = false;

void init_checker() noexcept {
    if (g_checker_init) return;
    for (u32 y = 0; y < kCheckerH; ++y) {
        for (u32 x = 0; x < kCheckerW; ++x) {
            const bool light = ((x ^ y) & 1) == 0;
            g_checker[y * kCheckerW + x] =
                light ? 0xFFFFFFFFu : 0xFF202020u;
        }
    }
    g_checker_init = true;
}

}  // namespace

TriangleMesh colored_triangle() noexcept {
    return TriangleMesh{
        kTriangleVerts,
        sizeof(kTriangleVerts) / sizeof(kTriangleVerts[0]),
        kTriangleIndices,
        sizeof(kTriangleIndices) / sizeof(kTriangleIndices[0]),
    };
}

TriangleMesh fullscreen_quad() noexcept {
    return TriangleMesh{
        kQuadVerts,
        sizeof(kQuadVerts) / sizeof(kQuadVerts[0]),
        kQuadIndices,
        sizeof(kQuadIndices) / sizeof(kQuadIndices[0]),
    };
}

const u32* checkerboard_texture(u32& w, u32& h) noexcept {
    init_checker();
    w = kCheckerW;
    h = kCheckerH;
    return g_checker;
}

}  // namespace psynder::render::raster::test_mesh
