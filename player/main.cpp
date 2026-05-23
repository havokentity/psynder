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
#include "ui/imm/detail/Font.h"

#include <algorithm>
#include <array>
#include <cmath>
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
void create_active_arcade_scene();

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
        "arcade_new_scene",
        "Create a blank live scene in Psynder Arcade.",
        [](std::span<const std::string_view>, console::Output& out) {
            if (!g_active_arcade) {
                out.PrintLine("arcade_new_scene: Psynder Arcade is not running");
                return;
            }
            create_active_arcade_scene();
            out.PrintLine("arcade_new_scene: created blank scene with active camera");
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

u8 clamp_u8(f32 value) noexcept {
    return static_cast<u8>(std::clamp(value, 0.0f, 255.0f));
}

u32 with_alpha(u32 rgba, f32 alpha) noexcept {
    return (rgba & 0x00FFFFFFu) |
           (static_cast<u32>(clamp_u8(alpha * 255.0f)) << 24u);
}

u32 mix_rgb(u32 a, u32 b, f32 t, f32 alpha = 1.0f) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    const f32 ar = static_cast<f32>(a & 0xFFu);
    const f32 ag = static_cast<f32>((a >> 8u) & 0xFFu);
    const f32 ab = static_cast<f32>((a >> 16u) & 0xFFu);
    const f32 br = static_cast<f32>(b & 0xFFu);
    const f32 bg = static_cast<f32>((b >> 8u) & 0xFFu);
    const f32 bb = static_cast<f32>((b >> 16u) & 0xFFu);
    return ui::imm::rgba(clamp_u8(ar + (br - ar) * t),
                         clamp_u8(ag + (bg - ag) * t),
                         clamp_u8(ab + (bb - ab) * t),
                         clamp_u8(alpha * 255.0f));
}

f32 ping_pong(f32 value, f32 limit) noexcept {
    if (limit <= 0.0f)
        return 0.0f;
    const f32 period = limit * 2.0f;
    const f32 wrapped = std::fmod(value, period);
    return wrapped <= limit ? wrapped : period - wrapped;
}

void blend_pixel(render::Framebuffer& fb, i32 x, i32 y, u32 src) {
    if (x < 0 || y < 0 || x >= static_cast<i32>(fb.width) || y >= static_cast<i32>(fb.height) ||
        fb.pixels == nullptr) {
        return;
    }
    auto* pixels = reinterpret_cast<u32*>(fb.pixels);
    u32& dst = pixels[static_cast<usize>(y) * fb.width + static_cast<usize>(x)];
    const f32 a = static_cast<f32>((src >> 24u) & 0xFFu) / 255.0f;
    if (a >= 0.996f) {
        dst = src;
        return;
    }
    const f32 ia = 1.0f - a;
    dst = ui::imm::rgba(clamp_u8(static_cast<f32>(src & 0xFFu) * a +
                                 static_cast<f32>(dst & 0xFFu) * ia),
                        clamp_u8(static_cast<f32>((src >> 8u) & 0xFFu) * a +
                                 static_cast<f32>((dst >> 8u) & 0xFFu) * ia),
                        clamp_u8(static_cast<f32>((src >> 16u) & 0xFFu) * a +
                                 static_cast<f32>((dst >> 16u) & 0xFFu) * ia));
}

void draw_glow(render::Framebuffer& fb, math::Vec2 centre, f32 radius, u32 color, f32 alpha) {
    const i32 min_x = static_cast<i32>(std::floor(centre.x - radius));
    const i32 max_x = static_cast<i32>(std::ceil(centre.x + radius));
    const i32 min_y = static_cast<i32>(std::floor(centre.y - radius));
    const i32 max_y = static_cast<i32>(std::ceil(centre.y + radius));
    const f32 inv_r = radius > 0.0f ? 1.0f / radius : 1.0f;
    for (i32 y = min_y; y <= max_y; ++y) {
        for (i32 x = min_x; x <= max_x; ++x) {
            const f32 dx = (static_cast<f32>(x) + 0.5f - centre.x) * inv_r;
            const f32 dy = (static_cast<f32>(y) + 0.5f - centre.y) * inv_r;
            const f32 d2 = dx * dx + dy * dy;
            if (d2 > 1.0f)
                continue;
            const f32 falloff = (1.0f - d2) * (1.0f - d2);
            blend_pixel(fb, x, y, with_alpha(color, alpha * falloff));
        }
    }
}

void draw_scaled_text(math::Vec2 origin, std::string_view text, f32 scale, u32 color) {
    constexpr f32 kGlyphW = static_cast<f32>(ui::imm::detail::kGlyphWidth);
    constexpr f32 kGlyphH = static_cast<f32>(ui::imm::detail::kGlyphHeight);
    constexpr f32 kCellW = static_cast<f32>(ui::imm::detail::kCellWidth);
    f32 pen_x = origin.x;
    for (char ch : text) {
        const ui::imm::detail::Glyph& glyph = ui::imm::detail::glyph_for(ch);
        for (u32 row = 0; row < ui::imm::detail::kGlyphHeight; ++row) {
            const u8 bits = glyph.rows[row];
            for (u32 col = 0; col < ui::imm::detail::kGlyphWidth; ++col) {
                const u8 mask = static_cast<u8>(1u << (ui::imm::detail::kGlyphWidth - 1u - col));
                if ((bits & mask) == 0u)
                    continue;
                ui::imm::filled_rect(math::Vec2{pen_x + static_cast<f32>(col) * scale,
                                                origin.y + static_cast<f32>(row) * scale},
                                     math::Vec2{scale, scale},
                                     color);
            }
        }
        pen_x += kCellW * scale;
    }
    (void)kGlyphW;
    (void)kGlyphH;
}

void draw_attract_mode(render::Framebuffer& fb, f64 seconds, std::string_view status) {
    if (fb.width == 0u || fb.height == 0u || fb.pixels == nullptr)
        return;

    constexpr u32 kNearBlack = ui::imm::rgba(0x04, 0x06, 0x12);
    constexpr u32 kDeepBlue = ui::imm::rgba(0x09, 0x19, 0x2B);
    constexpr u32 kMagenta = ui::imm::rgba(0xF0, 0x4A, 0xC8);
    constexpr u32 kCyan = ui::imm::rgba(0x4D, 0xE9, 0xFF);
    constexpr u32 kAmber = ui::imm::rgba(0xFF, 0xC7, 0x57);
    constexpr u32 kGreen = ui::imm::rgba(0x72, 0xFF, 0xB8);

    auto* pixels = reinterpret_cast<u32*>(fb.pixels);
    for (u32 y = 0; y < fb.height; ++y) {
        const f32 v = static_cast<f32>(y) / static_cast<f32>(std::max(1u, fb.height - 1u));
        const u32 row = mix_rgb(kNearBlack, kDeepBlue, v * 0.75f);
        for (u32 x = 0; x < fb.width; ++x)
            pixels[static_cast<usize>(y) * fb.width + x] = row;
    }

    const f32 t = static_cast<f32>(seconds);
    ui::imm::begin_frame(fb);
    for (u32 i = 0; i < 42u; ++i) {
        const f32 fi = static_cast<f32>(i);
        const f32 x0 = std::fmod(fi * 73.0f + t * (12.0f + std::fmod(fi, 5.0f) * 5.0f),
                                 static_cast<f32>(fb.width) + 120.0f) -
                       60.0f;
        const f32 y = 24.0f + std::fmod(fi * 37.0f + std::sin(t * 0.7f + fi) * 18.0f,
                                        static_cast<f32>(std::max(1u, fb.height - 48u)));
        const f32 len = 20.0f + std::fmod(fi * 11.0f, 46.0f);
        const u32 color = (i % 3u == 0u) ? with_alpha(kCyan, 0.28f)
                          : (i % 3u == 1u) ? with_alpha(kMagenta, 0.22f)
                                           : with_alpha(kGreen, 0.18f);
        ui::imm::line(math::Vec2{x0, y}, math::Vec2{x0 + len, y + std::sin(t + fi) * 7.0f}, color);
    }

    for (u32 i = 0; i < 4u; ++i) {
        const f32 phase = t * (0.9f + 0.12f * static_cast<f32>(i)) + static_cast<f32>(i) * 1.7f;
        const f32 cx = static_cast<f32>(fb.width) * (0.5f + 0.38f * std::sin(phase));
        const f32 cy = static_cast<f32>(fb.height) * (0.5f + 0.30f * std::cos(phase * 1.31f));
        const u32 color = i == 0u   ? kCyan
                          : i == 1u ? kMagenta
                          : i == 2u ? kAmber
                                    : kGreen;
        draw_glow(fb, math::Vec2{cx, cy}, 36.0f + 10.0f * std::sin(phase * 1.9f), color, 0.22f);
    }

    constexpr std::string_view kLogo = "PsyArcade";
    constexpr f32 kLogoScale = 5.0f;
    constexpr f32 kSmallCellW = 6.0f;
    const f32 logo_w =
        static_cast<f32>(kLogo.size()) * static_cast<f32>(ui::imm::detail::kCellWidth) * kLogoScale;
    const f32 logo_h = static_cast<f32>(ui::imm::detail::kGlyphHeight) * kLogoScale;
    const f32 bounds_w = std::max(0.0f, static_cast<f32>(fb.width) - logo_w);
    const f32 bounds_h = std::max(0.0f, static_cast<f32>(fb.height) - logo_h);
    const f32 bx = ping_pong(t * 86.0f, bounds_w);
    const f32 by = ping_pong(t * 57.0f, bounds_h);
    const f32 pulse = 0.5f + 0.5f * std::sin(t * 5.0f);

    draw_glow(fb,
              math::Vec2{bx + logo_w * 0.5f, by + logo_h * 0.5f},
              54.0f + 12.0f * pulse,
              mix_rgb(kCyan, kMagenta, pulse),
              0.34f);
    draw_scaled_text(math::Vec2{bx + 5.0f, by + 5.0f}, kLogo, kLogoScale, kMagenta);
    draw_scaled_text(math::Vec2{bx - 4.0f, by - 2.0f}, kLogo, kLogoScale, kCyan);
    draw_scaled_text(math::Vec2{bx, by}, kLogo, kLogoScale, kAmber);

    constexpr f32 kCellW = kSmallCellW;
    constexpr f32 kLineH = 10.0f;
    constexpr f32 kPadX = 14.0f;
    constexpr f32 kPadY = 12.0f;
    const std::array<std::string_view, 7> lines{
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

    ui::imm::filled_rect(math::Vec2{x, y},
                         math::Vec2{panel_w, panel_h},
                         with_alpha(ui::imm::rgba(0x06, 0x0B, 0x14), 0.88f));
    ui::imm::rect_outline(math::Vec2{x, y},
                          math::Vec2{panel_w, panel_h},
                          mix_rgb(kCyan, kMagenta, pulse));
    f32 line_y = y + kPadY;
    for (usize i = 0u; i < lines.size(); ++i) {
        const u32 color = i == 0u ? ui::imm::rgba(0xD8, 0xE6, 0xF7)
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

    void create_blank_scene() {
        if (!app)
            return;
        scene::Scene& scene = app->create_active_scene();
        scene.environment().set_clear_color(ui::imm::rgba(0x06, 0x08, 0x10));
        (void)scene.spawn_camera(scene::CameraDesc{
            .position = math::Vec3{0.0f, 1.4f, 4.5f},
            .look_at = math::Vec3{0.0f, 0.7f, 0.0f},
            .up = math::Vec3{0.0f, 1.0f, 0.0f},
        });
        load_target_scene = nullptr;
        idle_status = "Blank scene active.";
        show_idle_panel = false;
        PSY_LOG_INFO("psynder_arcade: created blank scene");
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
        if (show_idle_panel)
            draw_attract_mode(ctx.framebuffer, ctx.seconds, idle_status);
        (void)cr;
    }

    void frame_post(app::WindowFrameContextT<PlayerArgs>& ctx, app::WindowFrameCacheReady&) {
        ctx.app.engine_frame_post();
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

void create_active_arcade_scene() {
    if (g_active_arcade)
        g_active_arcade->create_blank_scene();
}

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(PlayerApp)
