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

    void started(app::WindowApp&) {
        scene_load
            .on_error([](std::string_view error) { PSY_LOG_ERROR("sample_02: {}", error); });
        scene_load.load_async("assets/crate_room.psyscene");
    }

    void frame(app::WindowFrameContext& ctx, app::WindowFrameCacheReady& cr) {
        ctx.app.update_scene_load(scene_load, cr.scene());

        (void)ctx.app.engine_frame_update(ctx.dt);
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(TexturedQuadSample)
