// SPDX-License-Identifier: MIT
// Psynder — lm_bake: offline lightmap baker (Lane 24 / tools).

#include "Bake.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "render/rt/Bvh.h"

namespace psynder::tools::bake {

namespace fs = std::filesystem;

namespace {

template <class T>
void append_le(std::vector<u8>& out, T value) {
    using U = std::make_unsigned_t<T>;
    auto u = static_cast<U>(value);
    for (usize i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<u8>(u >> (8 * i)));
    }
}
template <class T>
bool read_le(std::span<const u8> bytes, usize off, T& out) {
    if (off + sizeof(T) > bytes.size()) return false;
    using U = std::make_unsigned_t<T>;
    U u = 0;
    for (usize i = 0; i < sizeof(T); ++i) u |= static_cast<U>(bytes[off + i]) << (8 * i);
    out = static_cast<T>(u);
    return true;
}

// Möller–Trumbore single-ray triangle intersect. Returns (hit, t).
struct TriHit {
    bool hit = false;
    f32  t   = 0.0f;
};
TriHit intersect_tri(math::Vec3 ro, math::Vec3 rd,
                     math::Vec3 v0, math::Vec3 v1, math::Vec3 v2,
                     f32 t_max) {
    constexpr f32 kEps = 1e-7f;
    math::Vec3 e1 = math::sub(v1, v0);
    math::Vec3 e2 = math::sub(v2, v0);
    math::Vec3 p  = math::cross(rd, e2);
    f32 det = math::dot(e1, p);
    if (std::fabs(det) < kEps) return {};
    f32 inv = 1.0f / det;
    math::Vec3 s = math::sub(ro, v0);
    f32 u = math::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return {};
    math::Vec3 q = math::cross(s, e1);
    f32 v = math::dot(rd, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return {};
    f32 t = math::dot(e2, q) * inv;
    if (t < kEps || t > t_max) return {};
    return { true, t };
}

bool occluded(const BakeScene& scene, math::Vec3 ro, math::Vec3 rd, f32 t_max, u32 self_tri) {
    for (u32 i = 0; i < scene.triangles.size(); ++i) {
        if (i == self_tri) continue;
        const auto& tri = scene.triangles[i];
        auto h = intersect_tri(ro, rd, tri.v0, tri.v1, tri.v2, t_max);
        if (h.hit) return true;
    }
    return false;
}

// Closest-hit triangle ID (or -1). Used for indirect gather. Brute-force —
// the scene cardinality at bake time is small (the test scenes here have
// fewer than 100 tris). A BVH adapter via lane 08's Bvh8 is wired in
// `build_bake_bvh` below; this function is the fallback for cases where
// the BVH would be more overhead than help.
struct ClosestHit {
    i32 tri = -1;
    f32 t   = 1e30f;
};
ClosestHit closest_hit_bf(const BakeScene& scene, math::Vec3 ro, math::Vec3 rd,
                          f32 t_max, u32 self_tri) {
    ClosestHit best;
    for (u32 i = 0; i < scene.triangles.size(); ++i) {
        if (i == self_tri) continue;
        const auto& tri = scene.triangles[i];
        auto h = intersect_tri(ro, rd, tri.v0, tri.v1, tri.v2, t_max);
        if (h.hit && h.t < best.t) {
            best.t = h.t;
            best.tri = static_cast<i32>(i);
        }
    }
    return best;
}

// Build a cosine-weighted hemisphere sample around `normal` from a (u, v) pair
// in [0, 1)². cosθ = sqrt(1 − u), φ = 2π v.
math::Vec3 cosine_hemisphere(math::Vec3 normal, f32 u, f32 v) {
    constexpr f32 kPi = std::numbers::pi_v<f32>;
    f32 cos_t = std::sqrt(1.0f - u);
    f32 sin_t = std::sqrt(u);
    f32 phi   = 2.0f * kPi * v;
    f32 x = sin_t * std::cos(phi);
    f32 y = sin_t * std::sin(phi);
    f32 z = cos_t;
    // Build tangent frame around `normal`.
    math::Vec3 a = (std::fabs(normal.x) > 0.9f) ? math::Vec3{0, 1, 0} : math::Vec3{1, 0, 0};
    math::Vec3 t = math::cross(normal, a);
    f32 tl = std::sqrt(math::dot(t, t));
    if (tl > 1e-9f) t = math::mul(t, 1.0f / tl);
    math::Vec3 b = math::cross(normal, t);
    return math::add(math::add(math::mul(t, x), math::mul(b, y)),
                     math::mul(normal, z));
}

// Tiny hash for stratified jitter — keeps the bake deterministic across
// invocations (no RNG seed needed) and per-texel-per-sample.
u32 mix32(u32 a, u32 b, u32 c) {
    u32 h = a * 0x85ebca6bu;
    h ^= (h >> 13);
    h ^= b * 0xc2b2ae35u;
    h ^= (h >> 16);
    h ^= c * 0x27d4eb2fu;
    h ^= (h >> 15);
    return h;
}
f32 u32_to_f01(u32 x) { return (x >> 8) * (1.0f / 16777216.0f); }

// Build an orthonormal basis aligned with the triangle plane: u, v, n.
struct TriBasis {
    math::Vec3 origin;
    math::Vec3 u_axis;
    math::Vec3 v_axis;
    math::Vec3 normal;
    f32        u_len;        // length of v1-v0
    f32        v_extent;     // length of perpendicular extent
};
TriBasis build_basis(const BakeTriangle& tri) {
    TriBasis b{};
    b.origin = tri.v0;
    math::Vec3 e1 = math::sub(tri.v1, tri.v0);
    math::Vec3 e2 = math::sub(tri.v2, tri.v0);
    math::Vec3 n = math::cross(e1, e2);
    f32 nlen = std::sqrt(math::dot(n, n));
    if (nlen > 1e-12f) n = math::mul(n, 1.0f / nlen);
    f32 e1_len = std::sqrt(math::dot(e1, e1));
    math::Vec3 u_axis = e1;
    if (e1_len > 1e-12f) u_axis = math::mul(u_axis, 1.0f / e1_len);
    // v_axis perpendicular to u within the plane.
    math::Vec3 v_axis = math::cross(n, u_axis);
    // v extent = e2 projected onto v_axis.
    f32 v_extent = math::dot(e2, v_axis);
    b.normal = n;
    b.u_axis = u_axis;
    b.v_axis = v_axis;
    b.u_len = e1_len;
    b.v_extent = std::fabs(v_extent);
    return b;
}

// For grid texel (i, j) ∈ [0, res), return its world-space position
// and barycentrics. We sample the triangle in (u, v) parameter space:
//   u = (i + 0.5) / res, v = (j + 0.5) / res, accept if u+v <= 1.
struct TexelSample {
    math::Vec3 world{};
    bool       inside = false;
    f32        u = 0, v = 0;
};
TexelSample sample_texel(const BakeTriangle& tri, u32 i, u32 j, u32 res) {
    TexelSample ts{};
    f32 u = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(res);
    f32 v = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(res);
    if (u + v > 1.0f) return ts;
    ts.inside = true;
    ts.u = u;
    ts.v = v;
    math::Vec3 e1 = math::sub(tri.v1, tri.v0);
    math::Vec3 e2 = math::sub(tri.v2, tri.v0);
    ts.world = math::add(math::add(tri.v0, math::mul(e1, u)), math::mul(e2, v));
    return ts;
}

}  // anon namespace

BakedSurface bake_triangle_direct(const BakeScene& scene, u32 ti, const BakeOptions& opt) {
    BakedSurface surf{};
    if (ti >= scene.triangles.size()) return surf;
    const BakeTriangle& tri = scene.triangles[ti];
    TriBasis basis = build_basis(tri);

    surf.width = opt.lightmap_resolution;
    surf.height = opt.lightmap_resolution;
    surf.pixels.assign(static_cast<usize>(surf.width) * surf.height * 3u, 0.0f);

    for (u32 j = 0; j < surf.height; ++j) {
        for (u32 i = 0; i < surf.width; ++i) {
            TexelSample ts = sample_texel(tri, i, j, opt.lightmap_resolution);
            usize px = (static_cast<usize>(j) * surf.width + i) * 3u;
            if (!ts.inside) continue;
            math::Vec3 p = math::add(ts.world, math::mul(basis.normal, opt.ray_epsilon));

            // Loop over lights. Direct illumination Lambertian: I/r² * max(N·L, 0).
            f32 r = 0, g = 0, b = 0;
            for (const auto& light : scene.lights) {
                math::Vec3 L{};
                f32 t_max = 0;
                f32 atten = 0;
                if (light.kind == LightKind::kPoint) {
                    L = math::sub(light.position, p);
                    f32 dist = std::sqrt(math::dot(L, L));
                    if (dist < 1e-6f) continue;
                    L = math::mul(L, 1.0f / dist);
                    t_max = dist;
                    atten = light.intensity / (dist * dist);
                } else {
                    // Directional: `direction` points along the light's
                    // travel; flip for the incoming vector.
                    L = math::mul(light.direction, -1.0f);
                    f32 ln = std::sqrt(math::dot(L, L));
                    if (ln > 1e-9f) L = math::mul(L, 1.0f / ln);
                    t_max = 1e6f;
                    atten = light.intensity;
                }
                f32 ndotl = std::max(0.0f, math::dot(basis.normal, L));
                if (ndotl <= 0.0f) continue;
                if (occluded(scene, p, L, t_max, ti)) continue;
                r += atten * ndotl * light.color.x;
                g += atten * ndotl * light.color.y;
                b += atten * ndotl * light.color.z;
            }
            surf.pixels[px + 0] = r;
            surf.pixels[px + 1] = g;
            surf.pixels[px + 2] = b;
        }
    }
    return surf;
}

namespace {

// Sample the *current* lightmap of `tri_idx` at world-space hit `p_world`.
// We invert the texel mapping back to barycentrics. Returns linear RGB
// from the current per-texel pixel buffer (no filtering — Wave-C job).
math::Vec3 sample_surface_radiance(const BakeScene& scene,
                                   const BakedAtlas& atlas,
                                   u32 tri_idx,
                                   math::Vec3 p_world) {
    const BakeTriangle& tri = scene.triangles[tri_idx];
    const BakedSurface& surf = atlas.surfaces[tri_idx];
    if (surf.width == 0 || surf.height == 0) return {0,0,0};
    // Solve p = v0 + u·e1 + v·e2 → (u, v) in [0,1] with u+v ≤ 1.
    math::Vec3 e1 = math::sub(tri.v1, tri.v0);
    math::Vec3 e2 = math::sub(tri.v2, tri.v0);
    math::Vec3 q  = math::sub(p_world, tri.v0);
    f32 d00 = math::dot(e1, e1);
    f32 d01 = math::dot(e1, e2);
    f32 d11 = math::dot(e2, e2);
    f32 d20 = math::dot(q, e1);
    f32 d21 = math::dot(q, e2);
    f32 denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) < 1e-9f) return {0,0,0};
    f32 inv = 1.0f / denom;
    f32 u = (d11 * d20 - d01 * d21) * inv;
    f32 v = (d00 * d21 - d01 * d20) * inv;
    if (u < 0 || v < 0 || u + v > 1.0f) return {0,0,0};
    i32 i = std::clamp(static_cast<i32>(u * static_cast<f32>(surf.width)),
                       0, static_cast<i32>(surf.width) - 1);
    i32 j = std::clamp(static_cast<i32>(v * static_cast<f32>(surf.height)),
                       0, static_cast<i32>(surf.height) - 1);
    usize px = (static_cast<usize>(j) * surf.width + static_cast<usize>(i)) * 3u;
    return { surf.pixels[px + 0], surf.pixels[px + 1], surf.pixels[px + 2] };
}

// Optional lane-08 BVH adapter. Built once per bake(); used only as a
// visibility accelerator for hemisphere-gather rays. Brute-force closest-
// hit still goes through `closest_hit_bf` because the BVH8 returns only
// `Hit::primitive` per its public ABI and we need the same index in the
// brute-force path's testing — kept identical for verification.
struct BakeBvh {
    render::rt::Bvh8 bvh;
    bool             built = false;
};
void build_bake_bvh(const BakeScene& scene, BakeBvh& out) {
    if (scene.triangles.empty()) return;
    std::vector<render::rt::Triangle> tris;
    tris.reserve(scene.triangles.size());
    for (const auto& t : scene.triangles) {
        tris.push_back({ t.v0, t.v1, t.v2 });
    }
    out.bvh.build(tris.data(), static_cast<u32>(tris.size()));
    out.built = true;
}

// Replace the per-texel `out_surf` pixels with `direct + albedo * indirect`,
// where `indirect` is the mean cosine-weighted gather from the *current*
// atlas state (which captures all prior bounces).
void apply_bounce(const BakeScene& scene,
                  const BakedAtlas& prev_atlas,
                  const std::vector<BakedSurface>& direct_layer,
                  BakedAtlas& out_atlas,
                  const BakeOptions& opt,
                  u32 bounce_index) {
    constexpr f32 kPi = std::numbers::pi_v<f32>;
    for (u32 ti = 0; ti < scene.triangles.size(); ++ti) {
        const BakeTriangle& tri = scene.triangles[ti];
        BakedSurface& surf = out_atlas.surfaces[ti];
        const BakedSurface& direct = direct_layer[ti];
        // Build basis once per triangle.
        TriBasis basis = build_basis(tri);

        for (u32 j = 0; j < surf.height; ++j) {
            for (u32 i = 0; i < surf.width; ++i) {
                TexelSample ts = sample_texel(tri, i, j, opt.lightmap_resolution);
                usize px = (static_cast<usize>(j) * surf.width + i) * 3u;
                if (!ts.inside) {
                    surf.pixels[px + 0] = 0;
                    surf.pixels[px + 1] = 0;
                    surf.pixels[px + 2] = 0;
                    continue;
                }
                math::Vec3 p = math::add(ts.world, math::mul(basis.normal, opt.ray_epsilon));

                math::Vec3 indirect{0, 0, 0};
                u32 n_ok = 0;
                for (u32 s = 0; s < opt.indirect_samples_per_bounce; ++s) {
                    // Stratify (u, v) on √N × √N grid plus per-sample jitter.
                    u32 grid = static_cast<u32>(std::sqrt(static_cast<f32>(opt.indirect_samples_per_bounce))) + 1u;
                    u32 sx = s % grid;
                    u32 sy = s / grid;
                    u32 h = mix32(ti * 73856093u + i * 19349663u + j * 83492791u,
                                  bounce_index * 49979693u + s * 39916801u, 0xa5a5a5a5u);
                    f32 ju = (static_cast<f32>(sx) + u32_to_f01(h)) / static_cast<f32>(grid);
                    f32 jv = (static_cast<f32>(sy) + u32_to_f01(h ^ 0xdeadbeefu)) / static_cast<f32>(grid);
                    if (ju >= 1.0f) ju = 1.0f - 1e-4f;
                    if (jv >= 1.0f) jv = 1.0f - 1e-4f;
                    math::Vec3 dir = cosine_hemisphere(basis.normal, ju, jv);
                    f32 dn = std::sqrt(math::dot(dir, dir));
                    if (dn > 1e-9f) dir = math::mul(dir, 1.0f / dn);
                    ClosestHit ch = closest_hit_bf(scene, p, dir, 1e6f, ti);
                    if (ch.tri < 0) continue;
                    math::Vec3 hit_p = math::add(p, math::mul(dir, ch.t));
                    math::Vec3 L = sample_surface_radiance(scene, prev_atlas,
                                                           static_cast<u32>(ch.tri), hit_p);
                    indirect = math::add(indirect, L);
                    ++n_ok;
                }
                // PDF for cosine-weighted is cosθ / π. Lambertian BRDF is
                // albedo / π. cosθ from the geometry term cancels with the
                // PDF cosθ, leaving the gather weight as `albedo · π / π`
                // = `albedo`. So mean(L) * albedo is the indirect contribution.
                f32 inv = (n_ok > 0) ? (1.0f / static_cast<f32>(n_ok)) : 0.0f;
                math::Vec3 ind = math::mul(indirect, inv);
                f32 r = direct.pixels[px + 0] + tri.albedo.x * ind.x;
                f32 g = direct.pixels[px + 1] + tri.albedo.y * ind.y;
                f32 b = direct.pixels[px + 2] + tri.albedo.z * ind.z;
                surf.pixels[px + 0] = r;
                surf.pixels[px + 1] = g;
                surf.pixels[px + 2] = b;
                (void)kPi;   // PDF cancellation noted above.
            }
        }
    }
}

}  // anon

BakedAtlas bake(const BakeScene& scene, const BakeOptions& opt) {
    BakedAtlas atlas;
    atlas.surfaces.reserve(scene.triangles.size());
    for (u32 i = 0; i < scene.triangles.size(); ++i) {
        atlas.surfaces.push_back(bake_triangle_direct(scene, i, opt));
    }
    if (opt.max_indirect_bounces == 0) return atlas;
    // Lane 08 BVH adapter (built but not the visibility primary today —
    // brute-force keeps the path tracer deterministic against the same-
    // primitive-index closest-hit on small scenes). Kept here so the
    // Wave-C raymarcher-on-BVH8 swap is a one-line change.
    BakeBvh bvh{};
    build_bake_bvh(scene, bvh);
    (void)bvh;
    // Snapshot direct layer for the (direct + albedo*indirect) reconstruction.
    std::vector<BakedSurface> direct_layer = atlas.surfaces;
    // Iteratively gather one bounce at a time. Each bounce reads `atlas`
    // (which already reflects prior bounces) and writes into a scratch
    // copy, then swaps.
    BakedAtlas scratch = atlas;
    for (u32 b = 0; b < opt.max_indirect_bounces; ++b) {
        apply_bounce(scene, /*prev_atlas=*/atlas, direct_layer,
                     /*out_atlas=*/scratch, opt, /*bounce_index=*/b);
        std::swap(atlas.surfaces, scratch.surfaces);
    }
    return atlas;
}

// ─── f32 ↔ f16 ───────────────────────────────────────────────────────────

u16 f32_to_f16(f32 v) noexcept {
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    u32 sign = (bits >> 31) & 1u;
    i32 exp  = static_cast<i32>((bits >> 23) & 0xFFu) - 127;
    u32 mant = bits & 0x7FFFFFu;

    if (exp == 128) {                                  // Inf / NaN
        return static_cast<u16>((sign << 15) | 0x7C00u | (mant ? 0x0200u : 0u));
    }
    if (exp > 15) {                                    // overflow to inf
        return static_cast<u16>((sign << 15) | 0x7C00u);
    }
    if (exp >= -14) {                                  // normalized
        return static_cast<u16>((sign << 15) | (static_cast<u32>(exp + 15) << 10) | (mant >> 13));
    }
    if (exp >= -25) {                                  // subnormal
        mant |= 0x800000u;
        u32 shift = static_cast<u32>(-14 - exp + 13);
        return static_cast<u16>((sign << 15) | (mant >> shift));
    }
    return static_cast<u16>(sign << 15);              // underflow to zero
}

f32 f16_to_f32(u16 h) noexcept {
    u32 sign = (h >> 15) & 1u;
    u32 exp  = (h >> 10) & 0x1Fu;
    u32 mant = h & 0x3FFu;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign << 31; }
        else {
            // subnormal -> normalize
            int e = -14;
            while ((mant & 0x400u) == 0) { mant <<= 1; --e; }
            mant &= 0x3FFu;
            bits = (sign << 31) | (static_cast<u32>(e + 127) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = (sign << 31) | 0x7F800000u | (mant << 13);
    } else {
        bits = (sign << 31) | (static_cast<u32>(exp + (127 - 15)) << 23) | (mant << 13);
    }
    f32 out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// ─── .lmlight ────────────────────────────────────────────────────────────

void write_lmlight(const BakedAtlas& atlas, std::vector<u8>& out) {
    out.clear();
    append_le<u32>(out, kLmlMagic);
    append_le<u32>(out, kLmlVersion);
    append_le<u32>(out, static_cast<u32>(atlas.surfaces.size()));
    append_le<u32>(out, 0);  // reserved

    for (const auto& s : atlas.surfaces) {
        append_le<u32>(out, s.width);
        append_le<u32>(out, s.height);
        for (f32 px : s.pixels) {
            u16 h = f32_to_f16(px);
            append_le<u16>(out, h);
        }
    }
}

bool read_lmlight(std::span<const u8> bytes, BakedAtlas& out, std::string* err) {
    auto fail = [&](const char* msg) { if (err) *err = msg; return false; };
    if (bytes.size() < 16) return fail("lmlight header truncated");
    u32 magic = 0; read_le<u32>(bytes, 0, magic);
    if (magic != kLmlMagic) return fail("lmlight bad magic");
    u32 version = 0; read_le<u32>(bytes, 4, version);
    if (version != kLmlVersion) return fail("lmlight unsupported version");
    u32 count = 0; read_le<u32>(bytes, 8, count);
    usize cursor = 16;
    out.surfaces.clear(); out.surfaces.resize(count);
    for (u32 i = 0; i < count; ++i) {
        u32 w = 0, h = 0;
        if (!read_le<u32>(bytes, cursor + 0, w)) return fail("lmlight surf header");
        if (!read_le<u32>(bytes, cursor + 4, h)) return fail("lmlight surf header");
        cursor += 8;
        BakedSurface s;
        s.width = w; s.height = h;
        s.pixels.resize(static_cast<usize>(w) * h * 3u);
        for (usize px = 0; px < s.pixels.size(); ++px) {
            u16 hv = 0;
            if (!read_le<u16>(bytes, cursor, hv)) return fail("lmlight pixel truncated");
            s.pixels[px] = f16_to_f32(hv);
            cursor += 2;
        }
        out.surfaces[i] = std::move(s);
    }
    return true;
}

// ─── CLI ─────────────────────────────────────────────────────────────────

void print_help() {
    std::fprintf(stdout,
        "lm_bake — Psynder offline lightmap baker\n"
        "\n"
        "Usage:\n"
        "  lm_bake <scene.psyscene> <out.lmlight> [--resolution N] [--bounces N]\n"
        "                                          [--samples N]\n"
        "  lm_bake --help\n"
        "\n"
        "Flags:\n"
        "  --resolution N   texels per triangle edge (default 8)\n"
        "  --bounces N      indirect bounces (Wave-B: 0=direct only, 2-4 recommended)\n"
        "  --samples N      hemisphere gather samples per texel per bounce (default 16)\n"
        "\n"
        ".psyscene is a tiny ad-hoc text format (see Bake.cpp::parse_scene)\n"
        "good for round-trip tests. Real scenes will come out of lm_qbsp +\n"
        "lm_cook in Wave-B.\n");
}

namespace {

// A very small text scene format for CLI testing. Each line is one of:
//
//   tri V0X V0Y V0Z V1X V1Y V1Z V2X V2Y V2Z
//   light_point X Y Z R G B INTENSITY
//   light_dir   DX DY DZ R G B INTENSITY
//
// Comments start with #. This is intentionally minimal — production scenes
// flow through .psybsp / .lmm assets and the engine, not this CLI shim.
bool parse_scene(std::string_view text, BakeScene& out, std::string& err) {
    usize i = 0;
    while (i < text.size()) {
        usize end = text.find('\n', i);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(i, end - i);
        i = end + 1;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty() || line.front() == '#') continue;

        std::vector<std::string> tok;
        usize p = 0;
        while (p < line.size()) {
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r')) ++p;
            usize start = p;
            while (p < line.size() && line[p] != ' ' && line[p] != '\t' && line[p] != '\r') ++p;
            if (p > start) tok.emplace_back(line.substr(start, p - start));
        }
        if (tok.empty()) continue;
        auto& cmd = tok[0];
        auto pf = [](const std::string& s, f32& v) {
            char* e = nullptr;
            v = std::strtof(s.c_str(), &e);
            return e != s.c_str();
        };
        if (cmd == "tri" && tok.size() >= 10) {
            BakeTriangle t{};
            f32 vals[9];
            for (int k = 0; k < 9; ++k) {
                if (!pf(tok[1 + k], vals[k])) { err = "bake: bad tri"; return false; }
            }
            t.v0 = {vals[0], vals[1], vals[2]};
            t.v1 = {vals[3], vals[4], vals[5]};
            t.v2 = {vals[6], vals[7], vals[8]};
            t.normal = math::normalize(math::cross(math::sub(t.v1, t.v0), math::sub(t.v2, t.v0)));
            // Optional 3 trailing floats: albedo RGB.
            if (tok.size() >= 13) {
                f32 a[3];
                if (pf(tok[10], a[0]) && pf(tok[11], a[1]) && pf(tok[12], a[2])) {
                    t.albedo = { a[0], a[1], a[2] };
                }
            }
            out.triangles.push_back(t);
        } else if (cmd == "light_point" && tok.size() >= 8) {
            BakeLight l{};
            l.kind = LightKind::kPoint;
            f32 v[7];
            for (int k = 0; k < 7; ++k) if (!pf(tok[1 + k], v[k])) { err = "bake: bad point light"; return false; }
            l.position = {v[0], v[1], v[2]};
            l.color    = {v[3], v[4], v[5]};
            l.intensity = v[6];
            out.lights.push_back(l);
        } else if (cmd == "light_dir" && tok.size() >= 8) {
            BakeLight l{};
            l.kind = LightKind::kDirectional;
            f32 v[7];
            for (int k = 0; k < 7; ++k) if (!pf(tok[1 + k], v[k])) { err = "bake: bad dir light"; return false; }
            l.direction = math::normalize(math::Vec3{v[0], v[1], v[2]});
            l.color     = {v[3], v[4], v[5]};
            l.intensity = v[6];
            out.lights.push_back(l);
        } else {
            // ignore unknown — forward-compat.
        }
    }
    return true;
}

bool read_file(const fs::path& p, std::string& out, std::string& err) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { err = "cannot open " + p.string(); return false; }
    in.seekg(0, std::ios::end);
    out.resize(static_cast<usize>(in.tellg()));
    in.seekg(0, std::ios::beg);
    if (!out.empty()) in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(in);
}
bool write_file(const fs::path& p, std::span<const u8> data, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot write " + p.string(); return false; }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

}  // anon

int cli_main(int argc, char** argv) {
    if (argc < 2) { print_help(); return 1; }
    std::string_view a = argv[1];
    if (a == "--help" || a == "-h" || a == "help") { print_help(); return 0; }
    if (argc < 3) { print_help(); return 1; }
    BakeOptions opt;
    for (int i = 3; i < argc; ++i) {
        std::string_view k = argv[i];
        if (k == "--resolution" && i + 1 < argc) opt.lightmap_resolution = static_cast<u32>(std::atoi(argv[++i]));
        else if (k == "--bounces" && i + 1 < argc) opt.max_indirect_bounces = static_cast<u32>(std::atoi(argv[++i]));
        else if (k == "--samples" && i + 1 < argc) opt.indirect_samples_per_bounce = static_cast<u32>(std::atoi(argv[++i]));
    }
    std::string text, err;
    if (!read_file(fs::path(argv[1]), text, err)) {
        std::fprintf(stderr, "lm_bake: %s\n", err.c_str());
        return 1;
    }
    BakeScene scene;
    if (!parse_scene(text, scene, err)) {
        std::fprintf(stderr, "lm_bake: %s\n", err.c_str());
        return 1;
    }
    BakedAtlas atlas = bake(scene, opt);
    std::vector<u8> bytes;
    write_lmlight(atlas, bytes);
    if (!write_file(fs::path(argv[2]), bytes, err)) {
        std::fprintf(stderr, "lm_bake: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stdout, "lm_bake: baked %u surfaces -> %s\n",
                 static_cast<u32>(atlas.surfaces.size()), argv[2]);
    return 0;
}

}  // namespace psynder::tools::bake
