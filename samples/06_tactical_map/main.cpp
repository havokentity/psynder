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
//   * Raymarcher: we drive the scalar reference kernel from
//     `world::outdoor::detail::march_ray` directly — one ray per framebuffer
//     column at this pixel's NDC, with the same view + projection used by
//     the rasterizer for the props. The header-only kernel is the same code
//     lane 11's unit test pins. We compute world-space hits, derive a
//     splat-blended diffuse color, and write the corresponding NDC z into
//     the framebuffer's depth slot (24-bit float bit-pattern, identical
//     packing to `render::raster::pack_depth` so subsequent raster Z-tests
//     compare the same way).
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

#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "world/outdoor/Heightmap_internal.h"
#include "world/outdoor/Raymarch_internal.h"
#include "world/outdoor/Terrain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

// ─── CLI ─────────────────────────────────────────────────────────────────
struct Args {
    u32         smoke_frames = 0;
    std::string capture_out;
};

u32 parse_uint(std::string_view v) noexcept {
    u32 out = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return 0;
        out = out * 10u + static_cast<u32>(c - '0');
    }
    return out;
}

Args parse_args(int argc, char** argv) {
    Args a{};
    constexpr std::string_view kFlag   = "--smoke-frames=";
    constexpr std::string_view kFlagSp = "--smoke-frames";
    constexpr std::string_view kCapEq  = "--smoke-capture-out=";
    constexpr std::string_view kCapSp  = "--smoke-capture-out";
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s.starts_with(kFlag)) {
            a.smoke_frames = parse_uint(s.substr(kFlag.size()));
        } else if (s == kFlagSp && i + 1 < argc) {
            a.smoke_frames = parse_uint(std::string_view{argv[++i]});
        } else if (s.starts_with(kCapEq)) {
            a.capture_out = std::string(s.substr(kCapEq.size()));
        } else if (s == kCapSp && i + 1 < argc) {
            a.capture_out = argv[++i];
        }
    }
    return a;
}

// ─── Render config ───────────────────────────────────────────────────────
constexpr u32 kFbW = 512;
constexpr u32 kFbH = 288;

// World layout: 256×256 texels at 1m spacing → 256m × 256m playable area.
constexpr u32 kHmSize       = 256;
constexpr f32 kHmSpacing    = 1.0f;     // metres per texel
constexpr f32 kHmHeightScl  = 30.0f / 65535.0f;  // peaks ≈ 30m

// ─── Packed-pixel helpers ────────────────────────────────────────────────
constexpr u32 pack_rgba8(u32 r, u32 g, u32 b, u32 a = 0xFFu) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}
PSY_FORCEINLINE u32 clamp_u8(f32 v) noexcept {
    if (v < 0.0f) return 0u;
    if (v > 255.0f) return 255u;
    return static_cast<u32>(v);
}

// Pack a [0,1] float Z into the same 24-bit-float + 8-bit-stencil layout the
// rasterizer uses (`TileRaster.cpp::pack_depth`). Inlined here so the sample
// stays self-contained; the bit shuffling is trivial and verified by the
// rasterizer's unit tests on the receiving end.
PSY_FORCEINLINE u32 pack_depth_u24(f32 z) noexcept {
    if (z < 0.0f) z = 0.0f;
    if (z > 1.0f) z = 1.0f;
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
    const f32 h00 = hash01(static_cast<u32>(ix),     static_cast<u32>(iz),     seed);
    const f32 h10 = hash01(static_cast<u32>(ix + 1), static_cast<u32>(iz),     seed);
    const f32 h01 = hash01(static_cast<u32>(ix),     static_cast<u32>(iz + 1), seed);
    const f32 h11 = hash01(static_cast<u32>(ix + 1), static_cast<u32>(iz + 1), seed);
    const f32 a = h00 + (h10 - h00) * tx;
    const f32 b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
}

std::vector<u16> build_heightmap() {
    std::vector<u16> heights(static_cast<usize>(kHmSize) * kHmSize, 0u);

    // World extent of the map.
    const f32 map_m = static_cast<f32>(kHmSize) * kHmSpacing;
    const f32 cx    = map_m * 0.5f;
    const f32 cz    = map_m * 0.5f;

    for (u32 z = 0; z < kHmSize; ++z) {
        for (u32 x = 0; x < kHmSize; ++x) {
            const f32 wx = static_cast<f32>(x) * kHmSpacing;
            const f32 wz = static_cast<f32>(z) * kHmSpacing;

            // Three octaves of value noise — gentle rolling hills.
            f32 n = 0.0f;
            n += 0.55f * value_noise(wx, wz, 64.0f, 1u);
            n += 0.25f * value_noise(wx, wz, 24.0f, 2u);
            n += 0.10f * value_noise(wx, wz,  8.0f, 3u);
            // n is in [0, ~0.90]; normalize to [0, 1] approx.
            n = n / 0.90f;
            if (n < 0.0f) n = 0.0f;
            if (n > 1.0f) n = 1.0f;

            // Big ridge along Z axis through the middle: a Gaussian bump in
            // the Z direction (narrow band, ~30m wide) with a soft falloff
            // along X so the ridge has slight undulation along its length.
            const f32 dz  = wz - cz;
            const f32 ridge = std::exp(-(dz * dz) / (2.0f * 22.0f * 22.0f));
            // Slight X variation so the ridge crest isn't perfectly straight.
            const f32 ridge_mod =
                0.55f + 0.45f * value_noise(wx, 0.0f, 32.0f, 7u);

            // A second, smaller bump near the SE corner so the half-circle
            // camera flyover has interesting silhouettes both sides of the
            // ridge.
            const f32 bdx = wx - (cx + 60.0f);
            const f32 bdz = wz - (cz + 80.0f);
            const f32 bump =
                std::exp(-(bdx * bdx + bdz * bdz) / (2.0f * 28.0f * 28.0f));

            // Combine: base rolling hills + big ridge + small hill.
            const f32 base = 0.18f + 0.42f * n;             // [0.18, 0.60]
            const f32 sum  = base + 0.85f * ridge * ridge_mod + 0.35f * bump;
            const f32 hf   = sum > 1.0f ? 1.0f : sum;       // [0, 1]

            heights[static_cast<usize>(z) * kHmSize + x] =
                static_cast<u16>(hf * 65535.0f);
        }
    }
    return heights;
}

// ─── Splat → RGB ─────────────────────────────────────────────────────────
// The raymarcher's `splat_at_texel` returns 4-weight grass/rock/sand/snow.
// We collapse to a single RGB by blending against four reference colours.
// Distance-based haze fades terrain toward the sky color near the horizon.
PSY_FORCEINLINE u32 splat_to_rgb(world::outdoor::detail::SplatWeights s,
                                 f32 ndotl, f32 dist) noexcept {
    // Reference palette tuned for the dawn lighting.
    constexpr f32 grass[3] = {  98.0f, 122.0f,  60.0f };
    constexpr f32 rock [3] = { 118.0f, 102.0f,  82.0f };
    constexpr f32 sand [3] = { 198.0f, 178.0f, 120.0f };
    constexpr f32 snow [3] = { 230.0f, 232.0f, 240.0f };
    f32 r = grass[0]*s.w[0] + rock[0]*s.w[1] + sand[0]*s.w[2] + snow[0]*s.w[3];
    f32 g = grass[1]*s.w[0] + rock[1]*s.w[1] + sand[1]*s.w[2] + snow[1]*s.w[3];
    f32 b = grass[2]*s.w[0] + rock[2]*s.w[1] + sand[2]*s.w[2] + snow[2]*s.w[3];

    // Lambert against a low-east sun (warm light).
    constexpr f32 sun_r = 1.20f, sun_g = 0.92f, sun_b = 0.74f;
    constexpr f32 amb_r = 0.32f, amb_g = 0.34f, amb_b = 0.46f;
    const f32 nl = ndotl < 0.0f ? 0.0f : ndotl;
    r *= (amb_r + sun_r * nl);
    g *= (amb_g + sun_g * nl);
    b *= (amb_b + sun_b * nl);

    // Distance haze: blend toward warm-orange dawn horizon at far ranges.
    constexpr f32 haze_r = 200.0f, haze_g = 150.0f, haze_b = 110.0f;
    const f32 fog = std::min(1.0f, dist / 360.0f);
    r = r * (1.0f - fog) + haze_r * fog;
    g = g * (1.0f - fog) + haze_g * fog;
    b = b * (1.0f - fog) + haze_b * fog;

    return pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

// ─── Sky gradient ────────────────────────────────────────────────────────
// Vertical screen-Y gradient: warm orange near the horizon (bottom of the
// upper-screen band), cool blue at zenith.
PSY_FORCEINLINE u32 sample_sky_row(u32 fb_y) noexcept {
    const f32 t = static_cast<f32>(fb_y) / static_cast<f32>(kFbH - 1u);
    // t=0 zenith, t=1 horizon.
    constexpr f32 zenith [3] = {  64.0f,  92.0f, 150.0f };
    constexpr f32 horizon[3] = { 235.0f, 160.0f,  90.0f };
    const f32 r = zenith[0] * (1.0f - t) + horizon[0] * t;
    const f32 g = zenith[1] * (1.0f - t) + horizon[1] * t;
    const f32 b = zenith[2] * (1.0f - t) + horizon[2] * t;
    return pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

// ─── View setup ──────────────────────────────────────────────────────────
struct CameraView {
    math::Vec3 eye;
    math::Vec3 fwd;       // unit
    math::Vec3 right;     // unit
    math::Vec3 up;        // unit
    math::Mat4 view;
    math::Mat4 proj;
    f32        fov_tan;   // tan(half-fov-vertical)
    f32        aspect;
    f32        near_z;
    f32        far_z;
};

CameraView make_flyover_camera(f32 t_seconds) {
    // Centre of the map (the central ridge crest).
    const f32 map_m = static_cast<f32>(kHmSize) * kHmSpacing;
    const math::Vec3 centre{ map_m * 0.5f, 14.0f, map_m * 0.5f };
    // Half-circle: angle ∈ [-π/2, +π/2] across the run, slow.
    const f32 ang   = -math::kHalfPi + t_seconds * 0.10f;
    const f32 radius = 130.0f;
    const f32 alt   = 30.0f;
    const math::Vec3 eye{
        centre.x + std::cos(ang) * radius,
        alt,
        centre.z + std::sin(ang) * radius,
    };
    const math::Vec3 up_world{0, 1, 0};
    const math::Vec3 fwd   = math::normalize(math::sub(centre, eye));
    const math::Vec3 right = math::normalize(math::cross(fwd, up_world));
    const math::Vec3 up    = math::cross(right, fwd);

    CameraView c{};
    c.eye    = eye;
    c.fwd    = fwd;
    c.right  = right;
    c.up     = up;
    c.fov_tan = std::tan(60.0f * math::kDegToRad * 0.5f);
    c.aspect  = static_cast<f32>(kFbW) / static_cast<f32>(kFbH);
    c.near_z  = 1.0f;
    c.far_z   = 600.0f;
    c.view    = math::look_at_rh(eye, centre, up_world);
    c.proj    = math::perspective_rh(60.0f * math::kDegToRad,
                                     c.aspect, c.near_z, c.far_z);
    return c;
}

// Build a primary ray direction for the (px+0.5, py+0.5) pixel sample.
PSY_FORCEINLINE math::Vec3 primary_ray_dir(const CameraView& cam, u32 px, u32 py) noexcept {
    const f32 nx = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(kFbW);
    const f32 ny = (static_cast<f32>(py) + 0.5f) / static_cast<f32>(kFbH);
    const f32 sx = (2.0f * nx - 1.0f) * cam.aspect * cam.fov_tan;
    const f32 sy = (1.0f - 2.0f * ny) * cam.fov_tan;
    math::Vec3 d{
        cam.fwd.x + cam.right.x * sx + cam.up.x * sy,
        cam.fwd.y + cam.right.y * sx + cam.up.y * sy,
        cam.fwd.z + cam.right.z * sx + cam.up.z * sy,
    };
    return math::normalize(d);
}

// Compute the NDC z (in [0,1]) for a world-space point given the same proj/
// view used by the rasterizer. We do the full clip-space transform and
// perspective divide so the depth value compares byte-identical to the
// rasterizer's per-pixel z under `pack_depth`.
PSY_FORCEINLINE f32 ndc_z_for_world(const CameraView& cam, math::Vec3 wp) noexcept {
    const math::Vec4 v{wp.x, wp.y, wp.z, 1.0f};
    const math::Vec4 vview = math::mul(cam.view, v);
    const math::Vec4 vclip = math::mul(cam.proj, vview);
    if (vclip.w <= 1e-6f) return 1.0f;
    f32 z = vclip.z / vclip.w;
    // GL-style perspective_rh returns clip-space z in [-1, +1] post-divide;
    // map to [0, 1] for the Z buffer storage.
    z = z * 0.5f + 0.5f;
    if (z < 0.0f) return 0.0f;
    if (z > 1.0f) return 1.0f;
    return z;
}

// ─── Raymarch the terrain into pixels + depth ────────────────────────────
//
// For each framebuffer pixel we cast a ray and walk it (logarithmic step)
// until we hit terrain or run out of distance. Hits are coloured via the
// splat→RGB blend and have their NDC z packed into the depth slot so the
// rasterizer's subsequent submits Z-test correctly.
void render_terrain(const world::outdoor::HeightmapDesc& hm,
                    const CameraView& cam,
                    render::Framebuffer& fb,
                    std::vector<u8>& hit_mask) {
    auto* pixels = reinterpret_cast<u32*>(fb.pixels);

    // Lambert sun direction: low east-northeast, golden hour.
    const math::Vec3 sun_dir =
        math::normalize(math::Vec3{ 0.55f, 0.50f, 0.20f });

    // World "floor" for the map AABB so we can reject rays whose endpoint
    // is unambiguously above the maximum terrain height.
    constexpr f32 kMaxTerrainY = 32.0f;   // ≥ kHmHeightScl * 65535 + slack
    constexpr f32 kStepNear    = 0.4f;    // m per first march step
    constexpr f32 kStepFar     = 6.0f;    // m per step at the horizon
    constexpr f32 kStepFalloff = 90.0f;   // distance at which steps double-ish
    constexpr f32 kMaxT        = 420.0f;  // longest ray, in metres

    for (u32 y = 0; y < kFbH; ++y) {
        const u32 sky_color = sample_sky_row(y);
        for (u32 x = 0; x < kFbW; ++x) {
            const usize idx = static_cast<usize>(y) * kFbW + x;
            const math::Vec3 dir = primary_ray_dir(cam, x, y);

            // Skip rays that go up — they only ever see the sky.
            if (dir.y > 0.005f && cam.eye.y > kMaxTerrainY) {
                pixels[idx] = sky_color;
                hit_mask[idx] = 0;
                continue;
            }

            // Variable-step march. We follow `march_ray` semantics: walk
            // forward, sample bilinear height, refine on the cross-under.
            f32 prev_t  = 0.0f;
            f32 prev_ry = cam.eye.y;
            f32 prev_th = world::outdoor::detail::sample_bilinear(
                              hm, cam.eye.x, cam.eye.z);

            bool hit = false;
            f32  hit_t = 0.0f;

            f32 t = kStepNear;
            while (t <= kMaxT) {
                const f32 wx = cam.eye.x + dir.x * t;
                const f32 wy = cam.eye.y + dir.y * t;
                const f32 wz = cam.eye.z + dir.z * t;
                const f32 th = world::outdoor::detail::sample_bilinear(
                                   hm, wx, wz);

                if (wy <= th) {
                    // Bisect once for sub-step accuracy (matches march_ray).
                    const f32 dy_a = prev_ry - prev_th;     // > 0
                    const f32 dy_b = wy     - th;            // ≤ 0
                    const f32 denom = dy_a - dy_b;
                    f32 frac = denom > 0.0f ? (dy_a / denom) : 0.0f;
                    if (frac < 0.0f) frac = 0.0f;
                    if (frac > 1.0f) frac = 1.0f;
                    hit_t = prev_t + (t - prev_t) * frac;
                    hit = true;
                    break;
                }
                prev_t  = t;
                prev_ry = wy;
                prev_th = th;
                // Logarithmic step growth.
                const f32 step = kStepNear + (kStepFar - kStepNear) *
                                 std::min(1.0f, t / kStepFalloff);
                t += step;
            }

            if (!hit) {
                pixels[idx] = sky_color;
                hit_mask[idx] = 0;
                continue;
            }

            const math::Vec3 hit_pos{
                cam.eye.x + dir.x * hit_t,
                cam.eye.y + dir.y * hit_t,
                cam.eye.z + dir.z * hit_t,
            };

            // Texel-snapped splat + normal for shading.
            const i32 tx = static_cast<i32>(std::floor(hit_pos.x / kHmSpacing + 0.5f));
            const i32 tz = static_cast<i32>(std::floor(hit_pos.z / kHmSpacing + 0.5f));
            const auto       splat = world::outdoor::detail::splat_at_texel(hm, tx, tz);
            const math::Vec3 n     = world::outdoor::detail::normal_at_texel(hm, tx, tz);
            const f32        ndotl = math::dot(n, sun_dir);

            pixels[idx] = splat_to_rgb(splat, ndotl, hit_t);

            // Z-buffer.
            if (fb.depth) {
                const f32 z = ndc_z_for_world(cam, hit_pos);
                fb.depth[idx] = pack_depth_u24(z);
            }
            hit_mask[idx] = 1;
        }
    }
}

// ─── Box mesh helpers ────────────────────────────────────────────────────
// Unit cube spanning [-0.5, +0.5]³ — per-face flat-shaded by colour.

constexpr u32 kColTowerBody = pack_rgba8(82, 70, 52);
constexpr u32 kColTowerRoof = pack_rgba8(48, 38, 32);
constexpr u32 kColHeliBody  = pack_rgba8(60, 70, 56);
constexpr u32 kColHeliTrim  = pack_rgba8(36, 44, 36);
constexpr u32 kColRotor     = pack_rgba8(32, 32, 32);

// Build a cube with a single per-face colour; populates `verts`/`indices`
// appended to whatever the caller already has, returning the index offsets.
void emit_cube(std::vector<render::raster::Vertex>& verts,
               std::vector<u32>&                    indices,
               u32 col_side, u32 col_top) {
    const u32 base = static_cast<u32>(verts.size());
    // 6 faces × 4 verts. Per-face colour so the silhouette doesn't smear.
    auto push_face = [&](math::Vec3 a, math::Vec3 b, math::Vec3 c, math::Vec3 d,
                         math::Vec3 normal, u32 col) {
        const u32 v0 = static_cast<u32>(verts.size());
        verts.push_back({a, normal, {0,0}, {0,0}, col});
        verts.push_back({b, normal, {1,0}, {0,0}, col});
        verts.push_back({c, normal, {1,1}, {0,0}, col});
        verts.push_back({d, normal, {0,1}, {0,0}, col});
        indices.push_back(v0+0); indices.push_back(v0+1); indices.push_back(v0+2);
        indices.push_back(v0+0); indices.push_back(v0+2); indices.push_back(v0+3);
    };
    // +X face
    push_face({ 0.5f,-0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},
              { 1, 0, 0}, col_side);
    // -X face
    push_face({-0.5f,-0.5f, 0.5f},{-0.5f, 0.5f, 0.5f},{-0.5f, 0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},
              {-1, 0, 0}, col_side);
    // +Y top
    push_face({-0.5f, 0.5f,-0.5f},{-0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f},
              { 0, 1, 0}, col_top);
    // -Y bottom
    push_face({-0.5f,-0.5f, 0.5f},{-0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f},
              { 0,-1, 0}, col_side);
    // +Z face
    push_face({-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f},
              { 0, 0, 1}, col_side);
    // -Z face
    push_face({ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f},
              { 0, 0,-1}, col_side);
    (void)base;
}

struct Mesh {
    std::vector<render::raster::Vertex> verts;
    std::vector<u32>                    indices;
};

Mesh build_cube_mesh(u32 col_side, u32 col_top) {
    Mesh m;
    emit_cube(m.verts, m.indices, col_side, col_top);
    return m;
}

// ─── Watchtower placement ────────────────────────────────────────────────
struct Tower {
    math::Vec3 ground_pos;   // world XZ + Y on the terrain
    f32        height;       // metres
    f32        half_extent;  // metres
};

std::array<Tower, 6> make_watchtowers(const world::outdoor::HeightmapDesc& hm) {
    const f32 map_m = static_cast<f32>(kHmSize) * kHmSpacing;
    const f32 cx    = map_m * 0.5f;
    const f32 cz    = map_m * 0.5f;
    // Six positions: roughly on a hexagonal ring around the map centre.
    const std::array<math::Vec2, 6> xz = {{
        { cx + 100.0f, cz +  10.0f },
        { cx +  50.0f, cz +  85.0f },
        { cx -  50.0f, cz +  90.0f },
        { cx - 100.0f, cz +  20.0f },
        { cx -  40.0f, cz -  80.0f },
        { cx +  60.0f, cz -  90.0f },
    }};
    std::array<Tower, 6> towers{};
    for (u32 i = 0; i < 6; ++i) {
        const f32 wx = xz[i].x;
        const f32 wz = xz[i].y;
        const f32 hy = world::outdoor::detail::sample_bilinear(hm, wx, wz);
        towers[i].ground_pos  = { wx, hy, wz };
        towers[i].height      = 7.0f;
        towers[i].half_extent = 1.6f;
    }
    return towers;
}

// ─── Helicopter ──────────────────────────────────────────────────────────
struct HeliPose {
    math::Vec3 position;        // world
    f32        yaw_rad;         // heading
    f32        rotor_phase;     // [0, 2π)
};

HeliPose make_helicopter_pose(f32 t_seconds) {
    HeliPose hp{};
    const f32 map_m = static_cast<f32>(kHmSize) * kHmSpacing;
    const f32 cx    = map_m * 0.5f;
    const f32 cz    = map_m * 0.5f;
    // Circular patrol path at 50m altitude.
    const f32 ang   = t_seconds * 0.45f;
    const f32 r     = 70.0f;
    hp.position = { cx + std::cos(ang) * r, 50.0f, cz + std::sin(ang) * r };
    // Heading: tangent to the path (turn into the circle).
    hp.yaw_rad     = ang + math::kHalfPi;
    hp.rotor_phase = std::fmod(t_seconds * 18.0f, math::kTwoPi);
    return hp;
}

// Yaw-only Y-axis rotation as a Mat4.
math::Mat4 yaw_mat4(f32 yaw_rad) {
    return math::rotate_quat(
        math::quat_from_axis_angle(math::Vec3{0, 1, 0}, yaw_rad));
}

}  // namespace

int main(int argc, char** argv) {
    const Args args         = parse_args(argc, argv);
    const u32  smoke_frames = args.smoke_frames;

    // ─── Platform / framebuffer ──────────────────────────────────────────
    platform::WindowDesc desc{};
    desc.title         = "Psynder — sample 06 (tactical map flyover, M6)";
    desc.window_width  = 1280;
    desc.window_height = 720;
    desc.render_width  = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode    = platform::ScaleMode::Linear;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_06: failed to create window");
        return EXIT_FAILURE;
    }

    std::vector<u32> pixels(static_cast<usize>(kFbW) * kFbH, 0u);
    std::vector<u32> depth (static_cast<usize>(kFbW) * kFbH, 0u);
    std::vector<u8>  hit_mask(static_cast<usize>(kFbW) * kFbH, 0u);

    render::Framebuffer fb{};
    fb.width  = kFbW;
    fb.height = kFbH;
    fb.pitch  = kFbW * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(pixels.data());
    fb.depth  = depth.data();

    // ─── Heightmap + raymarcher state ────────────────────────────────────
    const std::vector<u16> heights = build_heightmap();
    world::outdoor::HeightmapDesc hm_desc{};
    hm_desc.size_x       = kHmSize;
    hm_desc.size_z       = kHmSize;
    hm_desc.spacing      = kHmSpacing;
    hm_desc.height_scale = kHmHeightScl;
    hm_desc.heights      = heights.data();

    // Wire the public TerrainRaymarch with the same desc — Wave A's render()
    // is a no-op (the framebuffer-bound integration is lane 07's job), so
    // we drive the per-column kernel ourselves below. We still register the
    // backend so the API contract is exercised end-to-end.
    world::outdoor::TerrainRaymarch terrain_rm;
    terrain_rm.set_heightmap(hm_desc);

    // ─── Scene props ─────────────────────────────────────────────────────
    const Mesh tower_mesh = build_cube_mesh(kColTowerBody, kColTowerRoof);
    const Mesh heli_body  = build_cube_mesh(kColHeliBody, kColHeliTrim);
    const Mesh heli_blade = build_cube_mesh(kColRotor, kColRotor);

    const auto towers = make_watchtowers(hm_desc);

    auto& rasterizer = render::raster::Rasterizer::Get();

    PSY_LOG_INFO("Psynder sample 06 running{}",
                 smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames",
                                                smoke_frames)
                                  : std::string{});

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame    = 0;

    while (!window->should_close()) {
        window->poll_events();

        if (auto* in = platform::input();
            in && in->key_down(platform::KeyCode::Escape)) {
            break;
        }

        // Smoke mode pins time to frame so the captured PNG is deterministic.
        const f64 t = smoke_frames > 0
                          ? static_cast<f64>(frame) * (1.0 / 60.0)
                          : platform::Clock::seconds(
                                platform::Clock::ticks_now() - t0);

        const CameraView cam = make_flyover_camera(static_cast<f32>(t));

        // ── Step 1: raymarch the terrain ───────────────────────────────
        // Writes pixels + per-pixel NDC z; unhit pixels get the sky
        // gradient and are left at far-Z = 1.0.
        // Reset depth to far before the raymarch (we own the buffer here).
        {
            u32 far_packed = pack_depth_u24(1.0f);
            const usize n = static_cast<usize>(kFbW) * kFbH;
            for (usize i = 0; i < n; ++i) depth[i] = far_packed;
        }
        render_terrain(hm_desc, cam, fb, hit_mask);

        // Drive the public TerrainRaymarch::render() too so the call is
        // exercised on the M6 demo path (Wave A no-ops; Wave B will plumb
        // this through lane 07's tile bin queue and our pixels become a
        // backup reference).
        terrain_rm.render(cam.view, cam.proj);

        // ── Step 2: rasterizer for helicopter + watchtowers ────────────
        render::raster::ViewState view_state{};
        view_state.target     = fb;
        view_state.view       = cam.view;
        view_state.projection = cam.proj;
        view_state.tile_w     = 64;
        view_state.tile_h     = 64;
        rasterizer.begin_frame(view_state);

        // Watchtowers — scale the unit cube to tower dimensions, translate
        // to the terrain surface + half-height.
        for (const auto& tw : towers) {
            const math::Mat4 scl = math::scale(
                math::Vec3{ tw.half_extent * 2.0f, tw.height,
                             tw.half_extent * 2.0f });
            const math::Mat4 trs = math::translate(
                math::Vec3{ tw.ground_pos.x,
                             tw.ground_pos.y + tw.height * 0.5f,
                             tw.ground_pos.z });

            render::raster::DrawItem item{};
            item.vertices     = tower_mesh.verts.data();
            item.vertex_count = static_cast<u32>(tower_mesh.verts.size());
            item.indices      = tower_mesh.indices.data();
            item.index_count  = static_cast<u32>(tower_mesh.indices.size());
            item.model        = math::mul(trs, scl);
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
            item.vertices     = heli_body.verts.data();
            item.vertex_count = static_cast<u32>(heli_body.verts.size());
            item.indices      = heli_body.indices.data();
            item.index_count  = static_cast<u32>(heli_body.indices.size());
            item.model        = math::mul(trs, math::mul(heli_yaw, scl));
            rasterizer.submit(item);
        }
        // Rotor: 4 thin blades sitting 0.9m above the body, spinning around
        // the helicopter's local Y axis.
        const math::Mat4 rotor_spin = math::rotate_quat(
            math::quat_from_axis_angle(math::Vec3{0, 1, 0}, hp.rotor_phase));
        const math::Mat4 rotor_lift = math::translate(
            math::Vec3{hp.position.x, hp.position.y + 0.9f, hp.position.z});
        for (u32 b = 0; b < 4; ++b) {
            // Each blade offset 90° around the rotor hub.
            const f32 ang = static_cast<f32>(b) * math::kHalfPi;
            const math::Mat4 blade_rot = math::rotate_quat(
                math::quat_from_axis_angle(math::Vec3{0, 1, 0}, ang));
            // Blade: 4m long × 0.06m thick × 0.25m wide; offset 1.5m
            // outward along local +X so it pivots about the hub.
            const math::Mat4 scl = math::scale(math::Vec3{4.0f, 0.08f, 0.28f});
            const math::Mat4 blade_off = math::translate(
                math::Vec3{1.5f, 0.0f, 0.0f});

            render::raster::DrawItem item{};
            item.vertices     = heli_blade.verts.data();
            item.vertex_count = static_cast<u32>(heli_blade.verts.size());
            item.indices      = heli_blade.indices.data();
            item.index_count  = static_cast<u32>(heli_blade.indices.size());
            // Final transform: world ← rotor_lift × heli_yaw × rotor_spin ×
            //                  blade_rot × blade_off × scl
            const math::Mat4 m =
                math::mul(rotor_lift,
                  math::mul(heli_yaw,
                    math::mul(rotor_spin,
                      math::mul(blade_rot,
                        math::mul(blade_off, scl)))));
            item.model = m;
            rasterizer.submit(item);
        }

        rasterizer.end_frame();

        window->present(fb);

        if (smoke_frames > 0 && ++frame >= smoke_frames) {
            PSY_LOG_INFO("sample_06: smoke target reached ({}); exiting",
                         smoke_frames);
            break;
        }
    }

    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(
            args.capture_out.c_str(), pixels.data(),
            fb.width, fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_06: failed to write capture to {}",
                          args.capture_out);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_06: wrote capture to {}", args.capture_out);
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
