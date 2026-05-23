// SPDX-License-Identifier: MIT
// Psynder Arcade — generic cooked scene runtime shell.

#include "asset/Vault.h"
#include "core/AppArgs.h"
#include "core/console/Console.h"
#include "core/Log.h"
#include "editor/core/WebPanels.h"
#include "platform/App.h"
#include "scene/SceneFile.h"
#include "ui/imm/Imm.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

using namespace psynder;

namespace {

constexpr std::string_view kDefaultScenePath = "assets/main.psyscene";

struct PlayerArgs : app::AppArgs {
    std::string scene_path = std::string{kDefaultScenePath};
    bool scene_arg_provided = false;
};

struct PlayerApp;

PlayerApp* g_active_arcade = nullptr;

void load_active_arcade_scene(std::string_view path);

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
            args.scene_arg_provided = true;
        } else if (s == kSceneSp && i + 1 < argc) {
            args.scene_path = argv[++i];
            args.scene_arg_provided = true;
        }
    }
    return args;
}

void print_arcade_help(console::Output& out) {
    out.PrintLine("Psynder Arcade is waiting for a scene.");
    out.PrintLine("Commands:");
    out.PrintLine("  arcade_load_scene <path>  Load a cooked .psyscene");
    out.PrintLine("  arcade_open_editor        Open the web editor workbench");
    out.PrintLine("  editor_panel psygraph     Open visual scripting");
    out.PrintLine("  editor_panel assets       Open the asset browser");
    out.PrintLine("  editor_panel props        Open prop spawning");
}

bool virtual_asset_exists(std::string_view path) {
    return asset::Vault::Get().read(path).bytes != 0u;
}

void register_arcade_console_commands() {
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    auto& console_ref = console::Console::Get();
    console_ref.RegisterCommand("arcade_help",
                                "Print Psynder Arcade startup and scene-loading commands.",
                                [](std::span<const std::string_view>, console::Output& out) {
                                    print_arcade_help(out);
                                });
    console_ref.RegisterCommand(
        "arcade_load_scene",
        "Load a cooked .psyscene into Psynder Arcade.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.empty()) {
                out.PrintLine("arcade_load_scene: expected a cooked .psyscene path");
                out.PrintLine("example: arcade_load_scene assets/crate_room.psyscene");
                return;
            }
            if (!g_active_arcade) {
                out.PrintLine("arcade_load_scene: Psynder Arcade is not running");
                return;
            }
            load_active_arcade_scene(args[0]);
            out.FormatLine("arcade_load_scene: loading {}", args[0]);
        });
    console_ref.RegisterCommand(
        "arcade_open_editor",
        "Open the Psynder web editor workbench.",
        [](std::span<const std::string_view>, console::Output& out) {
            const console::ExecuteResult result = console::Console::Get().Execute("editor_console");
            if (!result.output.empty())
                out.Print(result.output);
            if (!result.error.empty())
                out.Print(result.error);
        });
}

void log_arcade_startup_help(std::string_view status) {
    PSY_LOG_INFO("psynder_arcade: {}", status);
    PSY_LOG_INFO("psynder_arcade: press `~` for the console");
    PSY_LOG_INFO("psynder_arcade: arcade_load_scene <path> loads a cooked .psyscene");
    PSY_LOG_INFO("psynder_arcade: arcade_open_editor opens the web editor workbench");
    PSY_LOG_INFO("psynder_arcade: editor_panel psygraph opens visual scripting");
}

void draw_idle_panel(render::Framebuffer& fb, std::string_view status) {
    if (fb.width == 0u || fb.height == 0u || fb.pixels == nullptr)
        return;

    constexpr f32 kCellW = 6.0f;
    constexpr f32 kLineH = 10.0f;
    constexpr f32 kPadX = 14.0f;
    constexpr f32 kPadY = 12.0f;
    const std::array<std::string_view, 8> lines{
        "Psynder Arcade",
        status,
        "Press `~` for the console.",
        "arcade_load_scene <path>  Load a cooked .psyscene",
        "arcade_open_editor        Open the web editor workbench",
        "editor_panel psygraph     Open visual scripting",
        "editor_panel assets       Open the asset browser",
        "arcade_help               Print this list",
    };

    usize max_chars = 0u;
    for (const std::string_view line : lines)
        max_chars = std::max(max_chars, line.size());

    const f32 panel_w = std::min(static_cast<f32>(fb.width) - 24.0f,
                                 static_cast<f32>(max_chars) * kCellW + kPadX * 2.0f);
    const f32 panel_h = static_cast<f32>(lines.size()) * kLineH + kPadY * 2.0f;
    const f32 x = (static_cast<f32>(fb.width) - panel_w) * 0.5f;
    const f32 y = (static_cast<f32>(fb.height) - panel_h) * 0.5f;

    ui::imm::begin_frame(fb);
    ui::imm::filled_rect(math::Vec2{x, y},
                         math::Vec2{panel_w, panel_h},
                         ui::imm::rgba(0x08, 0x0D, 0x13));
    ui::imm::rect_outline(math::Vec2{x, y},
                          math::Vec2{panel_w, panel_h},
                          ui::imm::rgba(0x5F, 0x85, 0xB8));
    f32 line_y = y + kPadY;
    for (usize i = 0u; i < lines.size(); ++i) {
        const u32 color = i == 0u   ? ui::imm::rgba(0xFF, 0xD2, 0x66)
                          : i == 1u ? ui::imm::rgba(0xD8, 0xE6, 0xF7)
                                    : ui::imm::rgba(0x9F, 0xB5, 0xCC);
        ui::imm::label(math::Vec2{x + kPadX, line_y}, lines[i], color);
        line_y += kLineH;
    }
    ui::imm::end_frame();
}

struct PlayerApp {
    static constexpr const char* log_name = "psynder_arcade";
    static constexpr const char* display_name = "Psynder Arcade";
    static constexpr const char* asset_root = ".";

    scene::SceneLoadRequest scene_load{};
    app::WindowApp* app = nullptr;
    scene::Scene* load_target_scene = nullptr;
    std::string idle_status = "No scene loaded.";
    bool show_idle_panel = true;

    static PlayerArgs parse_args(int argc, char** argv) { return parse_player_args(argc, argv); }

    static app::WindowAppOptions window_options(const PlayerArgs&) {
        return app::WindowAppOptions{.depth_buffer = true, .scene_notices = false};
    }

    void load_scene(std::string_view path) {
        if (!app)
            return;
        load_target_scene = &app->create_scene();
        idle_status = "Loading ";
        idle_status.append(path.data(), path.size());
        show_idle_panel = true;
        scene_load.load_async(path);
    }

    void started(app::WindowApp& app_ref, const PlayerArgs& args) {
        app = &app_ref;
        g_active_arcade = this;
        editor::ensure_web_panel_commands_registered();
        register_arcade_console_commands();

        scene_load
            .on_ready([this](const scene::SceneLoadResult& result) {
                if (app && load_target_scene)
                    app->set_scene(*load_target_scene);
                show_idle_panel = false;
                PSY_LOG_INFO("psynder_arcade: scene ready ({} mesh instances)",
                             result.instantiate.mesh_instances);
            })
            .on_error([this](std::string_view error) {
                idle_status = "Load failed: ";
                idle_status.append(error.data(), error.size());
                show_idle_panel = true;
                load_target_scene = nullptr;
                PSY_LOG_ERROR("psynder_arcade: {}", error);
            });

        if (!args.scene_arg_provided && !virtual_asset_exists(args.scene_path)) {
            idle_status = "No default scene at ";
            idle_status += args.scene_path;
            log_arcade_startup_help(idle_status);
            return;
        }

        load_scene(args.scene_path);
    }

    void frame(app::WindowFrameContextT<PlayerArgs>& ctx, app::WindowFrameCacheReady& cr) {
        if (scene_load.pending() && load_target_scene)
            ctx.app.update_scene_load(scene_load, *load_target_scene);
        (void)ctx.app.engine_frame_update(ctx.dt);
        (void)cr;
    }

    void frame_post(app::WindowFrameContextT<PlayerArgs>& ctx, app::WindowFrameCacheReady&) {
        ctx.app.engine_frame_post();
        if (show_idle_panel)
            draw_idle_panel(ctx.framebuffer, idle_status);
    }

    void stopped(app::WindowApp&) {
        if (g_active_arcade == this)
            g_active_arcade = nullptr;
    }
};

void load_active_arcade_scene(std::string_view path) {
    if (g_active_arcade)
        g_active_arcade->load_scene(path);
}

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(PlayerApp)
