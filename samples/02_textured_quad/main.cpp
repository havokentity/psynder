// SPDX-License-Identifier: MIT
// Psynder - Sample 02 / M2 demo. Rotating textured crate room.
//
// Loads a cooked scene; mesh, material, texture, camera, and environment data
// are resolved by the engine scene path.
//
#include "asset/Precook.h"
#include "platform/App.h"
#include "scene/SceneFile.h"

#include <string_view>

using namespace psynder;

PSYNDER_RUNTIME_BUNDLE("02_textured_quad");
PSYNDER_PRECOOK_PSYSCENE("assets/crate_room.psyscene.json");

namespace {

struct TexturedQuadSample : app::BasicSceneApp {
    static constexpr const char* log_name = "sample_02";
    static constexpr const char* display_name = "Psynder sample 02 (crate room)";
    static constexpr const char* asset_root = "samples/02_textured_quad";

    scene::SceneLoadRequest scene_load{};
    scene::SceneGroupId crates_group{};
    scene::CachedSceneGroup crates{};

    void started(app::WindowApp&) {
        crates_group = scene().group_id("crates");
        scene_load
            .on_ready([this](const scene::SceneLoadResult&) {
                crates = scene().cache_group(crates_group);
            })
            .on_error([](std::string_view error) { PSY_LOG_ERROR("sample_02: {}", error); });
        scene_load.load_async("assets/crate_room.psyscene");
    }

    void frame(app::WindowFrameContext& ctx, app::WindowFrameCacheReady& cr) {
        ctx.app.update_scene_load(scene_load, cr.scene());

        (void)ctx.app.engine_frame_update(ctx.dt);
        const f32 t = static_cast<f32>(ctx.seconds);

        usize i = 0;
        for (auto [entity, transform, authored] : crates.transforms()) {
            (void)entity;
            const math::Quat spin = math::quat_from_axis_angle(
                math::Vec3{0.0f, 1.0f, 0.0f}, t * (0.35f + 0.12f * static_cast<f32>(i)));
            transform.translation = authored.local.translation;
            transform.rotation = math::quat_mul(spin, authored.local.rotation);
            transform.scale = authored.local.scale;
            ++i;
        }
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(TexturedQuadSample)
