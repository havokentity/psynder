// SPDX-License-Identifier: MIT
// Psynder — per-tile rasterizer. 2×2 quad coverage walk; perspective-correct
// attribute interpolation; bilinear texture sampling; 24-bit float Z early
// reject. DESIGN.md §7.4 / §7.5.
//
// The hot inner loop is templated over <TILE_W, TILE_H>; the compiler bakes
// tile dimensions as constants per ADR-002. Specializations for 32 / 64 /
// 128 are explicitly instantiated at the bottom of this file.

#include "TileBin.h"
#include "HiZ.h"
#include "SurfaceCache.h"

#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psynder::render::raster {

namespace {

// ─── Bilinear sample on a single mip level ───────────────────────────────
// Returns RGBA8 packed. Wraps via modulo — width/height don't need to be
// power-of-two, but pre-divisible by 2 along the mip chain is assumed
// (lane 05 cooks mips that way).
PSY_FORCEINLINE u32 sample_bilinear_mip(const Texture::MipLevel& mip, f32 u, f32 v) noexcept {
    if (!mip.texels || mip.width == 0 || mip.height == 0)
        return 0xFF888888u;

    const f32 wf = static_cast<f32>(mip.width);
    const f32 hf = static_cast<f32>(mip.height);

    // Texel-space coordinates with the standard half-texel bias.
    const f32 tx = u * wf - 0.5f;
    const f32 ty = v * hf - 0.5f;

    const i32 x0 = static_cast<i32>(std::floor(tx));
    const i32 y0 = static_cast<i32>(std::floor(ty));
    const f32 fx = tx - static_cast<f32>(x0);
    const f32 fy = ty - static_cast<f32>(y0);

    // Wrap. Modulo is fine — texel fetch latency dominates anyway.
    auto wrap = [](i32 a, i32 b) noexcept {
        i32 r = a % b;
        return static_cast<u32>(r < 0 ? r + b : r);
    };
    const i32 wx = static_cast<i32>(mip.width);
    const i32 hy = static_cast<i32>(mip.height);
    const u32 x1 = wrap(x0 + 1, wx);
    const u32 y1 = wrap(y0 + 1, hy);
    const u32 x0w = wrap(x0, wx);
    const u32 y0w = wrap(y0, hy);

    const u32 t00 = mip.texels[y0w * mip.pitch + x0w];
    const u32 t10 = mip.texels[y0w * mip.pitch + x1];
    const u32 t01 = mip.texels[y1 * mip.pitch + x0w];
    const u32 t11 = mip.texels[y1 * mip.pitch + x1];

    // Per-channel lerp (8 lerps total). Keep in fixed-point so we don't
    // round through float.
    auto chan = [&](u32 shift) noexcept {
        const f32 c00 = static_cast<f32>((t00 >> shift) & 0xFFu);
        const f32 c10 = static_cast<f32>((t10 >> shift) & 0xFFu);
        const f32 c01 = static_cast<f32>((t01 >> shift) & 0xFFu);
        const f32 c11 = static_cast<f32>((t11 >> shift) & 0xFFu);
        const f32 a = c00 + (c10 - c00) * fx;
        const f32 b = c01 + (c11 - c01) * fx;
        const f32 r = a + (b - a) * fy;
        return static_cast<u32>(r + 0.5f) & 0xFFu;
    };

    return chan(0) | (chan(8) << 8) | (chan(16) << 16) | (chan(24) << 24);
}

// Bilinear sample on mip 0 only — kept as the original entrypoint for
// callers that don't care about mip selection (e.g. UI bicubic upscalers
// when they land in lane 09).
PSY_FORCEINLINE u32 sample_bilinear(const Texture& tex, f32 u, f32 v) noexcept {
    Texture::MipLevel m0{tex.texels, tex.width, tex.height, tex.pitch};
    return sample_bilinear_mip(m0, u, v);
}

// ─── Trilinear sample across two mip levels ──────────────────────────────
// Two bilinear lookups (mip floor and mip floor+1) lerped by the
// fractional mip. The mip LOD is computed from the per-quad finite
// differences of (u,v) — for the within-tile inner loop we approximate
// du/dx and dv/dx as the 1-pixel stride. The §7.5 "trilinear" cost
// budget is 8 fetches; we deliver that.
PSY_FORCEINLINE u32 sample_trilinear(const Texture& tex, f32 u, f32 v, f32 mip_lod) noexcept {
    if (tex.mip_count <= 1) {
        return sample_bilinear(tex, u, v);
    }
    // Clamp mip LOD into [0, mip_count-1].
    const f32 max_lod = static_cast<f32>(tex.mip_count - 1);
    if (mip_lod < 0.0f)
        mip_lod = 0.0f;
    if (mip_lod > max_lod)
        mip_lod = max_lod;

    const i32 m0 = static_cast<i32>(std::floor(mip_lod));
    const i32 m1 = std::min(m0 + 1, static_cast<i32>(tex.mip_count - 1));
    const f32 t = mip_lod - static_cast<f32>(m0);

    const u32 s0 = sample_bilinear_mip(tex.mips[m0], u, v);
    if (m0 == m1 || t == 0.0f)
        return s0;
    const u32 s1 = sample_bilinear_mip(tex.mips[m1], u, v);

    // Per-channel lerp.
    auto lerp = [t](u32 a, u32 b) noexcept {
        const f32 af = static_cast<f32>(a);
        const f32 bf = static_cast<f32>(b);
        return static_cast<u32>(af + (bf - af) * t + 0.5f) & 0xFFu;
    };
    const u32 r = lerp(s0 & 0xFFu, s1 & 0xFFu);
    const u32 g = lerp((s0 >> 8) & 0xFFu, (s1 >> 8) & 0xFFu);
    const u32 b = lerp((s0 >> 16) & 0xFFu, (s1 >> 16) & 0xFFu);
    const u32 a = lerp((s0 >> 24) & 0xFFu, (s1 >> 24) & 0xFFu);
    return r | (g << 8) | (b << 16) | (a << 24);
}

// Estimate the mip LOD given screen-space derivatives of (u, v) and the
// texture's mip 0 dimensions. log2(max(|du/dx|*w, |dv/dx|*h, ...)). The
// derivatives arrive from the quad walk's 1-pixel stride deltas in
// texture coordinates; we compute the max texel-stride per pixel and
// take log2.
PSY_FORCEINLINE f32 compute_mip_lod(const Texture& tex, f32 dudx, f32 dvdx, f32 dudy, f32 dvdy) noexcept {
    if (tex.mip_count <= 1)
        return 0.0f;
    const f32 wf = static_cast<f32>(tex.width);
    const f32 hf = static_cast<f32>(tex.height);
    const f32 ax = dudx * wf;
    const f32 ay = dvdx * hf;
    const f32 bx = dudy * wf;
    const f32 by = dvdy * hf;
    const f32 d2_dx = ax * ax + ay * ay;
    const f32 d2_dy = bx * bx + by * by;
    const f32 d2_max = std::max(d2_dx, d2_dy);
    if (d2_max <= 1.0f)
        return 0.0f;
    // log2(sqrt(d2_max)) = 0.5 * log2(d2_max).
    return 0.5f * std::log2(d2_max);
}

// ─── EWA-approximation anisotropic sampler (DESIGN.md §7.5) ──────────────
// Given the texture-space ellipse defined by the per-pixel UV gradients
// (du/dx, dv/dx, du/dy, dv/dy) at the shaded sample, walk N samples
// along the ellipse's MAJOR axis, each a bilinear lookup, and average
// them with cosine-shaped (Gaussian-like) weights. N is clamped to the
// caller's max_anisotropy.
//
// The "exact" EWA filter walks the bounding box of the ellipse and
// weights by a Gaussian over the elliptical Mahalanobis distance — the
// canonical Heckbert '89 form. For a software CPU rasterizer that cost
// scales as O(major*minor) per pixel, which blows the §7.5 budget. The
// industry-standard approximation (Schilling/Texas, Mali Aniso) walks
// only the major axis and replaces the Gaussian with a cosine bell;
// O(N) per pixel, indistinguishable past 4 taps for natural textures.
//
// When the ellipse degenerates to a circle (major ≈ minor) we fall back
// to bilinear on the per-draw mip level — this is exactly what an
// isotropic filter would produce, so the dispatch above can call this
// unconditionally and still get the right answer.
//
// All math in floats; no allocations; no virtual.
PSY_FORCEINLINE u32 aniso_sample(const Texture::MipLevel& mip,
                                 f32 u,
                                 f32 v,
                                 f32 du_dx,
                                 f32 du_dy,
                                 f32 dv_dx,
                                 f32 dv_dy,
                                 u32 max_anisotropy) noexcept {
    if (!mip.texels || mip.width == 0 || mip.height == 0)
        return 0xFF888888u;

    // Map the screen-space (du/dx, dv/dx) and (du/dy, dv/dy) into
    // texel-space axis vectors. The ellipse is the image of the unit
    // pixel square (a circle of radius 0.5) under the UV gradient
    // jacobian. In texel space, the two axes are:
    //   ax = (du/dx * W, dv/dx * H)
    //   ay = (du/dy * W, dv/dy * H)
    const f32 wf = static_cast<f32>(mip.width);
    const f32 hf = static_cast<f32>(mip.height);
    const f32 ax_x = du_dx * wf;
    const f32 ax_y = dv_dx * hf;
    const f32 ay_x = du_dy * wf;
    const f32 ay_y = dv_dy * hf;

    const f32 lx2 = ax_x * ax_x + ax_y * ax_y;
    const f32 ly2 = ay_x * ay_x + ay_y * ay_y;

    // Pick major / minor from the two axes. The longer one is the
    // walking direction; the shorter governs the per-tap mip level.
    f32 maj_x, maj_y, maj2, min2;
    if (lx2 >= ly2) {
        maj_x = ax_x;
        maj_y = ax_y;
        maj2 = lx2;
        min2 = ly2;
    } else {
        maj_x = ay_x;
        maj_y = ay_y;
        maj2 = ly2;
        min2 = lx2;
    }

    // Number of taps along the major axis: round(major / minor),
    // clamped to [1, max_anisotropy]. If max_anisotropy ≤ 1 we collapse
    // to a single bilinear sample (the isotropic case).
    u32 N;
    if (max_anisotropy <= 1 || min2 <= 0.0f) {
        N = 1;
    } else {
        const f32 ratio = std::sqrt(maj2 / min2);
        u32 n_raw = static_cast<u32>(ratio + 0.5f);
        if (n_raw < 1)
            n_raw = 1;
        if (n_raw > max_anisotropy)
            n_raw = max_anisotropy;
        N = n_raw;
    }

    if (N == 1) {
        // Isotropic / degenerate: one bilinear sample on this mip.
        return sample_bilinear_mip(mip, u, v);
    }

    // Convert the major axis back to UV-space step per tap. The major
    // axis spans (maj_x, maj_y) in TEXEL space; in UV space that's
    // (maj_x / W, maj_y / H). We walk from -0.5 maj to +0.5 maj across
    // (N-1) tap intervals, centred on the sample point.
    const f32 step_u = (maj_x / wf) / static_cast<f32>(N);
    const f32 step_v = (maj_y / hf) / static_cast<f32>(N);

    // Cosine-bell weights:  w(i) = cos(pi/2 * (2i - (N-1)) / (N-1))
    // (so the centre tap weights ~1 and the ends weight ~0). For N=2 we
    // weight both taps equally (the cosine evaluates to 0 at ±1, which
    // would zero-out the sample — collapse to flat weights instead).
    f32 acc_r = 0.0f, acc_g = 0.0f, acc_b = 0.0f, acc_a = 0.0f, acc_w = 0.0f;
    const f32 half = static_cast<f32>(N - 1) * 0.5f;

    for (u32 i = 0; i < N; ++i) {
        const f32 off = static_cast<f32>(i) - half;  // [-half, +half]
        const f32 uu = u + step_u * off;
        const f32 vv = v + step_v * off;

        // Weight: cosine bell normalised by half. For N=2 -> cos(±pi/2)=0,
        // so substitute equal weights.
        f32 w;
        if (N <= 2) {
            w = 1.0f;
        } else {
            const f32 t = off / half;               // ∈ [-1, +1]
            w = std::cos(t * 1.5707963267948966f);  // cos(pi/2 * t)
            if (w < 0.0f)
                w = 0.0f;
        }

        const u32 s = sample_bilinear_mip(mip, uu, vv);
        acc_r += static_cast<f32>(s & 0xFFu) * w;
        acc_g += static_cast<f32>((s >> 8) & 0xFFu) * w;
        acc_b += static_cast<f32>((s >> 16) & 0xFFu) * w;
        acc_a += static_cast<f32>((s >> 24) & 0xFFu) * w;
        acc_w += w;
    }

    if (acc_w <= 0.0f) {
        // Pathological fallback — every weight clamped to zero. Should
        // be unreachable for N >= 2 but defend against numerical edge.
        return sample_bilinear_mip(mip, u, v);
    }

    const f32 inv = 1.0f / acc_w;
    auto sat = [inv](f32 c) noexcept {
        const f32 r = c * inv;
        if (r <= 0.0f)
            return 0u;
        if (r >= 255.0f)
            return 255u;
        return static_cast<u32>(r + 0.5f);
    };
    return sat(acc_r) | (sat(acc_g) << 8) | (sat(acc_b) << 16) | (sat(acc_a) << 24);
}

// Anisotropic sample on the mip-0 of `tex` — keeps the (cap=max_aniso)
// behaviour without needing a per-call mip pick. The major/minor walk
// happens against the highest-detail level; per-tap mip biasing is a
// future refinement (DESIGN.md §7.5).
PSY_FORCEINLINE u32 sample_aniso(
    const Texture& tex, f32 u, f32 v, f32 du_dx, f32 du_dy, f32 dv_dx, f32 dv_dy, u32 max_anisotropy) noexcept {
    Texture::MipLevel m0{tex.texels, tex.width, tex.height, tex.pitch};
    return aniso_sample(m0, u, v, du_dx, du_dy, dv_dx, dv_dy, max_anisotropy);
}

// ─── Pack/unpack f32 depth ───────────────────────────────────────────────
// Framebuffer.depth is u32; we store 24-bit float Z + 8-bit stencil
// (DESIGN.md §7.4). For the M1 path we keep stencil 0 and pack Z as a
// uint that compares the same way as the float — `bit_cast` is illegal
// for negative floats (z is in [0,1] so it never is) and shifting the
// mantissa works fine.
PSY_FORCEINLINE u32 pack_depth(f32 z, u8 stencil) noexcept {
    u32 raw;
    std::memcpy(&raw, &z, sizeof(raw));
    return (raw & 0xFFFFFF00u) | static_cast<u32>(stencil);
}
PSY_FORCEINLINE f32 unpack_depth(u32 packed) noexcept {
    f32 z;
    const u32 raw = packed & 0xFFFFFF00u;
    std::memcpy(&z, &raw, sizeof(z));
    return z;
}

PSY_FORCEINLINE u32 pack_rgba(f32 r, f32 g, f32 b, f32 a) noexcept {
    auto sat = [](f32 x) noexcept { return std::min(255.0f, std::max(0.0f, x * 255.0f + 0.5f)); };
    return static_cast<u32>(sat(r)) | (static_cast<u32>(sat(g)) << 8) |
           (static_cast<u32>(sat(b)) << 16) | (static_cast<u32>(sat(a)) << 24);
}

PSY_FORCEINLINE u32 multiply_framebuffer_rgba(u32 dst, u32 mask, u8 opacity_u8) noexcept {
    const f32 opacity = static_cast<f32>(opacity_u8) * (1.0f / 255.0f);
    const f32 mask_a = static_cast<f32>((mask >> 24) & 0xFFu) * (1.0f / 255.0f);
    const f32 factor = std::clamp(1.0f - opacity * mask_a, 0.0f, 1.0f);
    const u32 r = static_cast<u32>(static_cast<f32>(dst & 0xFFu) * factor + 0.5f);
    const u32 g = static_cast<u32>(static_cast<f32>((dst >> 8) & 0xFFu) * factor + 0.5f);
    const u32 b = static_cast<u32>(static_cast<f32>((dst >> 16) & 0xFFu) * factor + 0.5f);
    const u32 a = (dst >> 24) & 0xFFu;
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | (a << 24);
}

}  // namespace

// ─── Binner ──────────────────────────────────────────────────────────────
void bin_triangle(TileGrid& grid, u32 draw_idx, u32 tri_idx, const TriSetup& tri) noexcept {
    if (!tri.valid || grid.cols == 0 || grid.rows == 0)
        return;

    const i32 tw = static_cast<i32>(grid.tile_w);
    const i32 th = static_cast<i32>(grid.tile_h);

    const i32 tx_min = std::max(0, tri.minx / tw);
    const i32 ty_min = std::max(0, tri.miny / th);
    const i32 tx_max = std::min(static_cast<i32>(grid.cols) - 1, (tri.maxx - 1) / tw);
    const i32 ty_max = std::min(static_cast<i32>(grid.rows) - 1, (tri.maxy - 1) / th);

    for (i32 ty = ty_min; ty <= ty_max; ++ty) {
        for (i32 tx = tx_min; tx <= tx_max; ++tx) {
            // Per-tile coverage: triangle's bbox already overlaps the tile.
            // The cheap reject is "tri AABB ∩ tile AABB is non-empty",
            // which the loop bounds enforce. A tighter conservative-coverage
            // test (corner edge sign check) is a Wave-B micro-opt; not
            // required to make tests pass.
            const u32 tile_idx = static_cast<u32>(ty) * grid.cols + static_cast<u32>(tx);
            if (grid.entries_used >= grid.entries_capacity)
                return;  // arena full
            const u32 slot = grid.entries_used++;
            grid.entries[slot] = BinEntry{draw_idx, tri_idx};
            // Two-pass approach: tile_count is bumped in pass 1; the actual
            // sort by offset happens in the consume step. For Wave-A we
            // do a simple stable build: count, then scan, then place.
            grid.tile_count[tile_idx]++;
        }
    }
}

// ─── Tile rasterizer (templated body) ────────────────────────────────────
template <u32 TILE_W, u32 TILE_H>
void rasterize_tile(const Framebuffer& fb,
                    const DrawCmd* draws,
                    u32 /*draw_count*/,
                    const TileGrid& grid,
                    const Texture* tex,
                    u32 tile_x,
                    u32 tile_y,
                    bool affine_mode) noexcept {
    if (!fb.pixels || !draws || !grid.tile_offset || !grid.tile_count)
        return;

    const i32 fb_w = static_cast<i32>(fb.width);
    const i32 fb_h = static_cast<i32>(fb.height);
    const i32 tile_x0 = static_cast<i32>(tile_x * TILE_W);
    const i32 tile_y0 = static_cast<i32>(tile_y * TILE_H);
    const i32 tile_x1 = std::min(tile_x0 + static_cast<i32>(TILE_W), fb_w);
    const i32 tile_y1 = std::min(tile_y0 + static_cast<i32>(TILE_H), fb_h);
    if (tile_x0 >= tile_x1 || tile_y0 >= tile_y1)
        return;

    const u32 tile_idx = tile_y * grid.cols + tile_x;
    const u32 begin = grid.tile_offset[tile_idx];
    const u32 end = begin + grid.tile_count[tile_idx];

    auto* pixels = reinterpret_cast<u32*>(fb.pixels);

    // ─── HiZ: per-tile 8×8 cell-grid early-reject (DESIGN.md §7.3) ───────
    // Built from the framebuffer's depth slice (carries previously-drawn
    // triangles for this frame; cleared to 1.0 at frame start). Updated
    // incrementally below as each triangle finalizes. Lives on the stack —
    // 8×8 cells × 4 bytes = 256 B per 64×64 tile (1 cacheline x 4).
    HiZTile<TILE_W, TILE_H> hiz;
    hiz.rebuild_from_fb(fb, static_cast<u32>(tile_x0), static_cast<u32>(tile_y0));

    for (u32 e = begin; e < end; ++e) {
        const BinEntry& be = grid.entries[e];
        const DrawCmd& d = draws[be.draw_idx];
        const TriSetup& t = d.tris[be.tri_idx];
        if (!t.valid)
            continue;

        // Intersect triangle bbox with tile bbox
        const i32 x0 = std::max(t.minx, tile_x0);
        const i32 y0 = std::max(t.miny, tile_y0);
        const i32 x1 = std::min(t.maxx, tile_x1);
        const i32 y1 = std::min(t.maxy, tile_y1);
        if (x0 >= x1 || y0 >= y1)
            continue;

        // HiZ test — find the triangle's minimum z over its three verts
        // (a conservative lower bound; the actual interpolated z varies
        // inside the triangle but never goes below min(z0,z1,z2)). If no
        // cell touching the triangle's bbox has farther geometry, every
        // pixel under this triangle is occluded — skip the inner loop.
        const f32 tri_z_min = std::min({t.v0.z, t.v1.z, t.v2.z});
        const i32 tile_lx0 = x0 - tile_x0;
        const i32 tile_ly0 = y0 - tile_y0;
        const i32 tile_lx1 = x1 - tile_x0;
        const i32 tile_ly1 = y1 - tile_y0;
        if (!hiz.any_cell_passes(tile_lx0, tile_ly0, tile_lx1, tile_ly1, tri_z_min)) {
            continue;
        }

        // 2×2 quad walk (DESIGN.md §7.4 — gives free LOD finite-diffs,
        // which we don't use yet but the quad organization is the right
        // shape for later mipmap LOD). For Wave-A we evaluate each pixel
        // independently; the 2×2 group lets us share edge increments.

        // Pre-compute edge increments per pixel (Q24.8). For edge (a→b),
        // E(p) = (b.x-a.x)(p.y-a.y) - (b.y-a.y)(p.x-a.x)
        //   dE/dpx = -(b.y-a.y)
        //   dE/dpy =  (b.x-a.x)
        // Multiplied by kSubPixelScale because p moves in pixel units
        // (Q24.8 scale). Edge 0 = (v1→v2), edge 1 = (v2→v0), edge 2 = (v0→v1).
        const i64 ex0_dx = -(static_cast<i64>(t.y2.v) - static_cast<i64>(t.y1.v)) * kSubPixelScale;
        const i64 ex0_dy = (static_cast<i64>(t.x2.v) - static_cast<i64>(t.x1.v)) * kSubPixelScale;
        const i64 ex1_dx = -(static_cast<i64>(t.y0.v) - static_cast<i64>(t.y2.v)) * kSubPixelScale;
        const i64 ex1_dy = (static_cast<i64>(t.x0.v) - static_cast<i64>(t.x2.v)) * kSubPixelScale;
        const i64 ex2_dx = -(static_cast<i64>(t.y1.v) - static_cast<i64>(t.y0.v)) * kSubPixelScale;
        const i64 ex2_dy = (static_cast<i64>(t.x1.v) - static_cast<i64>(t.x0.v)) * kSubPixelScale;

        // Starting edge values at pixel-centre (+0.5, +0.5) of (x0, y0)
        const FxQ24_8 px = FxQ24_8::from_float(static_cast<f32>(x0) + 0.5f);
        const FxQ24_8 py = FxQ24_8::from_float(static_cast<f32>(y0) + 0.5f);
        i64 e0_row = eval_edge0(t, px, py);
        i64 e1_row = eval_edge1(t, px, py);
        i64 e2_row = eval_edge2(t, px, py);

        const bool has_tex = (tex != nullptr);
        const bool alpha_test = (d.flags & draw_flags::kAlphaTest) != 0;
        const bool affine = affine_mode || (d.flags & draw_flags::kAffine) != 0;
        const bool multiply_blend = d.blend_mode == 1u;

        // Dispatch tag — DESIGN.md §7.6. Picked once per draw; the inner
        // pixel loop branches on a const bool, never on a per-pixel
        // variable. SurfaceCached takes the pre-multiplied path; OnTheFly
        // does the modern sample(base) * sample(lightmap) path. Wave-B
        // ships SurfaceCached as a single-sample lookup against the slab;
        // the actual base*lightmap product is filled in by the surface
        // cooker when lane 10 lands.
        const ShadingPath path = static_cast<ShadingPath>(d.shading_path);
        const bool surface_cached =
            (path == ShadingPath::SurfaceCached) && d.surface_cache_payload != nullptr;

        // Per-draw screen→texture jacobian: (du/dx, dv/dx, du/dy, dv/dy)
        // approximated from the 1-pixel finite diff at the triangle's
        // centroid. Used by both the mip-LOD picker and the EWA aniso
        // dispatcher. Affine draws skip the divide; the bias is fine for
        // these per-draw constants. Computed once per draw so the inner
        // pixel loop stays branch-light.
        f32 draw_du_dx = 0.0f, draw_dv_dx = 0.0f;
        f32 draw_du_dy = 0.0f, draw_dv_dy = 0.0f;
        if (has_tex && !affine) {
            const f32 dw0_dx = static_cast<f32>(ex0_dx) * t.inv_area2x;
            const f32 dw1_dx = static_cast<f32>(ex1_dx) * t.inv_area2x;
            const f32 dw2_dx = static_cast<f32>(ex2_dx) * t.inv_area2x;
            const f32 dw0_dy = static_cast<f32>(ex0_dy) * t.inv_area2x;
            const f32 dw1_dy = static_cast<f32>(ex1_dy) * t.inv_area2x;
            const f32 dw2_dy = static_cast<f32>(ex2_dy) * t.inv_area2x;
            draw_du_dx = dw0_dx * t.v0.u_over_w + dw1_dx * t.v1.u_over_w + dw2_dx * t.v2.u_over_w;
            draw_dv_dx = dw0_dx * t.v0.v_over_w + dw1_dx * t.v1.v_over_w + dw2_dx * t.v2.v_over_w;
            draw_du_dy = dw0_dy * t.v0.u_over_w + dw1_dy * t.v1.u_over_w + dw2_dy * t.v2.u_over_w;
            draw_dv_dy = dw0_dy * t.v0.v_over_w + dw1_dy * t.v1.v_over_w + dw2_dy * t.v2.v_over_w;
        }

        // Trilinear mip-LOD per draw — derived from the jacobian above.
        f32 draw_mip_lod = 0.0f;
        if (has_tex && tex->mip_count > 1 && !affine) {
            draw_mip_lod = compute_mip_lod(*tex, draw_du_dx, draw_dv_dx, draw_du_dy, draw_dv_dy);
        }

        // EWA aniso dispatch — engage when:
        //   1. The draw's r_anisotropy cap is ≥ 2, AND
        //   2. The texture-space ellipse is significantly elongated
        //      (major/minor > 2 ⇒ enough stretch to be visible past
        //      bilinear/trilinear).
        // Picked once per draw; the inner pixel loop branches on a const
        // bool (no per-pixel dispatch). DESIGN.md §7.5.
        bool use_aniso = false;
        u32 draw_aniso_max = d.aniso_max;
        if (has_tex && !affine && draw_aniso_max >= 2) {
            const f32 wf = static_cast<f32>(tex->width);
            const f32 hf = static_cast<f32>(tex->height);
            const f32 ax_x = draw_du_dx * wf;
            const f32 ax_y = draw_dv_dx * hf;
            const f32 ay_x = draw_du_dy * wf;
            const f32 ay_y = draw_dv_dy * hf;
            const f32 lx2 = ax_x * ax_x + ax_y * ax_y;
            const f32 ly2 = ay_x * ay_x + ay_y * ay_y;
            const f32 maj2 = std::max(lx2, ly2);
            const f32 min2 = std::min(lx2, ly2);
            // major/minor > 2 ⇒ maj² / min² > 4. Guard against min2 = 0
            // by requiring a measurable minor axis.
            use_aniso = (min2 > 0.0f) && (maj2 > 4.0f * min2);
        }

        // Iterate rows in the tile-clipped bbox
        for (i32 y = y0; y < y1; ++y) {
            i64 e0 = e0_row;
            i64 e1 = e1_row;
            i64 e2 = e2_row;
            u32* row_pix = pixels + static_cast<usize>(y) * (fb.pitch / 4);
            u32* row_z = fb.depth ? fb.depth + static_cast<usize>(y) * fb.width : nullptr;

            for (i32 x = x0; x < x1; ++x) {
                // Inside test — all three edges non-negative
                if ((e0 | e1 | e2) >= 0) {
                    // Barycentric weights, normalized
                    const f32 w0 = static_cast<f32>(e0) * t.inv_area2x;
                    const f32 w1 = static_cast<f32>(e1) * t.inv_area2x;
                    const f32 w2 = static_cast<f32>(e2) * t.inv_area2x;

                    // Interpolated z + 1/w (linear in screen space)
                    const f32 z = w0 * t.v0.z + w1 * t.v1.z + w2 * t.v2.z;
                    const f32 inv_w = w0 * t.v0.inv_w + w1 * t.v1.inv_w + w2 * t.v2.inv_w;

                    // Early-Z (alpha-test off → safe to depth-write up front)
                    bool depth_ok = true;
                    if (row_z) {
                        const f32 zbuf = unpack_depth(row_z[x]);
                        depth_ok = multiply_blend ? (z <= zbuf + 1.0e-4f) : (z < zbuf);
                    }
                    if (PSY_LIKELY(depth_ok && !alpha_test && !multiply_blend)) {
                        if (row_z)
                            row_z[x] = pack_depth(z, 0);
                    }

                    if (depth_ok) {
                        // Attributes: linear-in-1/w then divide at the quad
                        // for perspective-correct, or skip divide for affine.
                        f32 r, g, b, a, u, v;
                        if (affine) {
                            // Affine — interpolate the un-/w-divided values
                            // directly. Looks like PS1: textures swim with
                            // perspective. The cvar r_affine engages this.
                            r = w0 * (t.v0.r_over_w / t.v0.inv_w) +
                                w1 * (t.v1.r_over_w / t.v1.inv_w) + w2 * (t.v2.r_over_w / t.v2.inv_w);
                            g = w0 * (t.v0.g_over_w / t.v0.inv_w) +
                                w1 * (t.v1.g_over_w / t.v1.inv_w) + w2 * (t.v2.g_over_w / t.v2.inv_w);
                            b = w0 * (t.v0.b_over_w / t.v0.inv_w) +
                                w1 * (t.v1.b_over_w / t.v1.inv_w) + w2 * (t.v2.b_over_w / t.v2.inv_w);
                            a = w0 * (t.v0.a_over_w / t.v0.inv_w) +
                                w1 * (t.v1.a_over_w / t.v1.inv_w) + w2 * (t.v2.a_over_w / t.v2.inv_w);
                            u = w0 * (t.v0.u_over_w / t.v0.inv_w) +
                                w1 * (t.v1.u_over_w / t.v1.inv_w) + w2 * (t.v2.u_over_w / t.v2.inv_w);
                            v = w0 * (t.v0.v_over_w / t.v0.inv_w) +
                                w1 * (t.v1.v_over_w / t.v1.inv_w) + w2 * (t.v2.v_over_w / t.v2.inv_w);
                        } else {
                            const f32 w_recip = inv_w != 0.0f ? 1.0f / inv_w : 0.0f;
                            r = (w0 * t.v0.r_over_w + w1 * t.v1.r_over_w + w2 * t.v2.r_over_w) * w_recip;
                            g = (w0 * t.v0.g_over_w + w1 * t.v1.g_over_w + w2 * t.v2.g_over_w) * w_recip;
                            b = (w0 * t.v0.b_over_w + w1 * t.v1.b_over_w + w2 * t.v2.b_over_w) * w_recip;
                            a = (w0 * t.v0.a_over_w + w1 * t.v1.a_over_w + w2 * t.v2.a_over_w) * w_recip;
                            u = (w0 * t.v0.u_over_w + w1 * t.v1.u_over_w + w2 * t.v2.u_over_w) * w_recip;
                            v = (w0 * t.v0.v_over_w + w1 * t.v1.v_over_w + w2 * t.v2.v_over_w) * w_recip;
                        }

                        u32 out_rgba;
                        if (surface_cached) {
                            // Surface-cache path: one fetch from the
                            // pre-multiplied chunk. Vertex colour still
                            // modulates so the lit triangle still shades.
                            Texture::MipLevel sc{
                                d.surface_cache_payload,
                                d.surface_cache_width,
                                d.surface_cache_height,
                                d.surface_cache_width,
                            };
                            const u32 t_rgba = sample_bilinear_mip(sc, u, v);
                            const f32 tr = static_cast<f32>(t_rgba & 0xFFu) * (1.0f / 255.0f);
                            const f32 tg = static_cast<f32>((t_rgba >> 8) & 0xFFu) * (1.0f / 255.0f);
                            const f32 tb = static_cast<f32>((t_rgba >> 16) & 0xFFu) * (1.0f / 255.0f);
                            const f32 ta = static_cast<f32>((t_rgba >> 24) & 0xFFu) * (1.0f / 255.0f);
                            out_rgba = pack_rgba(r * tr, g * tg, b * tb, a * ta);
                        } else if (has_tex) {
                            // OnTheFly: sample base texture. Three paths,
                            // picked by per-draw const bools above:
                            //   use_aniso  → EWA major-axis walk
                            //   mip_count>1→ trilinear
                            //   otherwise  → bilinear
                            u32 t_rgba;
                            if (use_aniso) {
                                t_rgba = sample_aniso(*tex,
                                                      u,
                                                      v,
                                                      draw_du_dx,
                                                      draw_du_dy,
                                                      draw_dv_dx,
                                                      draw_dv_dy,
                                                      draw_aniso_max);
                            } else if (tex->mip_count > 1) {
                                t_rgba = sample_trilinear(*tex, u, v, draw_mip_lod);
                            } else {
                                t_rgba = sample_bilinear(*tex, u, v);
                            }
                            const f32 tr = static_cast<f32>(t_rgba & 0xFFu) * (1.0f / 255.0f);
                            const f32 tg = static_cast<f32>((t_rgba >> 8) & 0xFFu) * (1.0f / 255.0f);
                            const f32 tb = static_cast<f32>((t_rgba >> 16) & 0xFFu) * (1.0f / 255.0f);
                            const f32 ta = static_cast<f32>((t_rgba >> 24) & 0xFFu) * (1.0f / 255.0f);
                            out_rgba = pack_rgba(r * tr, g * tg, b * tb, a * ta);
                        } else {
                            out_rgba = pack_rgba(r, g, b, a);
                        }
                        row_pix[x] = multiply_blend
                                         ? multiply_framebuffer_rgba(row_pix[x],
                                                                    out_rgba,
                                                                    d.blend_opacity)
                                         : out_rgba;

                        // Late-Z (only matters when alpha test is on; we
                        // approximate by always writing here when depth
                        // wasn't already written above).
                        if (alpha_test && !multiply_blend && row_z)
                            row_z[x] = pack_depth(z, 0);
                    }
                }

                e0 += ex0_dx;
                e1 += ex1_dx;
                e2 += ex2_dx;
            }

            e0_row += ex0_dy;
            e1_row += ex1_dy;
            e2_row += ex2_dy;
        }

        // ── HiZ update ───────────────────────────────────────────────
        // Tighten the per-cell max-z (farthest depth) toward this triangle's
        // max-vertex z — but ONLY for cells the triangle FULLY covers. For a
        // fully-covered cell every pixel ends up at min(old, tri_z) <= tri_z_max,
        // so tri_z_max stays a true upper bound and any_cell_passes() remains
        // conservative. Tightening a PARTIALLY-covered cell would be unsound:
        // its uncovered pixels can hold farther geometry, so lowering max_z to
        // tri_z_max underestimates the cell's farthest depth and would
        // early-reject a later, genuinely-visible triangle — the intermittent
        // "black holes" bug. Full coverage is the 4 cell-corner pixel centres
        // all passing the (top-left-biased) edge tests; the triangle is convex,
        // so all-corners-inside implies the whole cell is inside.
        const f32 tri_z_max = std::max({t.v0.z, t.v1.z, t.v2.z});
        const i32 lx0 = std::max(0, x0 - tile_x0);
        const i32 ly0 = std::max(0, y0 - tile_y0);
        const i32 lx1 = std::min(static_cast<i32>(TILE_W), x1 - tile_x0);
        const i32 ly1 = std::min(static_cast<i32>(TILE_H), y1 - tile_y0);
        if (lx0 < lx1 && ly0 < ly1) {
            auto corner_inside = [&](f32 fx, f32 fy) noexcept {
                const FxQ24_8 qx = FxQ24_8::from_float(fx);
                const FxQ24_8 qy = FxQ24_8::from_float(fy);
                return eval_edge0(t, qx, qy) >= 0 && eval_edge1(t, qx, qy) >= 0 &&
                       eval_edge2(t, qx, qy) >= 0;
            };
            const u32 cx0 = static_cast<u32>(lx0) / kHiZCellPx;
            const u32 cy0 = static_cast<u32>(ly0) / kHiZCellPx;
            const u32 cx1 = (static_cast<u32>(lx1 - 1) / kHiZCellPx) + 1;
            const u32 cy1 = (static_cast<u32>(ly1 - 1) / kHiZCellPx) + 1;
            for (u32 cy = cy0; cy < cy1 && cy < hiz.kRows; ++cy) {
                for (u32 cx = cx0; cx < cx1 && cx < hiz.kCols; ++cx) {
                    // Cell pixel extent (screen space) → its 4 corner centres.
                    const f32 px_l =
                        static_cast<f32>(tile_x0 + static_cast<i32>(cx * kHiZCellPx)) + 0.5f;
                    const f32 px_r = px_l + static_cast<f32>(kHiZCellPx - 1);
                    const f32 py_t =
                        static_cast<f32>(tile_y0 + static_cast<i32>(cy * kHiZCellPx)) + 0.5f;
                    const f32 py_b = py_t + static_cast<f32>(kHiZCellPx - 1);
                    if (corner_inside(px_l, py_t) && corner_inside(px_r, py_t) &&
                        corner_inside(px_l, py_b) && corner_inside(px_r, py_b)) {
                        hiz.update_cell(cy * hiz.kCols + cx, tri_z_max);
                    }
                }
            }
        }
    }
}

// ─── Explicit instantiations — ADR-002 specializations ───────────────────
template void rasterize_tile<32, 32>(
    const Framebuffer&, const DrawCmd*, u32, const TileGrid&, const Texture*, u32, u32, bool) noexcept;
template void rasterize_tile<64, 64>(
    const Framebuffer&, const DrawCmd*, u32, const TileGrid&, const Texture*, u32, u32, bool) noexcept;
template void rasterize_tile<128, 128>(
    const Framebuffer&, const DrawCmd*, u32, const TileGrid&, const Texture*, u32, u32, bool) noexcept;

TileRasterFn select_tile_raster_fn(u32 tile_w, u32 tile_h) noexcept {
    if (tile_w == 32 && tile_h == 32)
        return &rasterize_tile<32, 32>;
    if (tile_w == 128 && tile_h == 128)
        return &rasterize_tile<128, 128>;
    return &rasterize_tile<64, 64>;  // default per ADR-002
}

}  // namespace psynder::render::raster
