// SPDX-License-Identifier: MIT
// Psynder Player — generic cooked scene runtime shell.

#include "core/AppArgs.h"
#include "core/Log.h"
#include "platform/App.h"
#include "scene/SceneFile.h"

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

    scene::SceneLoadRequest scene_load{};

    static PlayerArgs parse_args(int argc, char** argv) { return parse_player_args(argc, argv); }

    void started(app::WindowApp&, const PlayerArgs& args) {
        scene_load
            .on_error([](std::string_view error) { PSY_LOG_ERROR("psynder_player: {}", error); });
        scene_load.load_async(args.scene_path);
    }

    void frame(app::WindowFrameContextT<PlayerArgs>& ctx, app::WindowFrameCacheReady& cr) {
        ctx.app.update_scene_load(scene_load, cr.scene());
        (void)ctx.app.engine_frame_update(ctx.dt);
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(PlayerApp)
