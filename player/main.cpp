// SPDX-License-Identifier: MIT
// Psynder Player — generic cooked scene runtime shell.

#include "core/AppArgs.h"
#include "core/Log.h"
#include "platform/App.h"
#include "render/GeometryTools.h"
#include "render/TextureGenerators.h"
#include "scene/SceneFile.h"

#include <array>
#include <string>
#include <string_view>

using namespace psynder;

namespace {

struct PlayerArgs : app::AppArgs {
    std::string scene_path = "assets/main.psyscene";
};

PlayerArgs parse_player_args(int argc, char** argv) {
    PlayerArgs args{};
    app::AppArgDiagnostics diagnostics{};
    constexpr std::string_view kSceneEq = "--scene=";
    constexpr std::string_view kSceneSp = "--scene";

    for (int i = 1; i < argc; ++i) {
        if (app::consume_common_arg(argc, argv, i, args, &diagnostics))
            continue;
        const std::string_view s{argv[i] ? argv[i] : ""};
        if (s.starts_with(kSceneEq)) {
            args.scene_path = std::string{s.substr(kSceneEq.size())};
        } else if (s == kSceneSp && i + 1 < argc) {
            args.scene_path = argv[++i];
        }
    }
    return args;
}

struct PlayerApp : app::BasicSceneApp {
    static constexpr const char* log_name = "psynder_player";
    static constexpr const char* display_name = "Psynder Player";
    static constexpr const char* asset_root = ".";

    render::Texture2D crate_texture{};
    render::MeshId unit_cube_crate{};
    render::MaterialId raster_material{};
    scene::SceneFileRequest scene_request{};
    scene::SceneFileLoaded scene_file{};
    bool instantiated = false;
    bool load_error_reported = false;

    static PlayerArgs parse_args(int argc, char** argv) { return parse_player_args(argc, argv); }

    void started(app::WindowApp& app, const PlayerArgs& args) {
        crate_texture = render::texture_generators::wooden_crate();

        render::MeshDesc cube_mesh_desc = render::geometry_tools::unit_cube();
        cube_mesh_desc.base_color = crate_texture.view();
        unit_cube_crate = app.rendering_system().cached_mesh(cube_mesh_desc);

        render::MaterialDesc material{};
        material.flags = render::MaterialFlags::RasterVisible;
        raster_material = scene().materials().create(material);

        scene_request.load_async(args.scene_path);
    }

    void frame(app::WindowFrameContextT<PlayerArgs>& ctx, app::WindowFrameCacheReady& cr) {
        instantiate_if_ready(ctx.app, cr.scene());
        (void)ctx.app.engine_frame_update(ctx.dt);
    }

    void instantiate_if_ready(app::WindowApp& app, scene::Scene& scene_ref) {
        if (instantiated)
            return;
        if (scene_request.failed()) {
            if (!load_error_reported) {
                PSY_LOG_ERROR("psynder_player: {}", scene_request.error());
                load_error_reported = true;
            }
            return;
        }

        scene::SceneFileLoaded loaded;
        if (!scene_request.consume(loaded))
            return;

        scene_file = std::move(loaded);
        scene_ref.prewarm_capacity(scene::scene_file_prewarm_config(scene_file.view));
        app.reserve_scene_capacity(static_cast<u32>(scene_file.view.mesh_instances.size()), 1u);

        const std::array bindings{
            scene::SceneMeshBinding{.mesh_name = "builtin.unit_cube.crate",
                                    .mesh = unit_cube_crate,
                                    .material = raster_material},
        };
        const scene::SceneFileInstantiateResult result =
            scene::instantiate_scene_file(scene_ref, scene_file.view, bindings);
        if (result.missing_mesh_bindings != 0u) {
            PSY_LOG_WARN("psynder_player: {} cooked mesh binding(s) were missing",
                         result.missing_mesh_bindings);
        }
        instantiated = true;
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(PlayerApp)
