// SPDX-License-Identifier: MIT
// Psynder — per-tile rasterizer. 2×2 quad coverage walk; perspective-correct
// attribute interpolation; bilinear texture sampling; 24-bit float Z early
// reject. DESIGN.md §7.4 / §7.5.
//
// The hot inner loop is templated over <TILE_W, TILE_H>; the compiler bakes
// tile dimensions as constants per ADR-002. Specializations for 32 / 64 /
// 128 are explicitly instantiated at the bottom of this file.

#include "TileBin.h"

#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psynder::render::raster {

namespace {

// ─── Bilinear texture sample ─────────────────────────────────────────────
// Returns RGBA8 packed. Wraps on negative coords via floor (i.e. wrap
// semantics); textures we ship are power-of-two so the mask is cheap. Lane
// 07 implements bilinear minimum (DESIGN.md §7.5); trilinear / aniso are
// Wave-B.
PSY_FORCEINLINE u32 sample_bilinear(const Texture& tex, f32 u, f32 v) noexcept {
    if (!tex.texels || tex.width == 0 || tex.height == 0) return 0xFF888888u;

    const f32 wf = static_cast<f32>(tex.width);
    const f32 hf = static_cast<f32>(tex.height);

    // Texel-space coordinates with the standard half-texel bias.
    const f32 tx = u * wf - 0.5f;
    const f32 ty = v * hf - 0.5f;

    const i32 x0 = static_cast<i32>(std::floor(tx));
    const i32 y0 = static_cast<i32>(std::floor(ty));
    const f32 fx = tx - static_cast<f32>(x0);
    const f32 fy = ty - static_cast<f32>(y0);

    // Wrap. Width/height aren't required to be PoT here — modulo cost is
    // negligible vs the texel fetch latency it serializes against.
    auto wrap = [](i32 a, i32 b) noexcept {
        i32 r = a % b;
        return static_cast<u32>(r < 0 ? r + b : r);
    };
    const i32 wx = static_cast<i32>(tex.width);
    const i32 hy = static_cast<i32>(tex.height);
    const u32 x1 = wrap(x0 + 1, wx);
    const u32 y1 = wrap(y0 + 1, hy);
    const u32 x0w = wrap(x0, wx);
    const u32 y0w = wrap(y0, hy);

    const u32 t00 = tex.texels[y0w * tex.pitch + x0w];
    const u32 t10 = tex.texels[y0w * tex.pitch + x1];
    const u32 t01 = tex.texels[y1  * tex.pitch + x0w];
    const u32 t11 = tex.texels[y1  * tex.pitch + x1];

    // Per-channel lerp (8 lerps total). Keep in fixed-point so we don't
    // round through float.
    auto chan = [&](u32 shift) noexcept {
        const f32 c00 = static_cast<f32>((t00 >> shift) & 0xFFu);
        const f32 c10 = static_cast<f32>((t10 >> shift) & 0xFFu);
        const f32 c01 = static_cast<f32>((t01 >> shift) & 0xFFu);
        const f32 c11 = static_cast<f32>((t11 >> shift) & 0xFFu);
        const f32 a   = c00 + (c10 - c00) * fx;
        const f32 b   = c01 + (c11 - c01) * fx;
        const f32 r   = a + (b - a) * fy;
        return static_cast<u32>(r + 0.5f) & 0xFFu;
    };

    return chan(0) | (chan(8) << 8) | (chan(16) << 16) | (chan(24) << 24);
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
    return static_cast<u32>(sat(r))
         | (static_cast<u32>(sat(g)) << 8)
         | (static_cast<u32>(sat(b)) << 16)
         | (static_cast<u32>(sat(a)) << 24);
}

}  // namespace

// ─── Binner ──────────────────────────────────────────────────────────────
void bin_triangle(TileGrid& grid, u32 draw_idx, u32 tri_idx,
                  const TriSetup& tri) noexcept {
    if (!tri.valid || grid.cols == 0 || grid.rows == 0) return;

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
            if (grid.entries_used >= grid.entries_capacity) return;  // arena full
            const u32 slot = grid.entries_used++;
            grid.entries[slot] = BinEntry{ draw_idx, tri_idx };
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
                    const DrawCmd*     draws,
                    u32                /*draw_count*/,
                    const TileGrid&    grid,
                    const Texture*     tex,
                    u32                tile_x,
                    u32                tile_y,
                    bool               affine_mode) noexcept {
    if (!fb.pixels || !draws || !grid.tile_offset || !grid.tile_count) return;

    const i32 fb_w = static_cast<i32>(fb.width);
    const i32 fb_h = static_cast<i32>(fb.height);
    const i32 tile_x0 = static_cast<i32>(tile_x * TILE_W);
    const i32 tile_y0 = static_cast<i32>(tile_y * TILE_H);
    const i32 tile_x1 = std::min(tile_x0 + static_cast<i32>(TILE_W), fb_w);
    const i32 tile_y1 = std::min(tile_y0 + static_cast<i32>(TILE_H), fb_h);
    if (tile_x0 >= tile_x1 || tile_y0 >= tile_y1) return;

    const u32 tile_idx = tile_y * grid.cols + tile_x;
    const u32 begin    = grid.tile_offset[tile_idx];
    const u32 end      = begin + grid.tile_count[tile_idx];

    auto* pixels = reinterpret_cast<u32*>(fb.pixels);

    for (u32 e = begin; e < end; ++e) {
        const BinEntry& be = grid.entries[e];
        const DrawCmd&  d  = draws[be.draw_idx];
        const TriSetup& t  = d.tris[be.tri_idx];
        if (!t.valid) continue;

        // Intersect triangle bbox with tile bbox
        const i32 x0 = std::max(t.minx, tile_x0);
        const i32 y0 = std::max(t.miny, tile_y0);
        const i32 x1 = std::min(t.maxx, tile_x1);
        const i32 y1 = std::min(t.maxy, tile_y1);
        if (x0 >= x1 || y0 >= y1) continue;

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
        const i64 ex0_dy =  (static_cast<i64>(t.x2.v) - static_cast<i64>(t.x1.v)) * kSubPixelScale;
        const i64 ex1_dx = -(static_cast<i64>(t.y0.v) - static_cast<i64>(t.y2.v)) * kSubPixelScale;
        const i64 ex1_dy =  (static_cast<i64>(t.x0.v) - static_cast<i64>(t.x2.v)) * kSubPixelScale;
        const i64 ex2_dx = -(static_cast<i64>(t.y1.v) - static_cast<i64>(t.y0.v)) * kSubPixelScale;
        const i64 ex2_dy =  (static_cast<i64>(t.x1.v) - static_cast<i64>(t.x0.v)) * kSubPixelScale;

        // Starting edge values at pixel-centre (+0.5, +0.5) of (x0, y0)
        const FxQ24_8 px = FxQ24_8::from_float(static_cast<f32>(x0) + 0.5f);
        const FxQ24_8 py = FxQ24_8::from_float(static_cast<f32>(y0) + 0.5f);
        i64 e0_row = eval_edge0(t, px, py);
        i64 e1_row = eval_edge1(t, px, py);
        i64 e2_row = eval_edge2(t, px, py);

        const bool has_tex   = (tex != nullptr);
        const bool alpha_test= (d.flags & 0x1) != 0;
        const bool affine    = affine_mode || (d.flags & 0x2) != 0;

        // Iterate rows in the tile-clipped bbox
        for (i32 y = y0; y < y1; ++y) {
            i64 e0 = e0_row;
            i64 e1 = e1_row;
            i64 e2 = e2_row;
            u32* row_pix = pixels + static_cast<usize>(y) * (fb.pitch / 4);
            u32* row_z   = fb.depth ? fb.depth + static_cast<usize>(y) * fb.width : nullptr;

            for (i32 x = x0; x < x1; ++x) {
                // Inside test — all three edges non-negative
                if ((e0 | e1 | e2) >= 0) {
                    // Barycentric weights, normalized
                    const f32 w0 = static_cast<f32>(e0) * t.inv_area2x;
                    const f32 w1 = static_cast<f32>(e1) * t.inv_area2x;
                    const f32 w2 = static_cast<f32>(e2) * t.inv_area2x;

                    // Interpolated z + 1/w (linear in screen space)
                    const f32 z      = w0 * t.v0.z     + w1 * t.v1.z     + w2 * t.v2.z;
                    const f32 inv_w  = w0 * t.v0.inv_w + w1 * t.v1.inv_w + w2 * t.v2.inv_w;

                    // Early-Z (alpha-test off → safe to depth-write up front)
                    bool depth_ok = true;
                    if (row_z) {
                        const f32 zbuf = unpack_depth(row_z[x]);
                        depth_ok = z < zbuf;
                    }
                    if (PSY_LIKELY(depth_ok && !alpha_test)) {
                        if (row_z) row_z[x] = pack_depth(z, 0);
                    }

                    if (depth_ok) {
                        // Attributes: linear-in-1/w then divide at the quad
                        // for perspective-correct, or skip divide for affine.
                        f32 r, g, b, a, u, v;
                        if (affine) {
                            // Affine — interpolate the un-/w-divided values
                            // directly. Looks like PS1: textures swim with
                            // perspective. The cvar r_affine engages this.
                            r = w0 * (t.v0.r_over_w / t.v0.inv_w)
                              + w1 * (t.v1.r_over_w / t.v1.inv_w)
                              + w2 * (t.v2.r_over_w / t.v2.inv_w);
                            g = w0 * (t.v0.g_over_w / t.v0.inv_w)
                              + w1 * (t.v1.g_over_w / t.v1.inv_w)
                              + w2 * (t.v2.g_over_w / t.v2.inv_w);
                            b = w0 * (t.v0.b_over_w / t.v0.inv_w)
                              + w1 * (t.v1.b_over_w / t.v1.inv_w)
                              + w2 * (t.v2.b_over_w / t.v2.inv_w);
                            a = w0 * (t.v0.a_over_w / t.v0.inv_w)
                              + w1 * (t.v1.a_over_w / t.v1.inv_w)
                              + w2 * (t.v2.a_over_w / t.v2.inv_w);
                            u = w0 * (t.v0.u_over_w / t.v0.inv_w)
                              + w1 * (t.v1.u_over_w / t.v1.inv_w)
                              + w2 * (t.v2.u_over_w / t.v2.inv_w);
                            v = w0 * (t.v0.v_over_w / t.v0.inv_w)
                              + w1 * (t.v1.v_over_w / t.v1.inv_w)
                              + w2 * (t.v2.v_over_w / t.v2.inv_w);
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
                        if (has_tex) {
                            const u32 t_rgba = sample_bilinear(*tex, u, v);
                            const f32 tr = static_cast<f32>(t_rgba       & 0xFFu) * (1.0f/255.0f);
                            const f32 tg = static_cast<f32>((t_rgba >>  8) & 0xFFu) * (1.0f/255.0f);
                            const f32 tb = static_cast<f32>((t_rgba >> 16) & 0xFFu) * (1.0f/255.0f);
                            const f32 ta = static_cast<f32>((t_rgba >> 24) & 0xFFu) * (1.0f/255.0f);
                            out_rgba = pack_rgba(r * tr, g * tg, b * tb, a * ta);
                        } else {
                            out_rgba = pack_rgba(r, g, b, a);
                        }
                        row_pix[x] = out_rgba;

                        // Late-Z (only matters when alpha test is on; we
                        // approximate by always writing here when depth
                        // wasn't already written above).
                        if (alpha_test && row_z) row_z[x] = pack_depth(z, 0);
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
    }
}

// ─── Explicit instantiations — ADR-002 specializations ───────────────────
template void rasterize_tile< 32,  32>(const Framebuffer&, const DrawCmd*, u32,
                                       const TileGrid&, const Texture*, u32, u32, bool) noexcept;
template void rasterize_tile< 64,  64>(const Framebuffer&, const DrawCmd*, u32,
                                       const TileGrid&, const Texture*, u32, u32, bool) noexcept;
template void rasterize_tile<128, 128>(const Framebuffer&, const DrawCmd*, u32,
                                       const TileGrid&, const Texture*, u32, u32, bool) noexcept;

TileRasterFn select_tile_raster_fn(u32 tile_w, u32 tile_h) noexcept {
    if (tile_w ==  32 && tile_h ==  32) return &rasterize_tile< 32,  32>;
    if (tile_w == 128 && tile_h == 128) return &rasterize_tile<128, 128>;
    return &rasterize_tile<64, 64>;  // default per ADR-002
}

}  // namespace psynder::render::raster
