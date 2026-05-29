// SPDX-License-Identifier: MIT
// Psynder — rasterizer public API + frame orchestration.
//
// Pipeline (DESIGN.md §7.1):
//   begin_frame    → reset frame arena, capture view state
//   submit(draw)   → record DrawItem (vertex pointers + model matrix)
//   end_frame      → vertex transform → triangle setup → bin → rasterize
//                    (the four hot stages of §7.1 steps 2–5)
//
// All per-frame allocations come from the frame arena (lane 01); no per-
// frame `new`/`delete`. No `virtual` in the hot loops. No `shared_ptr`.

#include "Raster.h"

#include "EdgeEq.h"
#include "SurfaceCache.h"
#include "TileBin.h"

#include "core/Types.h"
#include "core/alloc/Allocator.h"
#include "core/console/Console.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psynder::render::raster {

namespace {

// Frame-arena fallback: when the global mem::frame_scratch() is too small
// or shared with other lanes, the rasterizer carves a private slice from
// it on demand. Wave-A is single-threaded; lane 04 will plumb per-worker
// scratch when it lands the real job system.
struct FrameArena {
    u8* base = nullptr;
    usize head = 0;
    usize cap = 0;

    void reset(u8* b, usize c) noexcept {
        base = b;
        cap = c;
        head = 0;
    }
    void* alloc(usize bytes, usize align) noexcept {
        const usize cur = reinterpret_cast<usize>(base) + head;
        const usize aligned = (cur + align - 1) & ~(align - 1);
        const usize off = aligned - reinterpret_cast<usize>(base);
        if (off + bytes > cap)
            return nullptr;
        head = off + bytes;
        return base + off;
    }
    template <class T>
    T* alloc_array(usize n) noexcept {
        return static_cast<T*>(alloc(sizeof(T) * n, alignof(T)));
    }
};

// 8 MiB rasterizer-private arena. Big enough for ~32 k triangles plus
// per-tile pools. Lane 01 will replace this with a budgeted region.
// Sits in static .bss (zeroed at process load) rather than on any thread
// stack — the test runner's worker threads have small stacks.
struct FrameState {
    static constexpr usize kArenaBytes = 8 * 1024 * 1024;
    static constexpr u32 kMaxDraws = 4096;

    u8 buffer[kArenaBytes];
    FrameArena arena;
    ViewState view;
    bool in_frame = false;
    u32 draw_count = 0;
    DrawItem draw_items[kMaxDraws];
    DrawCmd draw_cmds[kMaxDraws];
    RasterLight preview_lights[1];
    RasterLightPacket light_packet;
};

// Single static instance — zero-init in .bss. No exceptions on construction.
PSY_FORCEINLINE FrameState& frame_state() noexcept {
    static FrameState s{};
    return s;
}

// cvars (lane 01 console singleton; registered lazily on first use).
struct Cvars {
    console::CVar* r_affine = nullptr;
    console::CVar* r_tile_size = nullptr;
    console::CVar* r_anisotropy = nullptr;
    console::CVar* r_raster_preview_light = nullptr;
    Cvars() {
        r_affine = console::Console::Get().RegisterCVar(
            "r_affine",
            "0",
            "1 = affine texture mapping (retro PS1 look); 0 = perspective-correct",
            console::CVarFlags::Archive);
        r_tile_size = console::Console::Get().RegisterCVar(
            "r_tile_size",
            "64",
            "Per-tile rasterizer tile dimension (32 / 64 / 128). See ADR-002.",
            console::CVarFlags::Archive);
        r_anisotropy = console::Console::Get().RegisterCVar(
            "r_anisotropy",
            "1",
            "Max anisotropic sample count for the EWA filter (1/2/4/8/16). "
            "1 = bilinear/trilinear only. DESIGN.md §7.5.",
            console::CVarFlags::Archive);
        r_raster_preview_light = console::Console::Get().RegisterCVar(
            "r_raster_preview_light",
            "0",
            "Debug only: enable lane-07 internal dynamic-light shader with one directional light. "
            "Scene lights need a render submission API before this can be driven by gameplay.",
            console::CVarFlags::Archive);
    }
};
PSY_FORCEINLINE Cvars& cvars() noexcept {
    static Cvars c;
    return c;
}

}  // namespace

// ─── Public API ──────────────────────────────────────────────────────────
Rasterizer& Rasterizer::Get() {
    static Rasterizer r;
    return r;
}

void Rasterizer::begin_frame(const ViewState& view) {
    auto& fs = frame_state();
    fs.arena.reset(fs.buffer, sizeof fs.buffer);
    fs.view = view;
    fs.draw_count = 0;
    fs.in_frame = true;
    (void)cvars();  // ensure cvar registration
    fs.light_packet = {};
    fs.light_packet.ambient_linear = {1.0f, 1.0f, 1.0f};
    if (cvars().r_raster_preview_light && cvars().r_raster_preview_light->GetBool()) {
        fs.preview_lights[0] = {};
        fs.preview_lights[0].kind = RasterLightKind::Directional;
        fs.preview_lights[0].direction_world = {-0.35f, -1.0f, -0.45f};
        fs.preview_lights[0].color_linear = {1.0f, 0.96f, 0.86f};
        fs.preview_lights[0].intensity = 1.25f;
        fs.light_packet.ambient_linear = {0.16f, 0.18f, 0.22f};
        fs.light_packet.lights = fs.preview_lights;
        fs.light_packet.light_count = 1;
    }
    // Bump the surface-cache frame index so hysteresis counts the right
    // number of "consecutive eligible" frames per surface.
    SurfaceCache::Get().begin_frame();
}

void Rasterizer::submit(const DrawItem& draw) {
    auto& fs = frame_state();
    if (!fs.in_frame || fs.draw_count >= FrameState::kMaxDraws)
        return;
    fs.draw_items[fs.draw_count++] = draw;
}

DrawItem make_projected_shadow_draw(const ProjectedShadowDesc& shadow) noexcept {
    DrawItem d{};
    d.vertices = shadow.vertices;
    d.vertex_count = shadow.vertex_count;
    d.indices = shadow.indices;
    d.index_count = shadow.index_count;
    d.model = shadow.model;
    d.cull = shadow.cull;
    d.blend = DrawBlendMode::Multiply;
    d.blend_opacity = std::clamp(shadow.opacity, 0.0f, 1.0f);
    d.lightmap_texels = shadow.mask_texels;
    d.lightmap_w = shadow.mask_w;
    d.lightmap_h = shadow.mask_h;
    return d;
}

void Rasterizer::end_frame() {
    auto& fs = frame_state();
    if (!fs.in_frame)
        return;
    fs.in_frame = false;

    if (fs.view.target.width == 0 || fs.view.target.height == 0)
        return;

    // ── Step 1: vertex transform + triangle setup ────────────────────────
    // For each DrawItem, transform vertices once into the frame arena
    // (Wave-A: scalar, single-threaded; lane 03's SIMD lanes plug into
    // the same loop once SoA buffers exist). Then run triangle setup
    // per index triple into a TriSetup array.
    const math::Mat4 view_proj = math::mul(fs.view.projection, fs.view.view);

    // Aniso cap is read once per frame from r_anisotropy; clamp to the
    // canonical set {1,2,4,8,16}. Out-of-range / negative inputs collapse
    // to 1 (DESIGN.md §7.5; tests cover the clamp).
    u8 frame_aniso_max = 1;
    if (auto* cv = cvars().r_anisotropy; cv) {
        const i32 a = cv->GetInt();
        if (a >= 16)
            frame_aniso_max = 16;
        else if (a >= 8)
            frame_aniso_max = 8;
        else if (a >= 4)
            frame_aniso_max = 4;
        else if (a >= 2)
            frame_aniso_max = 2;
        else
            frame_aniso_max = 1;
    }

    for (u32 di = 0; di < fs.draw_count; ++di) {
        const DrawItem& d = fs.draw_items[di];
        if (!d.vertices || d.vertex_count == 0 || !d.indices || d.index_count == 0) {
            fs.draw_cmds[di] = DrawCmd{};
            continue;
        }

        // Transform every vertex to clipspace once
        math::Vec4* cp = fs.arena.alloc_array<math::Vec4>(d.vertex_count);
        if (!cp) {
            fs.draw_cmds[di] = DrawCmd{};
            continue;
        }

        const math::Mat4 mvp = math::mul(view_proj, d.model);
        auto transform_point = [&](math::Vec3 p) noexcept {
            const math::Vec4 wp = math::mul(d.model, math::Vec4{p.x, p.y, p.z, 1.0f});
            return math::Vec3{wp.x, wp.y, wp.z};
        };
        auto transform_normal = [&](math::Vec3 n) noexcept {
            math::Vec3 out{
                d.model.m[0] * n.x + d.model.m[4] * n.y + d.model.m[8] * n.z,
                d.model.m[1] * n.x + d.model.m[5] * n.y + d.model.m[9] * n.z,
                d.model.m[2] * n.x + d.model.m[6] * n.y + d.model.m[10] * n.z,
            };
            const f32 len2 = math::dot(out, out);
            if (len2 <= 1.0e-12f)
                out = {0.0f, 1.0f, 0.0f};
            else
                out = math::mul(out, 1.0f / std::sqrt(len2));
            return out;
        };
        for (u32 i = 0; i < d.vertex_count; ++i) {
            const math::Vec3 p = d.vertices[i].position;
            cp[i] = math::mul(mvp, math::Vec4{p.x, p.y, p.z, 1.0f});
        }

        // Triangle setup per index triple, with FULL-FRUSTUM clipping.
        //
        // Each triangle is Sutherland-Hodgman-clipped against all SIX clip-space
        // frustum planes (inside: dot >= 0) before setup, interpolating uv +
        // colour at every cut, then fan-triangulated. Clipping the near plane is
        // mandatory — an interior (FPS-room) view surrounds the camera with
        // geometry, so big floor/wall/ceiling quads routinely have a vertex
        // behind the near plane and dropping the whole triangle left the room
        // black with holes at corners. Clipping the other five planes makes
        // SETUP robust: a vertex far outside the viewport (e.g. a wall that
        // extends well past the left/right/top/bottom edge, or geometry beyond
        // the far plane) would otherwise reach the Q24.8 fixed-point edge
        // functions with huge screen coordinates and risk overflow. We rely on
        // the clip rather than the screen-space bbox clamp for that safety.
        //
        // A triangle (3 verts) gains at most one vertex per clip plane, so the
        // 6-plane result is at most a 9-gon -> fan-triangulates to <= 7 tris.
        const u32 tri_count = d.index_count / 3;
        // Arena bound: each input triangle yields at most 7 output triangles
        // (6-plane clip -> <=9-gon -> 7-tri fan). We allocate the worst case
        // `tri_count * 7 + 1` directly. A straddler pre-pass (as the prior
        // near-plane-only path used) cannot cheaply predict the 6-plane fan
        // count, and under-allocating is unsafe: alloc_array returns nullptr on
        // overflow, which silently drops the entire draw. The +1 keeps a tiny
        // headroom and a non-zero request when tri_count == 0.
        constexpr usize kMaxTriSetups = FrameState::kArenaBytes / sizeof(TriSetup);
        if (static_cast<usize>(tri_count) > (kMaxTriSetups - 1u) / 7u) {
            fs.draw_cmds[di] = DrawCmd{};
            continue;
        }
        const usize tri_setup_count = static_cast<usize>(tri_count) * 7u + 1u;
        TriSetup* tris = fs.arena.alloc_array<TriSetup>(tri_setup_count);
        if (!tris) {
            fs.draw_cmds[di] = DrawCmd{};
            continue;
        }

        struct ClipVtx {
            math::Vec4 cp;
            math::Vec3 world;
            math::Vec3 normal;
            math::Vec2 uv;
            f32 r, g, b, a;  // unpacked colour, lerp-able across the cut
        };
        auto make_vtx = [&](u32 idx) noexcept {
            const u32 c = d.vertices[idx].color;
            ClipVtx v;
            v.cp = cp[idx];
            v.world = transform_point(d.vertices[idx].position);
            v.normal = transform_normal(d.vertices[idx].normal);
            v.uv = d.vertices[idx].uv;
            v.r = static_cast<f32>(c & 0xFFu);
            v.g = static_cast<f32>((c >> 8) & 0xFFu);
            v.b = static_cast<f32>((c >> 16) & 0xFFu);
            v.a = static_cast<f32>((c >> 24) & 0xFFu);
            return v;
        };
        auto lerp_vtx = [](const ClipVtx& p, const ClipVtx& q, f32 t) noexcept {
            ClipVtx v;
            v.cp = math::Vec4{p.cp.x + (q.cp.x - p.cp.x) * t,
                              p.cp.y + (q.cp.y - p.cp.y) * t,
                              p.cp.z + (q.cp.z - p.cp.z) * t,
                              p.cp.w + (q.cp.w - p.cp.w) * t};
            v.world = math::Vec3{p.world.x + (q.world.x - p.world.x) * t,
                                 p.world.y + (q.world.y - p.world.y) * t,
                                 p.world.z + (q.world.z - p.world.z) * t};
            v.normal = math::normalize(math::Vec3{p.normal.x + (q.normal.x - p.normal.x) * t,
                                                  p.normal.y + (q.normal.y - p.normal.y) * t,
                                                  p.normal.z + (q.normal.z - p.normal.z) * t});
            v.uv = math::Vec2{p.uv.x + (q.uv.x - p.uv.x) * t, p.uv.y + (q.uv.y - p.uv.y) * t};
            v.r = p.r + (q.r - p.r) * t;
            v.g = p.g + (q.g - p.g) * t;
            v.b = p.b + (q.b - p.b) * t;
            v.a = p.a + (q.a - p.a) * t;
            return v;
        };
        auto repack = [](const ClipVtx& v) noexcept -> u32 {
            auto cl = [](f32 x) noexcept {
                // Clamp then round-to-nearest (+0.5), matching TileRaster's
                // pack_rgba — plain truncation biased clipped-edge colours
                // darker and left visible seams along clip cuts.
                const f32 c = x < 0.0f ? 0.0f : (x > 255.0f ? 255.0f : x);
                return static_cast<u32>(c + 0.5f) & 0xFFu;
            };
            return cl(v.r) | (cl(v.g) << 8) | (cl(v.b) << 16) | (cl(v.a) << 24);
        };
        // Signed distance to clip-space frustum plane `plane` (0..5); inside is
        // >= 0. Order: 0 near (z+w), 1 far (w-z), 2 left (x+w), 3 right (w-x),
        // 4 bottom (y+w), 5 top (w-y).
        auto plane_dist = [](const ClipVtx& v, u32 plane) noexcept -> f32 {
            switch (plane) {
                case 0:
                    return v.cp.z + v.cp.w;  // near
                case 1:
                    return v.cp.w - v.cp.z;  // far
                case 2:
                    return v.cp.x + v.cp.w;  // left
                case 3:
                    return v.cp.w - v.cp.x;  // right
                case 4:
                    return v.cp.y + v.cp.w;  // bottom
                default:
                    return v.cp.w - v.cp.y;  // top
            }
        };
        // General single-plane Sutherland-Hodgman clip: keep inside vertices,
        // emit one interpolated vertex at each in<->out edge crossing. Writes up
        // to `n_in + 1` vertices to `out`; returns the count.
        auto clip_plane = [&](const ClipVtx* in, u32 n_in, ClipVtx* out, u32 plane) noexcept -> u32 {
            u32 n_out = 0;
            for (u32 e = 0; e < n_in; ++e) {
                const ClipVtx& a = in[e];
                const ClipVtx& b = in[(e + 1u == n_in) ? 0u : e + 1u];
                const f32 da = plane_dist(a, plane);
                const f32 db = plane_dist(b, plane);
                const bool ina = da >= 0.0f;
                const bool inb = db >= 0.0f;
                if (ina)
                    out[n_out++] = a;
                if (ina != inb) {
                    const f32 denom = da - db;
                    const f32 t = (denom != 0.0f) ? (da / denom) : 0.0f;
                    out[n_out++] = lerp_vtx(a, b, t);
                }
            }
            return n_out;
        };

        u32 produced = 0;
        for (u32 ti = 0; ti < tri_count; ++ti) {
            const u32 i0 = d.indices[ti * 3 + 0];
            const u32 i1 = d.indices[ti * 3 + 1];
            const u32 i2 = d.indices[ti * 3 + 2];
            if (i0 >= d.vertex_count || i1 >= d.vertex_count || i2 >= d.vertex_count)
                continue;

            // Clip the triangle against all six frustum planes in sequence,
            // ping-ponging between two fixed buffers. A 3-gon gains <=1 vertex
            // per plane, so the largest intermediate polygon is a 9-gon; size
            // 10 is the safe upper bound.
            ClipVtx buf_a[10];
            ClipVtx buf_b[10];
            buf_a[0] = make_vtx(i0);
            buf_a[1] = make_vtx(i1);
            buf_a[2] = make_vtx(i2);
            ClipVtx* poly = buf_a;
            ClipVtx* scratch = buf_b;
            u32 np = 3;
            for (u32 plane = 0; plane < 6 && np >= 3; ++plane) {
                np = clip_plane(poly, np, scratch, plane);
                ClipVtx* tmp = poly;
                poly = scratch;
                scratch = tmp;
            }
            if (np < 3)
                continue;  // fully outside the frustum

            // Fan-triangulate the clipped polygon (3..9 verts -> 1..7 tris).
            for (u32 k = 1; k + 1 < np; ++k) {
                const bool ok = setup_triangle_lit(poly[0].cp,
                                                   poly[k].cp,
                                                   poly[k + 1].cp,
                                                   poly[0].world,
                                                   poly[k].world,
                                                   poly[k + 1].world,
                                                   poly[0].normal,
                                                   poly[k].normal,
                                                   poly[k + 1].normal,
                                                   poly[0].uv,
                                                   poly[k].uv,
                                                   poly[k + 1].uv,
                                                   repack(poly[0]),
                                                   repack(poly[k]),
                                                   repack(poly[k + 1]),
                                                   fs.view.target.width,
                                                   fs.view.target.height,
                                                   tris[produced],
                                                   static_cast<u8>(d.cull));
                // Advance only on success so cmd.tri_count counts valid tris and
                // the binner never iterates culled / degenerate setups.
                if (ok)
                    ++produced;
            }
        }

        DrawCmd cmd{};
        cmd.tris = tris;
        cmd.tri_count = produced;
        cmd.material_id = d.material.raw;
        cmd.albedo_rgba8 = d.albedo_rgba8;
        cmd.flags = d.flags;
        cmd.aniso_max = frame_aniso_max;
        cmd.blend_mode = static_cast<u8>(d.blend);
        cmd.blend_opacity = static_cast<u8>(std::clamp(d.blend_opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
        cmd.material_lighting.diffuse = 1.0f;
        cmd.material_lighting.emissive = 0.0f;
        cmd.material_lighting.roughness = 1.0f;
        cmd.material_lighting.reflectivity = 0.0f;
        cmd.material_lighting.receives_dynamic_lights =
            fs.light_packet.light_count != 0 && (d.flags & DrawFlags::NoDynamicLights) == 0u;
        cmd.light_packet = fs.light_packet;
        if (!cmd.material_lighting.receives_dynamic_lights) {
            cmd.light_packet.lights = nullptr;
            cmd.light_packet.light_count = 0;
            cmd.light_packet.ambient_linear = {1.0f, 1.0f, 1.0f};
        }
        // ── Surface-cache classify (DESIGN.md §7.6 / ADR-001) ───────────
        SurfaceDesc sd{};
        sd.surface_id = d.material.raw;
        sd.lightmap_version = 0;  // lane 10 wires the real version in
                                  // Wave-B; until then keep 0 so the key
                                  // is stable across frames for the same
                                  // material.
        sd.mip_level = 0;         // single-mip materials for now; the mip
                                  // selector in the inner loop picks the
                                  // sample LOD for trilinear.
        sd.flags = d.flags;
        // Explicit per-draw lightmap chunk overrides the classifier + slab:
        // the caller hands us a ready RGBA8 chunk to sample through `uv`, so
        // force SurfaceCached and point the cmd straight at it. This is the
        // per-pixel lightmap path samples drive (e.g. sample_14) until lane 10
        // cooks the slab product; it cleanly bypasses the find() below.
        if (d.lightmap_texels != nullptr && d.lightmap_w > 0 && d.lightmap_h > 0) {
            cmd.shading_path = static_cast<u8>(ShadingPath::SurfaceCached);
            cmd.surface_cache_payload = d.lightmap_texels;
            cmd.surface_cache_width = d.lightmap_w;
            cmd.surface_cache_height = d.lightmap_h;
            fs.draw_cmds[di] = cmd;
            continue;
        }
        const ShadingPath path = classify_surface(sd);
        cmd.shading_path = static_cast<u8>(path);
        // The actual pre-multiplied payload arrives via the surface cache
        // slab when lane 18 fills it; for the Wave-B end-to-end we keep
        // the payload pointer nullable so the inner loop falls through to
        // OnTheFly if the slab hasn't been primed. The classifier still
        // tags the draw, so downstream tools / tests see SurfaceCached.
        if (path == ShadingPath::SurfaceCached) {
            const u32 slot =
                SurfaceCache::Get().find(sd.surface_id, sd.lightmap_version, sd.mip_level);
            if (slot != SurfaceCache::kInvalid) {
                const auto* e = SurfaceCache::Get().entry(slot);
                if (e && e->byte_size != 0) {
                    cmd.surface_cache_payload = reinterpret_cast<const u32*>(
                        SurfaceCache::Get().payload_base() + e->byte_offset);
                    cmd.surface_cache_width = e->width;
                    cmd.surface_cache_height = e->height;
                }
            }
        }
        fs.draw_cmds[di] = cmd;
    }

    // ── Step 2: build tile grid ──────────────────────────────────────────
    u32 tile_w = fs.view.tile_w;
    u32 tile_h = fs.view.tile_h;
    if (auto* cv = cvars().r_tile_size; cv) {
        const i32 t = cv->GetInt();
        if (t == 32 || t == 64 || t == 128) {
            tile_w = static_cast<u32>(t);
            tile_h = static_cast<u32>(t);
        }
    }
    TileGrid grid{};
    grid.tile_w = tile_w;
    grid.tile_h = tile_h;
    grid.cols = (fs.view.target.width + tile_w - 1) / tile_w;
    grid.rows = (fs.view.target.height + tile_h - 1) / tile_h;
    const u32 tile_count = grid.cols * grid.rows;
    grid.tile_offset = fs.arena.alloc_array<u32>(tile_count);
    grid.tile_count = fs.arena.alloc_array<u32>(tile_count);
    if (!grid.tile_offset || !grid.tile_count)
        return;
    std::memset(grid.tile_count, 0, sizeof(u32) * tile_count);

    // ── Step 3: pass 1 — count entries per tile ──────────────────────────
    for (u32 di = 0; di < fs.draw_count; ++di) {
        const DrawCmd& cmd = fs.draw_cmds[di];
        for (u32 ti = 0; ti < cmd.tri_count; ++ti) {
            const TriSetup& tri = cmd.tris[ti];
            if (!tri.valid)
                continue;
            const i32 tw = static_cast<i32>(tile_w);
            const i32 th = static_cast<i32>(tile_h);
            const i32 tx_min = std::max(0, tri.minx / tw);
            const i32 ty_min = std::max(0, tri.miny / th);
            const i32 tx_max = std::min(static_cast<i32>(grid.cols) - 1, (tri.maxx - 1) / tw);
            const i32 ty_max = std::min(static_cast<i32>(grid.rows) - 1, (tri.maxy - 1) / th);
            for (i32 ty = ty_min; ty <= ty_max; ++ty) {
                for (i32 tx = tx_min; tx <= tx_max; ++tx) {
                    grid.tile_count[static_cast<u32>(ty) * grid.cols + static_cast<u32>(tx)]++;
                }
            }
        }
    }

    // ── Step 4: prefix-sum offsets, allocate global entry pool ───────────
    u32 total_entries = 0;
    for (u32 i = 0; i < tile_count; ++i) {
        grid.tile_offset[i] = total_entries;
        total_entries += grid.tile_count[i];
        grid.tile_count[i] = 0;  // reset; reused as write cursor in pass 2
    }
    grid.entries_capacity = total_entries;
    grid.entries_used = 0;
    grid.entries = total_entries ? fs.arena.alloc_array<BinEntry>(total_entries) : nullptr;
    if (total_entries && !grid.entries)
        return;

    // ── Step 5: pass 2 — place entries into per-tile slots ───────────────
    for (u32 di = 0; di < fs.draw_count; ++di) {
        const DrawCmd& cmd = fs.draw_cmds[di];
        for (u32 ti = 0; ti < cmd.tri_count; ++ti) {
            const TriSetup& tri = cmd.tris[ti];
            if (!tri.valid)
                continue;
            const i32 tw = static_cast<i32>(tile_w);
            const i32 th = static_cast<i32>(tile_h);
            const i32 tx_min = std::max(0, tri.minx / tw);
            const i32 ty_min = std::max(0, tri.miny / th);
            const i32 tx_max = std::min(static_cast<i32>(grid.cols) - 1, (tri.maxx - 1) / tw);
            const i32 ty_max = std::min(static_cast<i32>(grid.rows) - 1, (tri.maxy - 1) / th);
            for (i32 ty = ty_min; ty <= ty_max; ++ty) {
                for (i32 tx = tx_min; tx <= tx_max; ++tx) {
                    const u32 t = static_cast<u32>(ty) * grid.cols + static_cast<u32>(tx);
                    const u32 slot = grid.tile_offset[t] + grid.tile_count[t]++;
                    grid.entries[slot] = BinEntry{di, ti};
                }
            }
        }
    }

    // ── Step 6: rasterize every tile ─────────────────────────────────────
    // Lane 04's job system will fan this out across workers; Wave-A runs
    // serially so the bench measurements are repeatable.
    const bool affine_mode = cvars().r_affine && cvars().r_affine->GetInt() != 0;
    TileRasterFn fn = select_tile_raster_fn(tile_w, tile_h);
    for (u32 ty = 0; ty < grid.rows; ++ty) {
        for (u32 tx = 0; tx < grid.cols; ++tx) {
            fn(fs.view.target,
               fs.draw_cmds,
               fs.draw_count,
               grid,
               /*tex*/ nullptr,
               tx,
               ty,
               affine_mode);
        }
    }
}

}  // namespace psynder::render::raster
