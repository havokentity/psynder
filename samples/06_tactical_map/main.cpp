// SPDX-License-Identifier: MIT
// Psynder — Sample 06 / M6 demo. Tactical map flyover (Project IGI / Delta
// Force-style). A procedurally generated 256×256 heightfield is rendered via
// the per-column heightmap raymarcher (`world::outdoor` backend B per
// ADR-008 / DESIGN.md §9.2). A scripted helicopter and six watchtowers are
// rasterized over the same framebuffer; both backends share the Z-buffer so
// the helicopter and towers are correctly occluded by the ridge line.
//
// Architecture notes:
//   * Terrain: 256×256 u16 heightmap built in-memory from a few octaves of
//     value-noise plus a single big ridge bump. The map is 1m/texel so the
//     world covers 256m on a side; gentle rolling hills with one tall ridge
//     across the centre. Spacing/height_scale chosen so peaks reach ~30m.
//   * Raymarcher: we drive the public `world::outdoor::TerrainRaymarch`
//     via `set_target(rm, &fb)` (sibling header `TerrainTarget.h`) +
//     `rm.render(view, proj)`. The Wave-E #112 wire-up paints terrain
//     pixels into the bound framebuffer + writes 24-bit-packed NDC z
//     (identical to `render::raster::pack_depth` so subsequent raster
//     Z-tests compare the same way). Sky is filled separately before
//     the terrain render — the public API leaves unhit pixels alone.
//   * Skybox: dawn gradient. Vertical (screen-Y) — warm orange at horizon
//     fading to cool blue at zenith. Drawn into pixels the raymarcher
//     didn't paint; tiles already at far-Z so the rasterizer never clobbers
//     them.
//   * Helicopter: a kinematic prop (body box + 4-blade rotor) moving along
//     a circular path at 50m altitude. Rotor spins, body banks slightly.
//   * Watchtowers: six scaled boxes scattered across the map.
//   * Camera: slow flyover at ~30m altitude doing a half-circle around the
//     central ridge, looking inward at the ridge centre.
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Same, space-separated.
//   --smoke-capture-out PATH Write the final framebuffer to PATH as PNG.

#include "common/MeshWinding.h"
#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/Texture.h"
#include "render/raster/Raster.h"
#include "world/outdoor/Terrain.h"
#include "world/outdoor/TerrainTarget.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace psynder;

namespace {

// ─── Render config ───────────────────────────────────────────────────────
constexpr u32 kFbW = 512;
constexpr u32 kFbH = 288;

// World layout: 256×256 texels at 1m spacing → 256m × 256m playable area.
constexpr u32 kHmSize = 256;
constexpr f32 kHmSpacing = 1.0f;                // metres per texel
constexpr f32 kHmHeightScl = 30.0f / 65535.0f;  // peaks ≈ 30m

// ─── Packed-pixel helpers ────────────────────────────────────────────────
constexpr u32 pack_rgba8(u32 r, u32 g, u32 b, u32 a = 0xFFu) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}
PSY_FORCEINLINE u32 clamp_u8(f32 v) noexcept {
    if (v < 0.0f)
        return 0u;
    if (v > 255.0f)
        return 255u;
    return static_cast<u32>(v);
}

// Pack a [0,1] float Z into the same 24-bit-float + 8-bit-stencil layout the
// rasterizer uses (`TileRaster.cpp::pack_depth`). Inlined here so the sample
// stays self-contained; the bit shuffling is trivial and verified by the
// rasterizer's unit tests on the receiving end.
PSY_FORCEINLINE u32 pack_depth_u24(f32 z) noexcept {
    if (z < 0.0f)
        z = 0.0f;
    if (z > 1.0f)
        z = 1.0f;
    u32 raw;
    std::memcpy(&raw, &z, sizeof(raw));
    return raw & 0xFFFFFF00u;
}

// ─── Heightmap synthesis ─────────────────────────────────────────────────
//
// A few octaves of value-noise (hash-driven, deterministic) plus a single
// large ridge bump that runs roughly along the X axis through the centre.
// Output is u16, 0..65535, scaled to metres by `height_scale`.

// 32-bit integer hash → uniform float in [0,1).
PSY_FORCEINLINE f32 hash01(u32 x, u32 z, u32 seed) noexcept {
    u32 h = x * 0x27d4eb2du ^ (z * 0x165667b1u + seed * 0x9e3779b9u);
    h ^= h >> 15;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return static_cast<f32>(h) * (1.0f / 4294967296.0f);
}

// Smoothstep (5th order — Perlin's standard fade curve).
PSY_FORCEINLINE f32 fade5(f32 t) noexcept {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Bilinear value-noise sample at world-position (wx, wz) for the given
// "octave grid spacing" (cells of `cell` units), seeded uniquely per octave.
PSY_FORCEINLINE f32 value_noise(f32 wx, f32 wz, f32 cell, u32 seed) noexcept {
    const f32 fx = wx / cell;
    const f32 fz = wz / cell;
    const i32 ix = static_cast<i32>(std::floor(fx));
    const i32 iz = static_cast<i32>(std::floor(fz));
    const f32 tx = fade5(fx - static_cast<f32>(ix));
    const f32 tz = fade5(fz - static_cast<f32>(iz));
    const f32 h00 = hash01(static_cast<u32>(ix), static_cast<u32>(iz), seed);
    const f32 h10 = hash01(static_cast<u32>(ix + 1), static_cast<u32>(iz), seed);
    const f32 h01 = hash01(static_cast<u32>(ix), static_cast<u32>(iz + 1), seed);
    const f32 h11 = hash01(static_cast<u32>(ix + 1), static_cast<u32>(iz + 1), seed);
    const f32 a = h00 + (h10 - h00) * tx;
    const f32 b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
}

std::vector<u16> build_heightmap() {
    std::vector<u16> heights(static_cast<usize>(kHmSize) * kHmSize, 0u);

    // World extent of the map.
    const f32 map_m = static_cast<f32>(kHmSize) * kHmSpacing;
    const f32 cx = map_m * 0.5f;
    const f32 cz = map_m * 0.5f;

    for (u32 z = 0; z < kHmSize; ++z) {
        for (u32 x = 0; x < kHmSize; ++x) {
            const f32 wx = static_cast<f32>(x) * kHmSpacing;
            const f32 wz = static_cast<f32>(z) * kHmSpacing;

            // Three octaves of value noise — gentle rolling hills.
            f32 n = 0.0f;
            n += 0.55f * value_noise(wx, wz, 64.0f, 1u);
            n += 0.25f * value_noise(wx, wz, 24.0f, 2u);
            n += 0.10f * value_noise(wx, wz, 8.0f, 3u);
            // n is in [0, ~0.90]; normalize to [0, 1] approx.
            n = n / 0.90f;
            if (n < 0.0f)
                n = 0.0f;
            if (n > 1.0f)
                n = 1.0f;

            // Big ridge along Z axis through the middle: a Gaussian bump in
            // the Z direction (narrow band, ~30m wide) with a soft falloff
            // along X so the ridge has slight undulation along its length.
            const f32 dz = wz - cz;
            const f32 ridge = std::exp(-(dz * dz) / (2.0f * 22.0f * 22.0f));
            // Slight X variation so the ridge crest isn't perfectly straight.
            const f32 ridge_mod = 0.55f + 0.45f * value_noise(wx, 0.0f, 32.0f, 7u);

            // A second, smaller bump near the SE corner so the half-circle
            // camera flyover has interesting silhouettes both sides of the
            // ridge.
            const f32 bdx = wx - (cx + 60.0f);
            const f32 bdz = wz - (cz + 80.0f);
            const f32 bump = std::exp(-(bdx * bdx + bdz * bdz) / (2.0f * 28.0f * 28.0f));

            // Combine: base rolling hills + big ridge + small hill.
            const f32 base = 0.18f + 0.42f * n;  // [0.18, 0.60]
            const f32 sum = base + 0.85f * ridge * ridge_mod + 0.35f * bump;
            const f32 hf = sum > 1.0f ? 1.0f : sum;  // [0, 1]

            heights[static_cast<usize>(z) * kHmSize + x] = static_cast<u16>(hf * 65535.0f);
        }
    }
    return heights;
}

// ─── Heightmap sampling ──────────────────────────────────────────────────
//
// Bilinear sample of the u16 heightmap at world-space (wx, wz), returning
// metres. Shared by watchtower placement *and* the flyover camera's terrain-
// collision clamp so both agree on the ground height under any world point.
//
// Semantics deliberately match `world::outdoor::detail::sample_bilinear`:
//   * null heights / zero size / non-positive spacing  →  return 0
//   * out-of-bounds (fx/fz outside [0, size-1])         →  return 0
//   * in-bounds                                          →  bilinear blend
// (The Copilot review on PR #114 flagged edge-clamp extrapolation for props
// near the map edge — falling back to 0 keeps this consistent with the rest
// of the engine.)
f32 terrain_height(const world::outdoor::HeightmapDesc& hm, f32 wx, f32 wz) noexcept {
    if (!hm.heights || hm.size_x == 0 || hm.size_z == 0 || hm.spacing <= 0.0f) {
        return 0.0f;
    }
    const f32 fx = wx / hm.spacing;
    const f32 fz = wz / hm.spacing;
    const f32 max_x = static_cast<f32>(hm.size_x - 1);
    const f32 max_z = static_cast<f32>(hm.size_z - 1);
    if (fx < 0.0f || fx > max_x || fz < 0.0f || fz > max_z) {
        return 0.0f;
    }
    const i32 ix = static_cast<i32>(std::floor(fx));
    const i32 iz = static_cast<i32>(std::floor(fz));
    const f32 tx = fx - static_cast<f32>(ix);
    const f32 tz = fz - static_cast<f32>(iz);
    auto fetch = [&hm](i32 cx, i32 cz) noexcept -> f32 {
        cx = std::clamp(cx, 0, static_cast<i32>(hm.size_x) - 1);
        cz = std::clamp(cz, 0, static_cast<i32>(hm.size_z) - 1);
        const usize idx = static_cast<usize>(cz) * hm.size_x + static_cast<usize>(cx);
        return static_cast<f32>(hm.heights[idx]) * hm.height_scale;
    };
    const f32 h00 = fetch(ix, iz);
    const f32 h10 = fetch(ix + 1, iz);
    const f32 h01 = fetch(ix, iz + 1);
    const f32 h11 = fetch(ix + 1, iz + 1);
    const f32 a = h00 * (1.0f - tx) + h10 * tx;
    const f32 b = h01 * (1.0f - tx) + h11 * tx;
    return a * (1.0f - tz) + b * tz;
}

// ─── Sky gradient ────────────────────────────────────────────────────────
// Vertical screen-Y gradient: warm orange near the horizon (bottom of the
// upper-screen band), cool blue at zenith.
PSY_FORCEINLINE u32 sample_sky_row(u32 fb_y) noexcept {
    const f32 t = static_cast<f32>(fb_y) / static_cast<f32>(kFbH - 1u);
    // t=0 zenith, t=1 horizon.
    constexpr f32 zenith[3] = {64.0f, 92.0f, 150.0f};
    constexpr f32 horizon[3] = {235.0f, 160.0f, 90.0f};
    const f32 r = zenith[0] * (1.0f - t) + horizon[0] * t;
    const f32 g = zenith[1] * (1.0f - t) + horizon[1] * t;
    const f32 b = zenith[2] * (1.0f - t) + horizon[2] * t;
    return pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

// ─── View setup ──────────────────────────────────────────────────────────
struct CameraView {
    math::Vec3 eye;
    math::Vec3 fwd;    // unit
    math::Vec3 right;  // unit
    math::Vec3 up;     // unit
    math::Mat4 view;
    math::Mat4 proj;
    f32 fov_tan;  // tan(half-fov-vertical)
    f32 aspect;
    f32 near_z;
    f32 far_z;
};

// Vertical clearance the flyover eye keeps above whatever it passes over.
// Sized to clear the ~7m watchtowers when the orbit carries the camera near
// one (terrain + clearance > terrain + tower height) and to leave comfortable
// headroom over the ~30m central ridge.
constexpr f32 kFlyoverClearance = 10.0f;
constexpr f32 kTowerHeight = 7.0f;

CameraView make_flyover_camera(f32 t_seconds,
                               const world::outdoor::HeightmapDesc& hm,
                               f32& smoothed_eye_y,
                               f32 dt) {
    // Centre of the map (the central ridge crest).
    const f32 map_m = static_cast<f32>(kHmSize) * kHmSpacing;
    const math::Vec3 centre{map_m * 0.5f, 14.0f, map_m * 0.5f};
    // Half-circle: angle ∈ [-π/2, +π/2] across the run, slow.
    const f32 ang = -math::kHalfPi + t_seconds * 0.10f;
    const f32 radius = 130.0f;
    const f32 alt = 30.0f;
    math::Vec3 eye{
        centre.x + std::cos(ang) * radius,
        alt,
        centre.z + std::sin(ang) * radius,
    };
    // Terrain collision. Sampling only the eye's own column is not enough: as
    // the orbit looks inward, the central ridge can rise BETWEEN the eye and the
    // map centre — taller than the eye — so the eye sits above its own ground yet
    // still ploughs into that in-front crest. Worst of all near ang≈0, where the
    // eye drifts a few metres off the +X map edge: its own column then reads as
    // flat (terrain_height → 0 off-map) while the ~30m ridge sits just ahead, so
    // the unclamped eye grazes straight along the crest. Clamp instead above the
    // MAX terrain found over three sets of samples:
    //   * the eye's own column,
    //   * a look-ahead fan toward the view target (catches the in-front ridge),
    //   * a small ring around the eye (catches a crest the orbit is about to
    //     pass — motion is tangential, not toward the target — and any sharp tip
    //     the bilinear at the exact column would miss).
    // `+kTowerHeight` keeps the headroom that clears a watchtower the orbit may
    // carry the eye over.
    f32 max_ground = terrain_height(hm, eye.x, eye.z);
    {
        // Horizontal direction toward the look target (centre); y is irrelevant.
        const f32 dx = centre.x - eye.x;
        const f32 dz = centre.z - eye.z;
        const f32 dlen = std::sqrt(dx * dx + dz * dz);
        const f32 fx = dlen > 1e-4f ? dx / dlen : 0.0f;
        const f32 fz = dlen > 1e-4f ? dz / dlen : 0.0f;
        for (f32 d = 8.0f; d <= 56.0f; d += 8.0f) {
            max_ground = std::max(max_ground, terrain_height(hm, eye.x + fx * d, eye.z + fz * d));
        }
        constexpr f32 kRingR = 14.0f;
        for (u32 k = 0; k < 8; ++k) {
            const f32 a = static_cast<f32>(k) * (math::kPi * 0.25f);
            max_ground = std::max(
                max_ground,
                terrain_height(hm, eye.x + std::cos(a) * kRingR, eye.z + std::sin(a) * kRingR));
        }
    }
    // ASYMMETRIC smoothing: RISE to the target instantly (never clip a crest);
    // DESCEND at a limited rate so the eye eases back down off a peak instead of
    // snapping straight through the mountain.
    const f32 target_y = std::max(eye.y, max_ground + kTowerHeight + kFlyoverClearance);
    if (target_y >= smoothed_eye_y) {
        smoothed_eye_y = target_y;  // instant rise — clear the terrain at once
    } else {
        constexpr f32 kDescendRate = 12.0f;  // m/s — gentle settle off a peak
        smoothed_eye_y = std::max(target_y, smoothed_eye_y - kDescendRate * dt);
    }
    eye.y = smoothed_eye_y;
    const math::Vec3 up_world{0, 1, 0};
    const math::Vec3 fwd = math::normalize(math::sub(centre, eye));
    const math::Vec3 right = math::normalize(math::cross(fwd, up_world));
    const math::Vec3 up = math::cross(right, fwd);

    CameraView c{};
    c.eye = eye;
    c.fwd = fwd;
    c.right = right;
    c.up = up;
    c.fov_tan = std::tan(60.0f * math::kDegToRad * 0.5f);
    c.aspect = static_cast<f32>(kFbW) / static_cast<f32>(kFbH);
    c.near_z = 1.0f;
    c.far_z = 600.0f;
    c.view = math::look_at_rh(eye, centre, up_world);
    c.proj = math::perspective_rh(60.0f * math::kDegToRad, c.aspect, c.near_z, c.far_z);
    return c;
}

// ─── Sky pre-fill ───────────────────────────────────────────────────────
//
// `TerrainRaymarch::render` (Wave E #112) paints terrain pixels only —
// it leaves unhit pixels at whatever was in the framebuffer. Sweep the
// sky gradient in first; the public render then writes terrain over it.
//
// Iterates using the framebuffer's actual `width / height` + the
// `pitch`-derived row stride rather than the sample's compile-time
// `kFbW / kFbH` — keeps the helper consistent with its parameter so a
// future render-resolution change can't OOB the pixel buffer.
void fill_sky(render::Framebuffer& fb) noexcept {
    if (!fb.pixels || fb.width == 0 || fb.height == 0 || fb.pitch == 0) {
        return;
    }
    auto* base = fb.pixels;
    const usize row_stride = static_cast<usize>(fb.pitch);  // bytes
    for (u32 y = 0; y < fb.height; ++y) {
        const u32 sky_color = sample_sky_row(y);
        auto* row = reinterpret_cast<u32*>(base + static_cast<usize>(y) * row_stride);
        for (u32 x = 0; x < fb.width; ++x) {
            row[x] = sky_color;
        }
    }
}

// ─── Box mesh helpers ────────────────────────────────────────────────────
// Unit cube spanning [-0.5, +0.5]³ — per-face flat-shaded by colour.

constexpr u32 kColHeliBody = pack_rgba8(60, 70, 56);
constexpr u32 kColHeliTrim = pack_rgba8(36, 44, 36);
constexpr u32 kColRotor = pack_rgba8(32, 32, 32);

// ─── Building facade texture ─────────────────────────────────────────────
//
// Deterministic RGBA8 chunk (kFacadeDim², pitch == width) read as a concrete
// watchtower facade: a mid-grey concrete field with fine speckle, a grid of
// lit windows (warm panes with dark mullions between them), and a darker
// roof/parapet band across the top. No RNG — one cheap integer hash gives the
// concrete its grain. Each tower's DrawItem points `lightmap_texels` here and
// the cube's 0..1 per-face uv spans the chunk, so the surface_cached path
// computes vertexColor × facade(uv) per pixel. Buffer is owned by main() and
// outlives the render loop.
constexpr u32 kFacadeDim = 64;

// Small deterministic 2D value hash → [0,1). Cheap, repeatable, no global
// state; adds fine concrete grain so the wall isn't a dead flat field.
PSY_FORCEINLINE f32 facade_hash2(u32 x, u32 y) noexcept {
    u32 h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0x1000000u);
}

render::Texture2D build_facade_texture() {
    const u32 dim = kFacadeDim;
    std::vector<u32> tex(static_cast<usize>(dim) * dim, 0u);

    // Palette: cool concrete field, dark mullion/parapet, warm lit panes.
    constexpr i32 kConcR = 150, kConcG = 152, kConcB = 150;  // concrete base
    constexpr i32 kMullR = 58, kMullG = 60, kMullB = 64;     // window frame
    constexpr i32 kRoofR = 70, kRoofG = 66, kRoofB = 60;     // roof/parapet band
    constexpr i32 kWinR = 196, kWinG = 176, kWinB = 96;      // lit pane

    const u32 roof_h = std::max(3u, dim / 8);  // parapet band height (top)
    const u32 cols = 3;                        // window columns across the face
    const u32 rows = 3;                        // window rows down the face
    const u32 cell_w = dim / cols;
    const u32 cell_h = (dim - roof_h) / rows;
    const u32 pane_inset = std::max(2u, cell_w / 6);  // mullion thickness

    for (u32 y = 0; y < dim; ++y) {
        for (u32 x = 0; x < dim; ++x) {
            // Concrete base with fine speckle so it isn't banded.
            const i32 sp = static_cast<i32>((facade_hash2(x, y) - 0.5f) * 18.0f);
            i32 r = kConcR + sp;
            i32 g = kConcG + sp;
            i32 b = kConcB + sp;

            if (y < roof_h) {
                // Darker roof / parapet band across the top.
                const i32 rs = static_cast<i32>((facade_hash2(x, y) - 0.5f) * 12.0f);
                r = kRoofR + rs;
                g = kRoofG + rs;
                b = kRoofB + rs;
            } else {
                // Window grid below the parapet. A pane is the inset interior
                // of each cell; the surrounding band reads as the mullion.
                const u32 wy = y - roof_h;
                const u32 col_in = x % cell_w;
                const u32 row_in = wy % cell_h;
                const bool in_pane = col_in >= pane_inset && col_in < cell_w - pane_inset &&
                                     row_in >= pane_inset && row_in < cell_h - pane_inset &&
                                     wy < rows * cell_h;
                if (in_pane) {
                    // Warm lit glass with a faint top-down gradient + speckle
                    // so the panes read as glazing rather than flat fill.
                    const f32 gy = static_cast<f32>(row_in) / static_cast<f32>(cell_h);
                    const i32 grad = static_cast<i32>((0.5f - gy) * 24.0f);
                    const i32 gs = static_cast<i32>((facade_hash2(x, y) - 0.5f) * 14.0f);
                    r = kWinR + grad + gs;
                    g = kWinG + grad + gs;
                    b = kWinB + grad + gs;
                } else if ((col_in < pane_inset || col_in >= cell_w - pane_inset ||
                            row_in < pane_inset || row_in >= cell_h - pane_inset) &&
                           wy < rows * cell_h) {
                    // Dark mullion between/around the panes.
                    r = kMullR;
                    g = kMullG;
                    b = kMullB;
                }
            }

            tex[static_cast<usize>(y) * dim + x] = pack_rgba8(clamp_u8(static_cast<f32>(r)),
                                                              clamp_u8(static_cast<f32>(g)),
                                                              clamp_u8(static_cast<f32>(b)),
                                                              255u);
        }
    }
    return render::Texture2D::from_rgba8(dim, dim, std::move(tex));
}

// Build a cube with a single per-face colour; populates `verts`/`indices`
// appended to whatever the caller already has, returning the index offsets.
void emit_cube(std::vector<render::raster::Vertex>& verts,
               std::vector<u32>& indices,
               u32 col_side,
               u32 col_top) {
    const u32 base = static_cast<u32>(verts.size());
    // 6 faces × 4 verts. Per-face colour so the silhouette doesn't smear.
    auto push_face =
        [&](math::Vec3 a, math::Vec3 b, math::Vec3 c, math::Vec3 d, math::Vec3 normal, u32 col) {
            const u32 v0 = static_cast<u32>(verts.size());
            verts.push_back({a, normal, {0, 0}, {0, 0}, col});
            verts.push_back({b, normal, {1, 0}, {0, 0}, col});
            verts.push_back({c, normal, {1, 1}, {0, 0}, col});
            verts.push_back({d, normal, {0, 1}, {0, 0}, col});
            indices.push_back(v0 + 0);
            indices.push_back(v0 + 1);
            indices.push_back(v0 + 2);
            indices.push_back(v0 + 0);
            indices.push_back(v0 + 2);
            indices.push_back(v0 + 3);
        };
    // +X face
    push_face({0.5f, -0.5f, -0.5f},
              {0.5f, 0.5f, -0.5f},
              {0.5f, 0.5f, 0.5f},
              {0.5f, -0.5f, 0.5f},
              {1, 0, 0},
              col_side);
    // -X face
    push_face({-0.5f, -0.5f, 0.5f},
              {-0.5f, 0.5f, 0.5f},
              {-0.5f, 0.5f, -0.5f},
              {-0.5f, -0.5f, -0.5f},
              {-1, 0, 0},
              col_side);
    // +Y top
    push_face({-0.5f, 0.5f, -0.5f},
              {-0.5f, 0.5f, 0.5f},
              {0.5f, 0.5f, 0.5f},
              {0.5f, 0.5f, -0.5f},
              {0, 1, 0},
              col_top);
    // -Y bottom
    push_face({-0.5f, -0.5f, 0.5f},
              {-0.5f, -0.5f, -0.5f},
              {0.5f, -0.5f, -0.5f},
              {0.5f, -0.5f, 0.5f},
              {0, -1, 0},
              col_side);
    // +Z face
    push_face({-0.5f, -0.5f, 0.5f},
              {0.5f, -0.5f, 0.5f},
              {0.5f, 0.5f, 0.5f},
              {-0.5f, 0.5f, 0.5f},
              {0, 0, 1},
              col_side);
    // -Z face
    push_face({0.5f, -0.5f, -0.5f},
              {-0.5f, -0.5f, -0.5f},
              {-0.5f, 0.5f, -0.5f},
              {0.5f, 0.5f, -0.5f},
              {0, 0, -1},
              col_side);
    (void)base;
}

struct Mesh {
    std::vector<render::raster::Vertex> verts;
    std::vector<u32> indices;
};

Mesh build_cube_mesh(u32 col_side, u32 col_top) {
    Mesh m;
    emit_cube(m.verts, m.indices, col_side, col_top);
    return m;
}

// ─── Watchtower placement ────────────────────────────────────────────────
struct Tower {
    math::Vec3 ground_pos;  // world XZ + Y on the terrain
    f32 height;             // metres
    f32 half_extent;        // metres
};

std::array<Tower, 6> make_watchtowers(const world::outdoor::HeightmapDesc& hm) {
    const f32 map_m = static_cast<f32>(kHmSize) * kHmSpacing;
    const f32 cx = map_m * 0.5f;
    const f32 cz = map_m * 0.5f;
    // Six positions: roughly on a hexagonal ring around the map centre.
    const std::array<math::Vec2, 6> xz = {{
        {cx + 100.0f, cz + 10.0f},
        {cx + 50.0f, cz + 85.0f},
        {cx - 50.0f, cz + 90.0f},
        {cx - 100.0f, cz + 20.0f},
        {cx - 40.0f, cz - 80.0f},
        {cx + 60.0f, cz - 90.0f},
    }};

    std::array<Tower, 6> towers{};
    for (u32 i = 0; i < 6; ++i) {
        const f32 wx = xz[i].x;
        const f32 wz = xz[i].y;
        const f32 hy = terrain_height(hm, wx, wz);
        towers[i].ground_pos = {wx, hy, wz};
        towers[i].height = 7.0f;
        towers[i].half_extent = 1.6f;
    }
    return towers;
}

// ─── Helicopter ──────────────────────────────────────────────────────────
struct HeliPose {
    math::Vec3 position;  // world
    f32 yaw_rad;          // heading
    f32 rotor_phase;      // [0, 2π)
};

HeliPose make_helicopter_pose(f32 t_seconds) {
    HeliPose hp{};
    const f32 map_m = static_cast<f32>(kHmSize) * kHmSpacing;
    const f32 cx = map_m * 0.5f;
    const f32 cz = map_m * 0.5f;
    // Circular patrol path at 50m altitude.
    const f32 ang = t_seconds * 0.45f;
    const f32 r = 70.0f;
    hp.position = {cx + std::cos(ang) * r, 50.0f, cz + std::sin(ang) * r};
    // Heading: tangent to the path (turn into the circle).
    hp.yaw_rad = ang + math::kHalfPi;
    hp.rotor_phase = std::fmod(t_seconds * 18.0f, math::kTwoPi);
    return hp;
}

// Yaw-only Y-axis rotation as a Mat4.
math::Mat4 yaw_mat4(f32 yaw_rad) {
    return math::rotate_quat(math::quat_from_axis_angle(math::Vec3{0, 1, 0}, yaw_rad));
}

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 06 (tactical map flyover, M6)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;
    return desc;
}

int sample_main(const app::AppArgs& base_args, app::WindowApp& app_host) {
    const app::AppArgs& args = base_args;
    const u32 smoke_frames = args.smoke_frames;
    const platform::WindowDesc desc = make_window_desc(args);
    auto* window = &app_host.window();

    render::Framebuffer& fb = app_host.framebuffer();
    std::vector<u32>& depth = app_host.depth();

    // ─── Heightmap + raymarcher state ────────────────────────────────────
    const std::vector<u16> heights = build_heightmap();
    world::outdoor::HeightmapDesc hm_desc{};
    hm_desc.size_x = kHmSize;
    hm_desc.size_z = kHmSize;
    hm_desc.spacing = kHmSpacing;
    hm_desc.height_scale = kHmHeightScl;
    hm_desc.heights = heights.data();

    // Wave E #112 made `TerrainRaymarch::render(view, proj)` paint into a
    // bound framebuffer via the sibling `set_target` hook. The lighting +
    // haze constants in `engine/world/outdoor/Terrain.cpp` are intentionally
    // identical to the palette this sample used inline before Wave E, so
    // visual output stays continuous after the cut-over.
    world::outdoor::TerrainRaymarch terrain_rm;
    terrain_rm.set_heightmap(hm_desc);
    world::outdoor::set_target(terrain_rm, &fb);

    // ─── Building facade texture ─────────────────────────────────────────
    // Owned here so its storage outlives every end_frame() that samples it;
    // each tower's DrawItem points `lightmap_texels` at this buffer.
    const render::Texture2D facade_tex = build_facade_texture();
    const render::TextureView facade_view = facade_tex.view();

    // ─── Scene props ─────────────────────────────────────────────────────
    // Towers wear the facade texture, so their verts are white — the
    // surface_cached path computes white × facade(uv) and shows the texture
    // faithfully. The heli keeps its flat per-face palette.
    Mesh tower_mesh = build_cube_mesh(pack_rgba8(255, 255, 255), pack_rgba8(255, 255, 255));
    Mesh heli_body = build_cube_mesh(kColHeliBody, kColHeliTrim);
    Mesh heli_blade = build_cube_mesh(kColRotor, kColRotor);

    // The rasterizer back-face culls by default (DESIGN.md §7.3), so each cube
    // must be wound consistently with its per-vertex normals or faces drop out
    // as the camera orbits. Rewind once from the shared normals; the per-face
    // colours/uv are left untouched.
    samples::fix_winding(tower_mesh.verts.data(),
                         static_cast<u32>(tower_mesh.verts.size()),
                         tower_mesh.indices.data(),
                         static_cast<u32>(tower_mesh.indices.size()));
    samples::fix_winding(heli_body.verts.data(),
                         static_cast<u32>(heli_body.verts.size()),
                         heli_body.indices.data(),
                         static_cast<u32>(heli_body.indices.size()));
    samples::fix_winding(heli_blade.verts.data(),
                         static_cast<u32>(heli_blade.verts.size()),
                         heli_blade.indices.data(),
                         static_cast<u32>(heli_blade.indices.size()));

    const auto towers = make_watchtowers(hm_desc);

    auto& rasterizer = render::raster::Rasterizer::Get();

    PSY_LOG_INFO("Psynder sample 06 running{}",
                 smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", smoke_frames)
                                  : std::string{});

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;
    // Smoothed flyover eye height (asymmetric terrain collision) + previous-time
    // for the per-frame dt the descent integrates against.
    f32 cam_eye_y = 30.0f;  // the flyover's base altitude
    f64 prev_t = 0.0;

    while (!window->should_close()) {
        window->poll_events();

        // ESC quits — unless the console is open, where Esc closes it instead.
        if (auto* in = platform::input();
            in && in->key_down(platform::KeyCode::Escape) && !editor::overlays_capturing()) {
            break;
        }

        // Smoke mode pins time to frame so the captured PNG is deterministic.
        const f64 t = smoke_frames > 0 ? static_cast<f64>(frame) * (1.0 / 60.0)
                                       : platform::Clock::seconds(platform::Clock::ticks_now() - t0);
        f32 dt = static_cast<f32>(t - prev_t);
        if (dt <= 0.0f || dt > 0.1f)
            dt = 1.0f / 60.0f;  // first frame / hitch guard
        prev_t = t;

        const CameraView cam = make_flyover_camera(static_cast<f32>(t), hm_desc, cam_eye_y, dt);

        // ── Step 1: terrain via the public TerrainRaymarch ─────────────
        // Reset depth to far before the raymarch (the public render writes
        // only at hit pixels, identical packing to `pack_depth`).
        {
            const u32 far_packed = pack_depth_u24(1.0f);
            const usize n = static_cast<usize>(kFbW) * kFbH;
            for (usize i = 0; i < n; ++i)
                depth[i] = far_packed;
        }
        // Pre-fill sky — the public render leaves unhit pixels alone.
        fill_sky(fb);
        // Terrain paints over the sky at hit pixels.
        terrain_rm.render(cam.view, cam.proj);

        // ── Step 2: rasterizer for helicopter + watchtowers ────────────
        render::raster::ViewState view_state{};
        view_state.target = fb;
        view_state.view = cam.view;
        view_state.projection = cam.proj;
        view_state.tile_w = 64;
        view_state.tile_h = 64;
        rasterizer.begin_frame(view_state);

        // Watchtowers — scale the unit cube to tower dimensions, translate
        // to the terrain surface + half-height.
        for (const auto& tw : towers) {
            const math::Mat4 scl =
                math::scale(math::Vec3{tw.half_extent * 2.0f, tw.height, tw.half_extent * 2.0f});
            const math::Mat4 trs = math::translate(
                math::Vec3{tw.ground_pos.x, tw.ground_pos.y + tw.height * 0.5f, tw.ground_pos.z});

            render::raster::DrawItem item{};
            item.vertices = tower_mesh.verts.data();
            item.vertex_count = static_cast<u32>(tower_mesh.verts.size());
            item.indices = tower_mesh.indices.data();
            item.index_count = static_cast<u32>(tower_mesh.indices.size());
            item.model = math::mul(trs, scl);
            // Bind the procedural facade chunk: forces the surface_cached path
            // so each pixel is white × facade(uv). Buffer owned by `facade_tex`
            // above and outlives this loop.
            item.lightmap_texels = facade_view.texels;
            item.lightmap_w = facade_view.width;
            item.lightmap_h = facade_view.height;
            rasterizer.submit(item);
        }

        // Helicopter — body + 4-blade rotor.
        const HeliPose hp = make_helicopter_pose(static_cast<f32>(t));
        const math::Mat4 heli_yaw = yaw_mat4(hp.yaw_rad);
        // Body: 3.6m long × 1.5m tall × 1.5m wide.
        {
            const math::Mat4 scl = math::scale(math::Vec3{3.6f, 1.5f, 1.5f});
            const math::Mat4 trs = math::translate(hp.position);
            render::raster::DrawItem item{};
            item.vertices = heli_body.verts.data();
            item.vertex_count = static_cast<u32>(heli_body.verts.size());
            item.indices = heli_body.indices.data();
            item.index_count = static_cast<u32>(heli_body.indices.size());
            item.model = math::mul(trs, math::mul(heli_yaw, scl));
            rasterizer.submit(item);
        }
        // Rotor: 4 thin blades sitting 0.9m above the body, spinning around
        // the helicopter's local Y axis.
        const math::Mat4 rotor_spin =
            math::rotate_quat(math::quat_from_axis_angle(math::Vec3{0, 1, 0}, hp.rotor_phase));
        const math::Mat4 rotor_lift =
            math::translate(math::Vec3{hp.position.x, hp.position.y + 0.9f, hp.position.z});
        for (u32 b = 0; b < 4; ++b) {
            // Each blade offset 90° around the rotor hub.
            const f32 ang = static_cast<f32>(b) * math::kHalfPi;
            const math::Mat4 blade_rot =
                math::rotate_quat(math::quat_from_axis_angle(math::Vec3{0, 1, 0}, ang));
            // Blade: 4m long × 0.06m thick × 0.25m wide; offset 1.5m
            // outward along local +X so it pivots about the hub.
            const math::Mat4 scl = math::scale(math::Vec3{4.0f, 0.08f, 0.28f});
            const math::Mat4 blade_off = math::translate(math::Vec3{1.5f, 0.0f, 0.0f});

            render::raster::DrawItem item{};
            item.vertices = heli_blade.verts.data();
            item.vertex_count = static_cast<u32>(heli_blade.verts.size());
            item.indices = heli_blade.indices.data();
            item.index_count = static_cast<u32>(heli_blade.indices.size());
            // Final transform: world ← rotor_lift × heli_yaw × rotor_spin ×
            //                  blade_rot × blade_off × scl
            const math::Mat4 m = math::mul(
                rotor_lift,
                math::mul(heli_yaw,
                          math::mul(rotor_spin, math::mul(blade_rot, math::mul(blade_off, scl)))));
            item.model = m;
            rasterizer.submit(item);
        }

        rasterizer.end_frame();

        // Engine overlay suite (lane 18): `~` drop-down console + F1 debug HUD
        // + F2 Play/Edit badge. One call, drawn over the rendered scene.
        if (auto* in = platform::input()) {
            editor::frame_overlays(*in, fb, {towers.size() + 5u, 0, 0});
        }

        window->present(fb);

        if (smoke_frames > 0 && ++frame >= smoke_frames) {
            PSY_LOG_INFO("sample_06: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    const bool capture_ok = app_host.write_capture_if_requested("sample_06");

    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct TacticalMapSample {
    static constexpr std::string_view log_name() noexcept { return "sample_06"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder sample 06"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    static app::WindowAppOptions window_options(const app::AppArgs&) noexcept {
        return {.depth_buffer = true};
    }

    int run(app::WindowApp& app_host, const app::AppArgs& args) {
        return sample_main(args, app_host);
    }
};

PSYNDER_WINDOW_SAMPLE_MAIN(TacticalMapSample)
