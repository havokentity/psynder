// SPDX-License-Identifier: MIT
// Psynder — Sample 02 / M2 demo. Rotating "crate room" — a small set of
// unit cubes spinning on the floor, viewed through a perspective camera.
//
// Sample 02 submits a small Scene through the hybrid rendering system.
// Raster remains the internal backend that performs tiled-binner + bilinear +
// Z. The crates now
// carry a real per-pixel procedural wooden-crate texture: each cube vertex
// is white and the whole RGBA8 chunk is bound on the DrawItem via
// `lightmap_texels`/`w`/`h`, so `end_frame` routes the draw onto the
// rasterizer's per-pixel bilinear surface_cached path (it samples the
// chunk through each face's 0..1 `uv` and multiplies by the white vertex
// colour ⇒ the texture dominates). The geometry still exercises every hot
// path in the rasterizer (vertex transform → triangle setup → tile bin →
// perspective-correct interpolation → Z reject) plus the textured fetch.
//
#include "platform/App.h"
#include "render/GeometryTools.h"
#include "render/Texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

using namespace psynder;

namespace {

constexpr u32 pack_rgba(u8 r, u8 g, u8 b, u8 a = 255) noexcept {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(a) << 24);
}

// ─── Procedural wooden-crate texture ─────────────────────────────────────
// Deterministic RGBA8 chunk (kCrateTexDim², pitch == width) that reads as a
// shipping crate: vertical wooden planks with darker grooves between them,
// a heavy dark frame around the border, two diagonal cross-battens, and a
// metal bolt in each corner. No RNG — a couple of cheap integer hashes give
// the planks per-column grain + light per-texel speckle so the wood doesn't
// look like flat bands. The sample owns the texture for the lifetime of
// every renderer submission that samples it.
constexpr u32 kCrateTexDim = 128;

// Small deterministic 2D value hash → [0,1). Cheap, repeatable, no global
// state; used only to add fine grain/speckle so the result isn't banded.
f32 hash2(u32 x, u32 y) noexcept {
    u32 h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0x1000000u);
}

u8 clamp_u8(i32 v) noexcept {
    return static_cast<u8>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

render::Texture2D build_crate_texture() {
    const u32 dim = kCrateTexDim;
    std::vector<u32> tex(static_cast<usize>(dim) * dim, 0u);

    // Palette (warm wood + dark iron frame). Plank base is a mid wood brown;
    // each plank gets a small per-column shade offset so adjacent boards read
    // as separate boards.
    constexpr i32 kWoodR = 150, kWoodG = 103, kWoodB = 56;      // base plank
    constexpr i32 kGrooveR = 64, kGrooveG = 40, kGrooveB = 20;  // gap shadow
    constexpr i32 kFrameR = 78, kFrameG = 50, kFrameB = 26;     // border/batten
    constexpr i32 kBoltR = 92, kBoltG = 96, kBoltB = 104;       // iron bolt

    const u32 frame = dim / 12;                 // border thickness (~10 px @128)
    const u32 plank_w = dim / 6;                // 6 vertical planks across the face
    const u32 groove = std::max(2u, dim / 64);  // dark gap between planks
    const f32 dimf = static_cast<f32>(dim);

    for (u32 y = 0; y < dim; ++y) {
        for (u32 x = 0; x < dim; ++x) {
            // Per-plank shade so neighbouring boards differ; deterministic
            // off the plank index. ±18 luma swing across the 6 planks.
            const u32 plank_idx = x / plank_w;
            const i32 plank_shade =
                static_cast<i32>((plank_idx * 37u) % 37u) - 18 + static_cast<i32>(plank_idx * 6u);
            // Long vertical grain: a slow cosine down the board + fine speckle.
            const f32 grain =
                std::cos((static_cast<f32>(x) + static_cast<f32>(plank_idx) * 11.0f) * 0.9f) * 10.0f;
            const f32 speckle = (hash2(x, y) - 0.5f) * 22.0f;
            i32 r = kWoodR + plank_shade + static_cast<i32>(grain + speckle);
            i32 g = kWoodG + plank_shade + static_cast<i32>(grain * 0.8f + speckle);
            i32 b = kWoodB + plank_shade + static_cast<i32>(grain * 0.5f + speckle);

            // Dark groove between planks (skip the leading edge at x==0).
            const u32 col_in_plank = x % plank_w;
            const bool in_groove = (col_in_plank < groove) && (plank_idx > 0);

            // Diagonal cross-battens (two bars forming an X across the
            // interior). Bar half-width scales with the face.
            const f32 fx = static_cast<f32>(x) / dimf;
            const f32 fy = static_cast<f32>(y) / dimf;
            const f32 bar = 0.06f;
            const bool on_batten = (std::fabs(fx - fy) < bar) || (std::fabs(fx - (1.0f - fy)) < bar);

            // Heavy border frame around the whole face.
            const bool in_frame =
                (x < frame) || (y < frame) || (x >= dim - frame) || (y >= dim - frame);

            // Corner bolts: small discs centred inside each corner of the
            // frame so the crate reads as bolted-together.
            const f32 bolt_in = static_cast<f32>(frame) * 0.5f;
            const f32 corners[4][2] = {
                {bolt_in, bolt_in},
                {dimf - bolt_in, bolt_in},
                {bolt_in, dimf - bolt_in},
                {dimf - bolt_in, dimf - bolt_in},
            };
            bool on_bolt = false;
            const f32 bolt_rad = static_cast<f32>(frame) * 0.30f;
            for (const auto& c : corners) {
                const f32 dx = static_cast<f32>(x) + 0.5f - c[0];
                const f32 dy = static_cast<f32>(y) + 0.5f - c[1];
                if (dx * dx + dy * dy <= bolt_rad * bolt_rad) {
                    on_bolt = true;
                    break;
                }
            }

            if (on_bolt) {
                // Tiny top-left shading so the bolt reads as a rounded head.
                const i32 sh = static_cast<i32>((hash2(x, y) - 0.5f) * 30.0f);
                r = kBoltR + sh;
                g = kBoltG + sh;
                b = kBoltB + sh;
            } else if (in_groove) {
                r = kGrooveR;
                g = kGrooveG;
                b = kGrooveB;
            } else if (in_frame || on_batten) {
                // Frame/batten share the dark iron-stained timber tone, with
                // the same fine speckle so they aren't dead flat.
                const i32 sp = static_cast<i32>((hash2(x, y) - 0.5f) * 16.0f);
                r = kFrameR + sp;
                g = kFrameG + sp;
                b = kFrameB + sp;
            }

            tex[static_cast<usize>(y) * dim + x] =
                pack_rgba(clamp_u8(r), clamp_u8(g), clamp_u8(b), 255);
        }
    }
    return render::Texture2D::from_rgba8(dim, dim, std::move(tex));
}

// Crate placements inside the room. Four cubes form a small clump in front
// of the camera; the camera looks down -Z so all four are visible.
const std::array<math::Vec3, 4> kCratePositions{{
    {-1.3f, 0.0f, -3.0f},
    {1.3f, 0.0f, -3.0f},
    {-0.6f, 0.0f, -5.0f},
    {0.7f, 1.2f, -5.5f},
}};

struct TexturedQuadSample {
    static constexpr const char* log_name = "sample_02";
    static constexpr const char* display_name = "Psynder sample 02 (crate room)";

    render::Texture2D crate_texture{};
    std::array<Entity, kCratePositions.size()> crates{};

    static app::WindowAppOptions window_options(const app::AppArgs&) noexcept {
        return {.depth_buffer = true};
    }

    void started(app::WindowApp& app) {
        crate_texture = build_crate_texture();

        auto& scene = app.loaded_scene();
        scene.prewarm_capacity(
            {.scene_entities = 8u, .renderables = 4u, .cameras = 1u, .render_items = 4u});
        app.reserve_scene_capacity(4u, 1u);
        scene.environment().set_clear_color(0xFF182030u);

        render::MeshDesc cube_mesh_desc = render::geometry_tools::unit_cube();
        cube_mesh_desc.base_color = crate_texture.view();
        const render::MeshId cube_mesh = app.rendering_system().cached_mesh(cube_mesh_desc);

        render::MaterialDesc crate_material{};
        crate_material.flags = render::MaterialFlags::RasterVisible;
        const render::MaterialId crate_material_id = scene.materials().create(crate_material);

        scene::LocalTransform camera_rig_transform{};
        camera_rig_transform.translation = math::Vec3{0.0f, 1.5f, 1.5f};
        const Entity camera_rig = scene.create_entity(camera_rig_transform);

        const render::Framebuffer& framebuffer = app.framebuffer();
        scene::CameraComponent camera_component{};
        camera_component.aspect =
            framebuffer.height == 0u ? 1.0f
                                     : static_cast<f32>(framebuffer.width) /
                                           static_cast<f32>(framebuffer.height);
        scene::LocalTransform camera_transform{};
        camera_transform.rotation =
            math::quat_from_axis_angle(math::Vec3{1.0f, 0.0f, 0.0f}, -std::atan2(1.5f, 4.5f));
        const Entity camera_entity =
            scene.create_camera(camera_component, camera_transform, scene.node(camera_rig));
        scene.set_active_camera(camera_entity);

        std::array<scene::LocalTransform, kCratePositions.size()> crate_transforms{};
        for (usize i = 0; i < kCratePositions.size(); ++i)
            crate_transforms[i].translation = kCratePositions[i];
        scene.spawn_mesh_batch(cube_mesh, crate_material_id, crate_transforms, crates);
    }

    void frame(app::WindowFrameContext& ctx, app::WindowFrameCacheReady& cr) {
        const auto edit_mode = ctx.app.engine_frame_update(ctx.dt);
        const f32 t = edit_mode == editor::Mode::Edit ? 0.0f : static_cast<f32>(ctx.seconds);

        for (usize i = 0; i < kCratePositions.size(); ++i) {
            scene::LocalTransform transform{};
            transform.translation = kCratePositions[i];
            transform.rotation = math::quat_from_axis_angle(
                math::Vec3{0, 1, 0}, t * (0.35f + 0.12f * static_cast<f32>(i)));
            cr.scene().set_transform(crates[i], transform);
        }
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(TexturedQuadSample)
