// SPDX-License-Identifier: MIT
// Psynder Arcade — generic cooked scene runtime shell.

#include "asset/Vault.h"
#include "audio/Audio.h"
#include "audio/Chiptune.h"
#include "core/AppArgs.h"
#include "core/console/Console.h"
#include "core/Log.h"
#include "editor/core/CommandHistory.h"
#include "editor/core/Editor.h"
#include "editor/core/Selection.h"
#include "editor/core/ViewportGizmo.h"
#include "editor/core/WebPanels.h"
#include "math/MathExt.h"
#include "platform/App.h"
#include "platform/RuntimeConfig.h"
#include "scene/SceneFile.h"
#include "ui/imm/Imm.h"
#include "ui/imm/detail/Font.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
Entity add_active_arcade_primitive(std::string_view kind, std::string_view material_preset = {});
Entity add_active_arcade_empty_entity();
Entity add_active_arcade_camera();
Entity add_active_arcade_light(scene::LightKind kind = scene::LightKind::Point);
bool rename_active_arcade_entity(Entity entity, std::string_view name);
bool delete_active_arcade_entity(Entity entity);
Entity duplicate_active_arcade_entity(Entity entity);
bool reparent_active_arcade_entity(Entity entity, Entity parent);
bool set_active_arcade_component_field(Entity entity,
                                       std::string_view component,
                                       std::string_view field,
                                       std::string_view value);
bool apply_active_arcade_material_preset(Entity entity, std::string_view preset);
bool undo_active_arcade_editor_command(std::string& label);
bool redo_active_arcade_editor_command(std::string& label);
bool save_active_arcade_scene(std::string_view path, std::string& error);
std::optional<Entity> parse_entity_arg(std::string_view text);
std::string joined_name_args(std::span<const std::string_view> args, usize first);
std::string_view material_preset_arg(std::span<const std::string_view> args);
std::optional<scene::LightKind> parse_light_kind_arg(std::string_view text);

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
    out.PrintLine("  primitive_add <kind>      Add box, sphere, plane, cone, pyramid, triangle");
    out.PrintLine("  light_add [kind]          Add point, spot, or directional light");
    out.PrintLine("  entity_reparent <e> <p>   Reparent entity; use 0/root for scene root");
    out.PrintLine("  scene_dirty               Print backend dirty state");
    out.PrintLine("  web_console               Open the web editor workbench");
    out.PrintLine("  arcade_boot_tune          Replay the 8-bit boot chime");
    out.PrintLine("  arcade_startup_tune 0|1   Disable/enable boot music");
    out.PrintLine("  config_open_dir           Open saved settings folder");
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
    if (auto* startup_tune =
            console_ref.RegisterCVar("arcade_startup_tune",
                                     "1",
                                     "Play Psynder Arcade boot music on startup: 0 | 1.",
                                     console::CVarFlags::Archive)) {
        startup_tune->allowed_values = {"0", "1"};
    }
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
        "entity_add_empty",
        "Add an empty transform entity to the active scene.",
        [](std::span<const std::string_view>, console::Output& out) {
            const Entity entity = add_active_arcade_empty_entity();
            if (!entity.valid()) {
                out.PrintLine("entity_add_empty: failed");
                return;
            }
            out.FormatLine("entity_add_empty: added {}", entity.raw);
        });
    console_ref.RegisterCommand(
        "camera_add",
        "Add a camera entity to the active scene.",
        [](std::span<const std::string_view>, console::Output& out) {
            const Entity entity = add_active_arcade_camera();
            if (!entity.valid()) {
                out.PrintLine("camera_add: failed");
                return;
            }
            out.FormatLine("camera_add: added {}", entity.raw);
        });
    console_ref.RegisterCommand(
        "light_add",
        "Add a visible placeholder light to the active scene: light_add [point|spot|directional].",
        [](std::span<const std::string_view> args, console::Output& out) {
            scene::LightKind kind = scene::LightKind::Point;
            if (!args.empty()) {
                const std::optional<scene::LightKind> parsed = parse_light_kind_arg(args[0]);
                if (!parsed) {
                    out.PrintLine("light_add: expected point, spot, or directional");
                    return;
                }
                kind = *parsed;
            }
            const Entity entity = add_active_arcade_light(kind);
            if (!entity.valid()) {
                out.PrintLine("light_add: failed");
                return;
            }
            out.FormatLine("light_add: added {}", entity.raw);
        });
    console_ref.RegisterCommand(
        "primitive_add",
        "Add a primitive to the active scene: box, sphere, plane, cone, pyramid, triangle.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (!g_active_arcade) {
                out.PrintLine("primitive_add: Psynder Arcade is not running");
                return;
            }
            if (args.empty()) {
                out.PrintLine("primitive_add: expected a kind");
                out.PrintLine("kinds: box, cube, sphere, geo_sphere, plane, cone, pyramid, triangle");
                return;
            }
            const Entity entity = add_active_arcade_primitive(args[0], material_preset_arg(args));
            if (!entity.valid()) {
                out.FormatLine("primitive_add: unknown kind `{}`", args[0]);
                out.PrintLine("kinds: box, cube, sphere, geo_sphere, plane, cone, pyramid, triangle");
                return;
            }
            out.FormatLine("primitive_add: added {} as entity {}", args[0], entity.raw);
        });
    console_ref.RegisterCommand(
        "material_apply",
        "Apply a material preset to a renderable: material_apply <entity_id> <default|clay|metal|glass|emissive>.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.size() < 2u) {
                out.PrintLine("material_apply: expected <entity_id> <preset>");
                return;
            }
            const std::optional<Entity> entity = parse_entity_arg(args[0]);
            if (!entity || !apply_active_arcade_material_preset(*entity, args[1])) {
                out.PrintLine("material_apply: failed");
                return;
            }
            out.FormatLine("material_apply: {} -> {}", entity->raw, args[1]);
        });
    console_ref.RegisterCommand(
        "entity_rename",
        "Rename a live scene entity in the editor hierarchy.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.size() < 2u) {
                out.PrintLine("entity_rename: expected <entity_id> <name>");
                return;
            }
            const std::optional<Entity> entity = parse_entity_arg(args[0]);
            const std::string name = joined_name_args(args, 1u);
            if (!entity || name.empty() || !rename_active_arcade_entity(*entity, name)) {
                out.PrintLine("entity_rename: failed");
                return;
            }
            out.FormatLine("entity_rename: {} -> {}", entity->raw, name);
        });
    console_ref.RegisterCommand(
        "entity_delete",
        "Delete a live scene entity.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.empty()) {
                out.PrintLine("entity_delete: expected <entity_id>");
                return;
            }
            const std::optional<Entity> entity = parse_entity_arg(args[0]);
            if (!entity || !delete_active_arcade_entity(*entity)) {
                out.PrintLine("entity_delete: failed");
                return;
            }
            out.FormatLine("entity_delete: deleted {}", entity->raw);
        });
    console_ref.RegisterCommand(
        "entity_duplicate",
        "Duplicate a live scene entity.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.empty()) {
                out.PrintLine("entity_duplicate: expected <entity_id>");
                return;
            }
            const std::optional<Entity> entity = parse_entity_arg(args[0]);
            if (!entity) {
                out.PrintLine("entity_duplicate: failed");
                return;
            }
            const Entity duplicate = duplicate_active_arcade_entity(*entity);
            if (!duplicate.valid()) {
                out.PrintLine("entity_duplicate: failed");
                return;
            }
            out.FormatLine("entity_duplicate: {} -> {}", entity->raw, duplicate.raw);
        });
    console_ref.RegisterCommand(
        "entity_reparent",
        "Reparent a live scene entity: entity_reparent <entity_id> <parent_id|0|root>.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.size() < 2u) {
                out.PrintLine("entity_reparent: expected <entity_id> <parent_id|0|root>");
                return;
            }
            const std::optional<Entity> entity = parse_entity_arg(args[0]);
            Entity parent{};
            if (args[1] != "0" && args[1] != "root" && args[1] != "none") {
                const std::optional<Entity> parsed_parent = parse_entity_arg(args[1]);
                if (!parsed_parent) {
                    out.PrintLine("entity_reparent: invalid parent");
                    return;
                }
                parent = *parsed_parent;
            }
            if (!entity || !reparent_active_arcade_entity(*entity, parent)) {
                out.PrintLine("entity_reparent: failed");
                return;
            }
            if (parent.valid())
                out.FormatLine("entity_reparent: {} -> parent {}", entity->raw, parent.raw);
            else
                out.FormatLine("entity_reparent: {} -> scene root", entity->raw);
        });
    console_ref.RegisterCommand(
        "component_set",
        "Set an editable native component field: component_set <entity> <component> <field> <value>.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.size() < 4u) {
                out.PrintLine("component_set: expected <entity> <component> <field> <value>");
                return;
            }
            const std::optional<Entity> entity =
                args[0] == "0" ? std::optional<Entity>{Entity{0u}} : parse_entity_arg(args[0]);
            const std::string value = joined_name_args(args, 3u);
            if (!entity ||
                !set_active_arcade_component_field(*entity, args[1], args[2], value)) {
                out.PrintLine("component_set: failed");
                return;
            }
            out.FormatLine("component_set: {}.{} updated on {}", args[1], args[2], entity->raw);
        });
    console_ref.RegisterCommand(
        "editor_undo",
        "Undo the latest editor-authored live scene edit.",
        [](std::span<const std::string_view>, console::Output& out) {
            std::string label;
            if (!undo_active_arcade_editor_command(label)) {
                out.PrintLine("editor_undo: nothing to undo");
                return;
            }
            out.FormatLine("editor_undo: {}", label);
        });
    console_ref.RegisterCommand(
        "editor_redo",
        "Redo the latest editor-authored live scene edit.",
        [](std::span<const std::string_view>, console::Output& out) {
            std::string label;
            if (!redo_active_arcade_editor_command(label)) {
                out.PrintLine("editor_redo: nothing to redo");
                return;
            }
            out.FormatLine("editor_redo: {}", label);
        });
    console_ref.RegisterCommand(
        "scene_save",
        "Save the active live scene to a cooked .psyscene file.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.empty()) {
                out.PrintLine("scene_save: expected <path>");
                return;
            }
            std::string error;
            if (!save_active_arcade_scene(args[0], error)) {
                out.FormatLine("scene_save: failed: {}", error.empty() ? "unknown error" : error);
                return;
            }
            out.FormatLine("scene_save: wrote {}", args[0]);
        });
    console_ref.RegisterCommand(
        "scene_dirty",
        "Print the backend scene dirty state.",
        [](std::span<const std::string_view>, console::Output& out) {
            const editor::WebSceneDirtyState dirty = editor::web_scene_dirty_state();
            out.FormatLine("scene_dirty: {} generation {}",
                           dirty.dirty ? "dirty" : "clean",
                           dirty.generation);
        });
    console_ref.RegisterCommand(
        "arcade_boot_tune",
        "Replay the Psynder Arcade 8-bit boot chime.",
        [](std::span<const std::string_view>, console::Output& out) {
            audio::play_chiptune(audio::boot_chime_song());
            out.PrintLine("arcade_boot_tune: playing boot chime");
        });
}

void log_arcade_startup_help(std::string_view status) {
    PSY_LOG_INFO("psynder_arcade: {}", status);
    PSY_LOG_INFO("psynder_arcade: press `~` for the console");
    PSY_LOG_INFO("psynder_arcade: arcade_load_scene <path> loads a cooked .psyscene");
    PSY_LOG_INFO("psynder_arcade: primitive_add box adds a primitive to the scene");
    PSY_LOG_INFO("psynder_arcade: web_console opens the web editor workbench");
    PSY_LOG_INFO("psynder_arcade: arcade_boot_tune replays the 8-bit boot chime");
    PSY_LOG_INFO("psynder_arcade: arcade_startup_tune 0 disables startup music");
    PSY_LOG_INFO("psynder_arcade: config_open_dir opens saved settings");
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

std::string primitive_display_name(std::string_view kind) {
    if (kind == "box" || kind == "cube")
        return "Box";
    if (kind == "sphere" || kind == "uv_sphere")
        return "Sphere";
    if (kind == "geo_sphere" || kind == "geosphere")
        return "Geo Sphere";
    if (kind == "plane" || kind == "floor")
        return "Plane";
    if (kind == "cone")
        return "Cone";
    if (kind == "pyramid")
        return "Pyramid";
    if (kind == "triangle")
        return "Triangle";
    return {};
}

std::optional<Entity> parse_entity_arg(std::string_view text) {
    u32 raw = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, raw);
    if (ec != std::errc{} || ptr != end || raw == 0u)
        return std::nullopt;
    return Entity{raw};
}

std::string joined_name_args(std::span<const std::string_view> args, usize first) {
    std::string out;
    for (usize i = first; i < args.size(); ++i) {
        if (!out.empty())
            out.push_back(' ');
        out.append(args[i].data(), args[i].size());
    }
    if (out.size() >= 2u && out.front() == '"' && out.back() == '"')
        out = out.substr(1u, out.size() - 2u);
    return out;
}

std::string_view material_preset_arg(std::span<const std::string_view> args) {
    constexpr std::string_view kPrefix = "--material=";
    for (std::string_view arg : args) {
        if (arg.starts_with(kPrefix))
            return arg.substr(kPrefix.size());
    }
    return {};
}

std::optional<scene::LightKind> parse_light_kind_arg(std::string_view text) {
    if (text == "point" || text == "Point")
        return scene::LightKind::Point;
    if (text == "directional" || text == "dir" || text == "sun" || text == "Directional")
        return scene::LightKind::Directional;
    if (text == "spot" || text == "spotlight" || text == "Spot")
        return scene::LightKind::Spot;
    return std::nullopt;
}

std::string_view light_kind_label(scene::LightKind kind) {
    switch (kind) {
        case scene::LightKind::Point:
            return "Point Light";
        case scene::LightKind::Directional:
            return "Directional Light";
        case scene::LightKind::Spot:
            return "Spot Light";
    }
    return "Light";
}

Entity parent_entity_for_node(scene::Scene& scene, scene::SceneNode node) {
    const scene::SceneNode parent = scene.graph().parent(node);
    if (!parent.valid())
        return {};
    auto& registry = scene.registry();
    std::vector<Entity> entities(registry.snapshot_live_entities({}));
    registry.snapshot_live_entities(entities);
    for (Entity entity : entities) {
        if (scene.node(entity).raw == parent.raw)
            return entity;
    }
    return {};
}

bool parse_f32_value(std::string_view text, f32& out) {
    std::string owned{text};
    char* end = nullptr;
    const float value = std::strtof(owned.c_str(), &end);
    if (end == owned.c_str() || (end && *end != '\0'))
        return false;
    out = value;
    return true;
}

bool parse_u32_value(std::string_view text, u32& out) {
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

bool parse_bool_value(std::string_view text, bool& out) {
    if (text == "1" || text == "true") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false") {
        out = false;
        return true;
    }
    return false;
}

bool parse_f32_array(std::string_view text, std::span<f32> out) {
    if (text.size() >= 2u && text.front() == '[' && text.back() == ']')
        text = text.substr(1u, text.size() - 2u);

    usize index = 0u;
    usize start = 0u;
    while (start <= text.size()) {
        const usize comma = text.find(',', start);
        const usize end = comma == std::string_view::npos ? text.size() : comma;
        if (index >= out.size())
            return false;
        std::string_view token = text.substr(start, end - start);
        while (!token.empty() && token.front() == ' ')
            token.remove_prefix(1u);
        while (!token.empty() && token.back() == ' ')
            token.remove_suffix(1u);
        if (!parse_f32_value(token, out[index++]))
            return false;
        if (comma == std::string_view::npos)
            break;
        start = comma + 1u;
    }
    return index == out.size();
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
    const std::array<std::string_view, 9> lines{
        status,
        "Press `~` for the console.",
        "arcade_load_scene <path>  Load a cooked .psyscene",
        "primitive_add box         Add primitives to the active scene",
        "web_console               Open the web editor workbench",
        "arcade_new_scene          Create a blank scene",
        "config_open_dir           Open saved settings folder",
        "arcade_startup_tune 0|1   Toggle boot music",
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
    u32 primitive_spawn_count = 0u;
    bool show_idle_panel = true;
    bool audio_started = false;
    bool replaying_history = false;
    editor::command_history::History edit_history{128u};
    std::vector<scene::SceneLightItem> gathered_lights{};
    struct GizmoDragEdit {
        Entity entity{};
        editor::viewport::GizmoMode mode = editor::viewport::GizmoMode::Translate;
        scene::LocalTransform before{};
        scene::LocalTransform after{};
        bool active = false;
    };
    GizmoDragEdit gizmo_drag{};

    static PlayerArgs parse_args(int argc, char** argv) { return parse_player_args(argc, argv); }

    static app::WindowAppOptions window_options(const PlayerArgs&) {
        return app::WindowAppOptions{.depth_buffer = true, .scene_notices = false};
    }

    void mark_authoring_dirty() { editor::mark_web_scene_dirty(); }

    void set_authoring_dirty(bool dirty) { editor::set_web_scene_dirty(dirty); }

    void load_scene(std::string_view path) {
        if (!app)
            return;
        reset_active_authoring_scene();
        load_target_scene = &app->create_scene();
        idle_status = "Loading ";
        idle_status.append(path.data(), path.size());
        show_idle_panel = true;
        scene_load.load_async(path);
    }

    void create_blank_scene() {
        if (!app)
            return;
        reset_active_authoring_scene();
        scene::Scene& scene = app->create_active_scene();
        scene.environment().set_clear_color(ui::imm::rgba(0x06, 0x08, 0x10));
        const Entity camera = scene.spawn_camera(scene::CameraDesc{
            .position = math::Vec3{0.0f, 1.4f, 4.5f},
            .look_at = math::Vec3{0.0f, 0.7f, 0.0f},
            .up = math::Vec3{0.0f, 1.0f, 0.0f},
        });
        editor::set_web_entity_label(camera, "Camera #1");
        load_target_scene = nullptr;
        idle_status = "Blank scene active.";
        show_idle_panel = false;
        PSY_LOG_INFO("psynder_arcade: created blank scene");
    }

    Entity add_primitive(std::string_view kind, std::string_view material_preset = {}) {
        if (!app)
            return {};
        if (!app->active_scene())
            create_blank_scene();
        scene::Scene* scene = app->active_scene();
        if (!scene)
            return {};

        render::BuiltInMesh mesh_kind = render::BuiltInMesh::UnitCube;
        scene::LocalTransform local{};
        local.translation = primitive_spawn_position(primitive_spawn_count);
        local.scale = math::Vec3{1.0f, 1.0f, 1.0f};

        const std::string primitive_name = primitive_display_name(kind);
        if (primitive_name.empty())
            return {};

        if (kind == "box" || kind == "cube") {
            mesh_kind = render::BuiltInMesh::UnitCube;
        } else if (kind == "sphere" || kind == "uv_sphere") {
            mesh_kind = render::BuiltInMesh::UvSphere;
        } else if (kind == "geo_sphere" || kind == "geosphere") {
            mesh_kind = render::BuiltInMesh::GeodesicSphere;
        } else if (kind == "plane" || kind == "floor") {
            mesh_kind = render::BuiltInMesh::UnitCube;
            local.scale = math::Vec3{3.0f, 0.035f, 3.0f};
        } else if (kind == "cone") {
            mesh_kind = render::BuiltInMesh::Cone;
        } else if (kind == "pyramid") {
            mesh_kind = render::BuiltInMesh::Pyramid;
        } else if (kind == "triangle") {
            mesh_kind = render::BuiltInMesh::TexturedTriangle;
        }

        render::MaterialDesc material = primitive_material_desc(material_preset,
                                                                primitive_spawn_count);

        const render::MeshId mesh = app->rendering_system().builtin_mesh(mesh_kind);
        local.translation.y = primitive_grounded_y(app->rendering_system(), mesh, local.scale);
        const render::MaterialId material_id = scene->materials().create(material);
        const render::SceneMeshEntity created =
            app->rendering_system().create_mesh_instance(*scene, mesh, material_id, local);
        if (!created.entity.valid())
            return {};

        ++primitive_spawn_count;
        std::string label = primitive_name;
        label += " Renderable #";
        label += std::to_string(primitive_spawn_count);
        editor::set_web_entity_label(created.entity, label);
        show_idle_panel = false;
        mark_authoring_dirty();
        editor::publish_web_scene_hierarchy(scene);
        return created.entity;
    }

    Entity add_empty_entity() {
        scene::Scene* scene = ensure_active_scene();
        if (!scene)
            return {};
        scene::LocalTransform local{};
        local.translation = primitive_spawn_position(primitive_spawn_count++);
        const Entity entity = scene->create_entity(local);
        if (entity.valid()) {
            editor::set_web_entity_label(entity, numbered_label("Empty Entity"));
            show_idle_panel = false;
            mark_authoring_dirty();
            editor::publish_web_scene_hierarchy(scene);
        }
        return entity;
    }

    Entity add_camera_entity() {
        scene::Scene* scene = ensure_active_scene();
        if (!scene)
            return {};
        const math::Vec3 p = primitive_spawn_position(primitive_spawn_count++);
        scene::CameraDesc desc{};
        desc.position = math::Vec3{p.x, 1.4f, p.z + 4.5f};
        desc.look_at = math::Vec3{p.x, 0.7f, p.z};
        desc.active = scene->active_camera_entity().valid() ? false : true;
        const Entity entity = scene->spawn_camera(desc);
        if (entity.valid()) {
            editor::set_web_entity_label(entity, numbered_label("Camera"));
            show_idle_panel = false;
            mark_authoring_dirty();
            editor::publish_web_scene_hierarchy(scene);
        }
        return entity;
    }

    Entity add_light_entity(scene::LightKind kind = scene::LightKind::Point) {
        scene::Scene* scene = ensure_active_scene();
        if (!scene)
            return {};

        render::MaterialDesc material{};
        material.albedo_rgba8 = kind == scene::LightKind::Directional
                                     ? ui::imm::rgba(0xFF, 0xD8, 0x7A)
                                     : (kind == scene::LightKind::Spot
                                            ? ui::imm::rgba(0x8F, 0xD6, 0xFF)
                                            : ui::imm::rgba(0xFF, 0xF3, 0xB0));
        material.emissive = kind == scene::LightKind::Directional ? 1.1f : 1.5f;
        material.roughness = 0.35f;
        scene::LocalTransform local{};
        local.translation = primitive_spawn_position(primitive_spawn_count++);
        local.translation.y = kind == scene::LightKind::Directional ? 1.8f : 1.25f;
        local.scale = kind == scene::LightKind::Directional ? math::Vec3{0.35f, 0.08f, 0.35f}
                                                            : math::Vec3{0.25f, 0.25f, 0.25f};
        const render::MeshId mesh =
            app->rendering_system().builtin_mesh(kind == scene::LightKind::Spot
                                                     ? render::BuiltInMesh::Cone
                                                     : render::BuiltInMesh::UvSphere);
        const render::MaterialId material_id = scene->materials().create(material);
        const render::SceneMeshEntity created =
            app->rendering_system().create_mesh_instance(*scene, mesh, material_id, local);
        if (created.entity.valid()) {
            scene::LightComponent light{};
            light.kind = kind;
            light.color_rgba8 = material.albedo_rgba8;
            light.intensity = kind == scene::LightKind::Directional ? 1.5f : 3.0f;
            light.range = kind == scene::LightKind::Directional ? 0.0f : 8.0f;
            light.inner_cone_deg = kind == scene::LightKind::Spot ? 18.0f : 20.0f;
            light.outer_cone_deg = kind == scene::LightKind::Spot ? 38.0f : 45.0f;
            light.casts_shadow = 0u;
            scene->attach_light(created.entity, light);
            editor::set_web_entity_label(created.entity, numbered_label(light_kind_label(kind)));
            show_idle_panel = false;
            mark_authoring_dirty();
            editor::publish_web_scene_hierarchy(scene);
        }
        return created.entity;
    }

    bool rename_entity(Entity entity, std::string_view name) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene || !scene->registry().alive(entity) || name.empty())
            return false;
        const std::string before = editor::web_entity_label(entity);
        const std::string after{name};
        editor::set_web_entity_label(entity, name);
        editor::publish_web_scene_hierarchy(scene);
        mark_authoring_dirty();
        if (!replaying_history && before != after) {
            edit_history.push_callback(
                "Rename entity",
                [this, entity, before]() {
                    replaying_history = true;
                    (void)rename_entity(entity, before);
                    replaying_history = false;
                },
                [this, entity, after]() {
                    replaying_history = true;
                    (void)rename_entity(entity, after);
                    replaying_history = false;
                });
        }
        return true;
    }

    bool delete_entity(Entity entity) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene || !scene->registry().alive(entity))
            return false;
        const bool deleted = scene->destroy_entity(entity);
        editor::set_web_entity_label(entity, {});
        editor::publish_web_scene_hierarchy(scene);
        if (deleted)
            mark_authoring_dirty();
        return deleted;
    }

    Entity duplicate_entity(Entity entity) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene || !scene->registry().alive(entity))
            return {};
        auto& registry = scene->registry();
        scene::LocalTransform local = scene->transform(entity);
        local.translation.x += 0.75f;
        local.translation.z -= 0.25f;

        if (const auto* renderable = registry.get<scene::RenderableComponent>(entity)) {
            scene::RenderableComponent copy = *renderable;
            if (copy.material.valid())
                copy.material = scene->materials().create(scene->materials().get(copy.material));
            const Entity duplicate = scene->create_renderable(copy, local);
            if (duplicate.valid()) {
                if (const auto* light = registry.get<scene::LightComponent>(entity))
                    scene->attach_light(duplicate, *light);
                editor::set_web_entity_label(duplicate, duplicate_label(entity, "Renderable"));
                editor::publish_web_scene_hierarchy(scene);
                mark_authoring_dirty();
            }
            return duplicate;
        }

        if (const auto* camera = registry.get<scene::CameraComponent>(entity)) {
            const Entity duplicate = scene->create_camera(*camera, local);
            if (duplicate.valid()) {
                editor::set_web_entity_label(duplicate, duplicate_label(entity, "Camera"));
                editor::publish_web_scene_hierarchy(scene);
                mark_authoring_dirty();
            }
            return duplicate;
        }

        const Entity duplicate = scene->create_entity(local);
        if (duplicate.valid()) {
            editor::set_web_entity_label(duplicate, duplicate_label(entity, "Entity"));
            editor::publish_web_scene_hierarchy(scene);
            mark_authoring_dirty();
        }
        return duplicate;
    }

    bool reparent_entity(Entity entity, Entity parent) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene || !scene->registry().alive(entity))
            return false;

        scene::SceneNode node = scene->node(entity);
        if (!node.valid())
            return false;

        scene::SceneNode parent_node{};
        if (parent.valid()) {
            if (!scene->registry().alive(parent) || parent.raw == entity.raw)
                return false;
            parent_node = scene->node(parent);
            if (!parent_node.valid())
                return false;
        }

        const Entity before_parent = parent_entity_for_node(*scene, node);
        if (before_parent.raw == parent.raw)
            return true;

        if (!scene->graph().set_parent(node, parent_node))
            return false;

        mark_authoring_dirty();
        editor::publish_web_scene_hierarchy(scene);
        if (!replaying_history) {
            edit_history.push_callback(
                "Reparent entity",
                [this, entity, before_parent]() {
                    replaying_history = true;
                    (void)reparent_entity(entity, before_parent);
                    replaying_history = false;
                },
                [this, entity, parent]() {
                    replaying_history = true;
                    (void)reparent_entity(entity, parent);
                    replaying_history = false;
                });
        }
        return true;
    }

    static std::string f32_text(f32 value) {
        return std::to_string(value);
    }

    static std::string bool_text(bool value) {
        return value ? "true" : "false";
    }

    static std::string vec3_text(const math::Vec3& value) {
        return "[" + f32_text(value.x) + "," + f32_text(value.y) + "," + f32_text(value.z) + "]";
    }

    static std::string quat_text(const math::Quat& value) {
        return "[" + f32_text(value.x) + "," + f32_text(value.y) + "," +
               f32_text(value.z) + "," + f32_text(value.w) + "]";
    }

    static math::Vec3 rotation_degrees(math::Quat q) {
        const math::Vec3 euler = math::quat_to_euler(math::quat_normalize(q));
        return math::Vec3{
            euler.x * math::kRadToDeg,
            euler.y * math::kRadToDeg,
            euler.z * math::kRadToDeg,
        };
    }

    static math::Quat rotation_degrees_to_quat(const std::array<f32, 3>& degrees) {
        return math::quat_normalize(math::quat_from_euler(degrees[0] * math::kDegToRad,
                                                         degrees[1] * math::kDegToRad,
                                                         degrees[2] * math::kDegToRad));
    }

    std::optional<std::string> component_field_value(Entity entity,
                                                     std::string_view component,
                                                     std::string_view field) const {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene)
            return std::nullopt;

        if (component == "EnvironmentComponent") {
            if (entity.raw != 0u)
                return std::nullopt;
            const scene::EnvironmentSettings& env = scene->environment().settings();
            if (field == "clear_color_rgba8")
                return std::to_string(env.clear_color_rgba8);
            if (field == "clear_color")
                return bool_text(env.clear_color);
            if (field == "clear_depth")
                return bool_text(env.clear_depth);
            if (field == "sky_zenith_rgba8")
                return std::to_string(env.sky.zenith_rgba8);
            if (field == "sky_horizon_rgba8")
                return std::to_string(env.sky.horizon_rgba8);
            if (field == "sky_intensity")
                return f32_text(env.sky.intensity);
            if (field == "cloud_enabled")
                return bool_text(env.clouds.enabled);
            if (field == "cloud_coverage")
                return f32_text(env.clouds.coverage);
            if (field == "cloud_density")
                return f32_text(env.clouds.density);
            if (field == "cloud_height")
                return f32_text(env.clouds.height);
            return std::nullopt;
        }

        if (!scene->registry().alive(entity))
            return std::nullopt;

        auto& registry = scene->registry();
        if (component == "TransformComponent") {
            const scene::LocalTransform local = scene->transform(entity);
            if (field == "translation")
                return vec3_text(local.translation);
            if (field == "rotation")
                return vec3_text(rotation_degrees(local.rotation));
            if (field == "rotation_quat")
                return quat_text(local.rotation);
            if (field == "scale")
                return vec3_text(local.scale);
            return std::nullopt;
        }

        if (component == "CameraComponent") {
            const auto* camera = registry.get<scene::CameraComponent>(entity);
            if (!camera)
                return std::nullopt;
            if (field == "fov_y_deg")
                return f32_text(camera->fov_y_rad * math::kRadToDeg);
            if (field == "aspect")
                return f32_text(camera->aspect);
            if (field == "near_z")
                return f32_text(camera->near_z);
            if (field == "far_z")
                return f32_text(camera->far_z);
            if (field == "tile_w")
                return std::to_string(camera->tile_w);
            if (field == "tile_h")
                return std::to_string(camera->tile_h);
            if (field == "active")
                return bool_text(camera->active != 0u);
            return std::nullopt;
        }

        if (component == "RenderableComponent") {
            const auto* renderable = registry.get<scene::RenderableComponent>(entity);
            if (!renderable)
                return std::nullopt;
            if (field == "visible")
                return bool_text(scene::renderable_is_visible(*renderable));
            if (field == "casts_shadow_override") {
                return bool_text(
                    (renderable->flags & scene::RenderableFlags::CastsShadowOverride) != 0u);
            }
            return std::nullopt;
        }

        if (component == "MaterialComponent") {
            const auto* renderable = registry.get<scene::RenderableComponent>(entity);
            if (!renderable || !scene->materials().valid(renderable->material))
                return std::nullopt;
            const render::MaterialDesc material = scene->materials().get(renderable->material);
            if (field == "albedo_rgba8")
                return std::to_string(material.albedo_rgba8);
            if (field == "reflectivity")
                return f32_text(material.reflectivity);
            if (field == "roughness")
                return f32_text(material.roughness);
            if (field == "emissive")
                return f32_text(material.emissive);
            if (field == "alpha_cutoff")
                return f32_text(material.alpha_cutoff);
            if (field == "blend")
                return std::to_string(static_cast<u32>(material.blend));
            if (field == "shadow_opacity")
                return f32_text(material.shadow_opacity);
            if (field == "shadow_softness")
                return f32_text(material.shadow_softness);
            return std::nullopt;
        }

        if (component == "LightComponent") {
            const auto* light = registry.get<scene::LightComponent>(entity);
            if (!light)
                return std::nullopt;
            if (field == "kind")
                return std::to_string(static_cast<u32>(light->kind));
            if (field == "color_rgba8")
                return std::to_string(light->color_rgba8);
            if (field == "intensity")
                return f32_text(light->intensity);
            if (field == "range")
                return f32_text(light->range);
            if (field == "inner_cone_deg")
                return f32_text(light->inner_cone_deg);
            if (field == "outer_cone_deg")
                return f32_text(light->outer_cone_deg);
            if (field == "casts_shadow")
                return bool_text(light->casts_shadow != 0u);
            return std::nullopt;
        }

        return std::nullopt;
    }

    bool apply_component_field(Entity entity,
                               std::string_view component,
                               std::string_view field,
                               std::string_view value) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (component == "EnvironmentComponent") {
            if (!scene || entity.raw != 0u)
                return false;
            scene::EnvironmentSettings settings = scene->environment().settings();
            f32 f = 0.0f;
            u32 u = 0u;
            bool b = false;
            if (field == "clear_color_rgba8") {
                if (!parse_u32_value(value, u))
                    return false;
                settings.clear_color_rgba8 = u;
            } else if (field == "clear_color") {
                if (!parse_bool_value(value, b))
                    return false;
                settings.clear_color = b;
            } else if (field == "clear_depth") {
                if (!parse_bool_value(value, b))
                    return false;
                settings.clear_depth = b;
            } else if (field == "sky_zenith_rgba8") {
                if (!parse_u32_value(value, u))
                    return false;
                settings.sky.zenith_rgba8 = u;
            } else if (field == "sky_horizon_rgba8") {
                if (!parse_u32_value(value, u))
                    return false;
                settings.sky.horizon_rgba8 = u;
            } else if (field == "sky_intensity") {
                if (!parse_f32_value(value, f))
                    return false;
                settings.sky.intensity = std::max(0.0f, f);
            } else if (field == "cloud_enabled") {
                if (!parse_bool_value(value, b))
                    return false;
                settings.clouds.enabled = b;
            } else if (field == "cloud_coverage") {
                if (!parse_f32_value(value, f))
                    return false;
                settings.clouds.coverage = std::clamp(f, 0.0f, 1.0f);
            } else if (field == "cloud_density") {
                if (!parse_f32_value(value, f))
                    return false;
                settings.clouds.density = std::max(0.0f, f);
            } else if (field == "cloud_height") {
                if (!parse_f32_value(value, f))
                    return false;
                settings.clouds.height = std::max(0.0f, f);
            } else {
                return false;
            }
            scene->environment().set_settings(settings);
            editor::publish_web_scene_hierarchy(scene);
            return true;
        }

        if (!scene || !scene->registry().alive(entity))
            return false;

        auto& registry = scene->registry();
        if (component == "TransformComponent") {
            scene::LocalTransform local = scene->transform(entity);
            if (field == "translation") {
                std::array<f32, 3> v{};
                if (!parse_f32_array(value, v))
                    return false;
                local.translation = math::Vec3{v[0], v[1], v[2]};
            } else if (field == "scale") {
                std::array<f32, 3> v{};
                if (!parse_f32_array(value, v))
                    return false;
                local.scale = math::Vec3{v[0], v[1], v[2]};
            } else if (field == "rotation") {
                std::array<f32, 3> v{};
                if (!parse_f32_array(value, v))
                    return false;
                local.rotation = rotation_degrees_to_quat(v);
            } else if (field == "rotation_quat") {
                std::array<f32, 4> v{};
                if (!parse_f32_array(value, v))
                    return false;
                local.rotation = math::quat_normalize(math::Quat{v[0], v[1], v[2], v[3]});
            } else {
                return false;
            }
            const bool ok = scene->set_transform(entity, local);
            editor::publish_web_scene_hierarchy(scene);
            return ok;
        }

        if (component == "CameraComponent") {
            const auto* camera = registry.get<scene::CameraComponent>(entity);
            if (!camera)
                return false;
            scene::CameraComponent updated = *camera;
            f32 f = 0.0f;
            u32 u = 0u;
            bool b = false;
            if (field == "fov_y_deg") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.fov_y_rad = f * math::kDegToRad;
            } else if (field == "aspect") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.aspect = f;
            } else if (field == "near_z") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.near_z = f;
            } else if (field == "far_z") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.far_z = f;
            } else if (field == "tile_w") {
                if (!parse_u32_value(value, u))
                    return false;
                updated.tile_w = u;
            } else if (field == "tile_h") {
                if (!parse_u32_value(value, u))
                    return false;
                updated.tile_h = u;
            } else if (field == "active") {
                if (!parse_bool_value(value, b))
                    return false;
                updated.active = b ? 1u : 0u;
            } else {
                return false;
            }
            registry.add<scene::CameraComponent>(entity, updated);
            if (updated.active)
                scene->set_active_camera(entity);
            editor::publish_web_scene_hierarchy(scene);
            return true;
        }

        if (component == "RenderableComponent") {
            const auto* renderable = registry.get<scene::RenderableComponent>(entity);
            if (!renderable)
                return false;
            scene::RenderableComponent updated = *renderable;
            bool b = false;
            if (field == "visible") {
                if (!parse_bool_value(value, b))
                    return false;
                u32 bits = scene::renderable_flags_bits(updated.flags);
                if (b)
                    bits |= scene::renderable_flags_bits(scene::RenderableFlags::Visible);
                else
                    bits &= ~scene::renderable_flags_bits(scene::RenderableFlags::Visible);
                updated.flags = static_cast<scene::RenderableFlags>(bits);
            } else if (field == "casts_shadow_override") {
                if (!parse_bool_value(value, b))
                    return false;
                u32 bits = scene::renderable_flags_bits(updated.flags);
                if (b) {
                    bits |= scene::renderable_flags_bits(
                        scene::RenderableFlags::CastsShadowOverride);
                } else {
                    bits &= ~scene::renderable_flags_bits(
                        scene::RenderableFlags::CastsShadowOverride);
                }
                updated.flags = static_cast<scene::RenderableFlags>(bits);
            } else {
                return false;
            }
            const bool ok = scene->update_renderable(entity, updated);
            editor::publish_web_scene_hierarchy(scene);
            return ok;
        }

        if (component == "MaterialComponent") {
            const auto* renderable = registry.get<scene::RenderableComponent>(entity);
            if (!renderable || !scene->materials().valid(renderable->material))
                return false;
            render::MaterialDesc material = scene->materials().get(renderable->material);
            f32 f = 0.0f;
            u32 u = 0u;
            if (field == "albedo_rgba8") {
                if (!parse_u32_value(value, u))
                    return false;
                material.albedo_rgba8 = u;
            } else if (field == "reflectivity") {
                if (!parse_f32_value(value, f))
                    return false;
                material.reflectivity = std::clamp(f, 0.0f, 1.0f);
            } else if (field == "roughness") {
                if (!parse_f32_value(value, f))
                    return false;
                material.roughness = std::clamp(f, 0.0f, 1.0f);
            } else if (field == "emissive") {
                if (!parse_f32_value(value, f))
                    return false;
                material.emissive = std::max(0.0f, f);
            } else if (field == "alpha_cutoff") {
                if (!parse_f32_value(value, f))
                    return false;
                material.alpha_cutoff = std::clamp(f, 0.0f, 1.0f);
            } else if (field == "blend") {
                if (!parse_u32_value(value, u) || u > 2u)
                    return false;
                material.blend = static_cast<render::MaterialBlendMode>(u);
            } else if (field == "shadow_opacity") {
                if (!parse_f32_value(value, f))
                    return false;
                material.shadow_opacity = std::clamp(f, 0.0f, 1.0f);
            } else if (field == "shadow_softness") {
                if (!parse_f32_value(value, f))
                    return false;
                material.shadow_softness = std::clamp(f, 0.0f, 1.0f);
            } else {
                return false;
            }
            const bool ok = scene->materials().update(renderable->material, material);
            editor::publish_web_scene_hierarchy(scene);
            return ok;
        }

        if (component == "LightComponent") {
            const auto* light = registry.get<scene::LightComponent>(entity);
            if (!light)
                return false;
            scene::LightComponent updated = *light;
            f32 f = 0.0f;
            u32 u = 0u;
            bool b = false;
            if (field == "kind") {
                if (!parse_u32_value(value, u) || u > 2u)
                    return false;
                updated.kind = static_cast<scene::LightKind>(u);
            } else if (field == "color_rgba8") {
                if (!parse_u32_value(value, u))
                    return false;
                updated.color_rgba8 = u;
            } else if (field == "intensity") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.intensity = std::max(0.0f, f);
            } else if (field == "range") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.range = std::max(0.0f, f);
            } else if (field == "inner_cone_deg") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.inner_cone_deg = std::clamp(f, 0.0f, 179.0f);
            } else if (field == "outer_cone_deg") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.outer_cone_deg = std::clamp(f, updated.inner_cone_deg, 179.0f);
            } else if (field == "casts_shadow") {
                if (!parse_bool_value(value, b))
                    return false;
                updated.casts_shadow = b ? 1u : 0u;
            } else {
                return false;
            }
            const bool ok = scene->update_light(entity, updated);
            editor::publish_web_scene_hierarchy(scene);
            return ok;
        }

        return false;
    }

    bool set_component_field(Entity entity,
                             std::string_view component,
                             std::string_view field,
                             std::string_view value) {
        const std::optional<std::string> before =
            component_field_value(entity, component, field);
        const bool ok = apply_component_field(entity, component, field, value);
        if (!ok)
            return false;

        mark_authoring_dirty();
        if (!replaying_history && before) {
            const std::optional<std::string> after =
                component_field_value(entity, component, field);
            if (after && *before != *after) {
                std::string label{component};
                label += ".";
                label += field;
                const std::string component_name{component};
                const std::string field_name{field};
                const std::string undo_value = *before;
                const std::string redo_value = *after;
                edit_history.push_callback(
                    label,
                    [this, entity, component_name, field_name, undo_value]() {
                        replaying_history = true;
                        (void)apply_component_field(entity, component_name, field_name, undo_value);
                        replaying_history = false;
                    },
                    [this, entity, component_name, field_name, redo_value]() {
                        replaying_history = true;
                        (void)apply_component_field(entity, component_name, field_name, redo_value);
                        replaying_history = false;
                    });
            }
        }
        return true;
    }

    bool apply_material_preset(Entity entity, std::string_view preset) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene || !scene->registry().alive(entity))
            return false;
        auto& registry = scene->registry();
        const auto* renderable = registry.get<scene::RenderableComponent>(entity);
        if (!renderable || !scene->materials().valid(renderable->material))
            return false;
        if (!(preset.empty() || preset == "default" || preset == "clay" || preset == "metal" ||
              preset == "glass" || preset == "emissive"))
            return false;

        const render::MaterialId material_id = renderable->material;
        const render::MaterialDesc before = scene->materials().get(material_id);
        const render::MaterialDesc after = primitive_material_desc(preset, entity.index());
        const bool ok = scene->materials().update(material_id, after);
        if (!ok)
            return false;

        editor::publish_web_scene_hierarchy(scene);
        mark_authoring_dirty();
        if (!replaying_history) {
            edit_history.push_callback(
                "Apply material preset",
                [this, material_id, before]() {
                    scene::Scene* active = app ? app->active_scene() : nullptr;
                    if (!active || !active->materials().valid(material_id))
                        return;
                    replaying_history = true;
                    active->materials().update(material_id, before);
                    editor::publish_web_scene_hierarchy(active);
                    replaying_history = false;
                },
                [this, material_id, after]() {
                    scene::Scene* active = app ? app->active_scene() : nullptr;
                    if (!active || !active->materials().valid(material_id))
                        return;
                    replaying_history = true;
                    active->materials().update(material_id, after);
                    editor::publish_web_scene_hierarchy(active);
                    replaying_history = false;
                });
        }
        return true;
    }

    bool save_scene(std::string_view path, std::string& error) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene) {
            error = "no active scene";
            return false;
        }

        scene::detail::AlignedVector<u8> bytes;
        scene::SceneFileSaveStats stats{};
        const scene::SceneFileSaveHooks hooks{
            .user = this,
            .mesh_name = &PlayerApp::save_mesh_name,
            .material_name = &PlayerApp::save_material_name,
            .material_base_color_texture_name = nullptr,
            .material_preset_name = nullptr,
            .mesh_instance_group_name = &PlayerApp::save_mesh_group_name,
        };
        if (!scene::save_scene_file(*scene, hooks, bytes, &stats, &error))
            return false;

        std::ofstream file(std::string{path}, std::ios::binary);
        if (!file) {
            error = "could not open output file";
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            error = "could not write output file";
            return false;
        }
        PSY_LOG_INFO("psynder_arcade: saved scene {} ({} cameras, {} meshes, {} materials)",
                     path,
                     stats.cameras,
                     stats.mesh_instances,
                     stats.materials);
        set_authoring_dirty(false);
        return true;
    }

    bool undo_editor_command(std::string& label) {
        editor::command_history::Command command;
        if (!edit_history.undo(command))
            return false;
        label = std::string{command.label()};
        mark_authoring_dirty();
        if (app)
            editor::publish_web_scene_hierarchy(app->active_scene());
        return true;
    }

    bool redo_editor_command(std::string& label) {
        editor::command_history::Command command;
        if (!edit_history.redo(command))
            return false;
        label = std::string{command.label()};
        mark_authoring_dirty();
        if (app)
            editor::publish_web_scene_hierarchy(app->active_scene());
        return true;
    }

    void start_boot_audio(const PlayerArgs& args) {
        if (args.smoke_frames > 0u)
            return;
        if (auto* tune = console::Console::Get().FindCVar("arcade_startup_tune");
            tune && !tune->GetBool()) {
            PSY_LOG_INFO("psynder_arcade: startup music disabled by arcade_startup_tune");
            return;
        }
        if (!audio::Engine::Get().start(audio::DeviceDesc{})) {
            PSY_LOG_WARN("psynder_arcade: audio backend unavailable; boot chime disabled");
            return;
        }
        audio_started = true;
        audio::play_chiptune(audio::boot_chime_song());
    }

    void started(app::WindowApp& app_ref, const PlayerArgs& args) {
        app = &app_ref;
        g_active_arcade = this;
        editor::ensure_web_panel_commands_registered();
        register_arcade_console_commands();
        platform::runtime_config::register_console_commands();
        platform::runtime_config::register_console_archive_autosave();
        (void)platform::runtime_config::load_console_archive();
        start_boot_audio(args);

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
        ctx.app.engine_frame_render();
        draw_scene_light_post(ctx);
        draw_viewport_gizmo_with_undo(ctx);
        ctx.app.engine_frame_post();
    }

    static bool project_to_screen(const math::Vec3& world,
                                  const math::Mat4& view_projection,
                                  math::Vec2 framebuffer_size,
                                  math::Vec2& out,
                                  f32& out_depth) noexcept {
        const math::Vec4 clip =
            math::mul(view_projection, math::Vec4{world.x, world.y, world.z, 1.0f});
        if (clip.w <= 0.0001f)
            return false;
        const f32 ndc_x = clip.x / clip.w;
        const f32 ndc_y = clip.y / clip.w;
        if (ndc_x < -1.25f || ndc_x > 1.25f || ndc_y < -1.25f || ndc_y > 1.25f)
            return false;
        out = {
            (ndc_x * 0.5f + 0.5f) * framebuffer_size.x,
            (1.0f - (ndc_y * 0.5f + 0.5f)) * framebuffer_size.y,
        };
        out_depth = clip.w;
        return true;
    }

    static f32 framebuffer_aspect(const render::Framebuffer& fb) noexcept {
        return fb.height == 0u ? 1.0f : static_cast<f32>(fb.width) / static_cast<f32>(fb.height);
    }

    void draw_scene_light_post(app::WindowFrameContextT<PlayerArgs>& ctx) {
        scene::Scene* scene = ctx.app.active_scene();
        if (!scene || show_idle_panel)
            return;

        scene::SceneCameraView camera{};
        if (!scene->active_camera_view(framebuffer_aspect(ctx.framebuffer), camera))
            return;

        gathered_lights.clear();
        scene->gather_lights(gathered_lights);
        if (gathered_lights.empty())
            return;

        const math::Mat4 view_projection = math::mul(camera.projection, camera.view);
        const math::Vec2 framebuffer_size{
            static_cast<f32>(ctx.framebuffer.width),
            static_cast<f32>(ctx.framebuffer.height),
        };
        for (const scene::SceneLightItem& light : gathered_lights) {
            math::Vec2 screen{};
            f32 depth = 1.0f;
            if (!project_to_screen(light.position, view_projection, framebuffer_size, screen, depth))
                continue;
            const u32 color = light.color_rgba8;
            const f32 range = std::max(0.25f, light.range);
            const f32 radius =
                std::clamp((range / std::max(0.5f, depth)) * 34.0f, 6.0f, 96.0f);
            const f32 alpha =
                std::clamp(0.045f * std::max(0.25f, light.intensity), 0.04f, 0.32f);
            draw_glow(ctx.framebuffer, screen, radius, color, alpha);
        }
    }

    void draw_viewport_gizmo_with_undo(app::WindowFrameContextT<PlayerArgs>& ctx) {
        if (editor::current_mode() != editor::Mode::Edit || editor::overlays_capturing()) {
            finish_gizmo_drag(false);
            return;
        }
        scene::Scene* scene = ctx.app.active_scene();
        const Entity selected = editor::selection::selected_scene_entity();
        if (!scene || !selected.valid() || !scene->registry().alive(selected)) {
            finish_gizmo_drag(false);
            return;
        }

        scene::SceneCameraView camera{};
        if (!scene->active_camera_view(framebuffer_aspect(ctx.framebuffer), camera)) {
            finish_gizmo_drag(false);
            return;
        }

        const platform::Input* input = platform::input();
        if (!input) {
            finish_gizmo_drag(false);
            return;
        }
        const platform::MouseState mouse =
            platform::mouse_to_framebuffer_space(input->mouse(),
                                                 ctx.framebuffer.width,
                                                 ctx.framebuffer.height);
        const math::Mat4 view_projection = math::mul(camera.projection, camera.view);
        ui::imm::begin_frame(ctx.framebuffer);
        const editor::viewport::GizmoResult result = editor::viewport::draw_apply_gizmo(
            editor::viewport::GizmoFrame{
                .scene = scene,
                .selected_entity = selected,
                .view_projection = view_projection,
                .mouse = mouse,
                .framebuffer_size = {static_cast<f32>(ctx.framebuffer.width),
                                     static_cast<f32>(ctx.framebuffer.height)},
                .mode = editor::viewport::GizmoMode::Translate,
                .apply_transform = true,
            });
        ui::imm::end_frame();

        if (result.transform.valid) {
            if (!gizmo_drag.active || gizmo_drag.entity != result.transform.entity) {
                gizmo_drag = GizmoDragEdit{
                    .entity = result.transform.entity,
                    .mode = result.transform.mode,
                    .before = result.transform.before,
                    .after = result.transform.after,
                    .active = true,
                };
            } else {
                gizmo_drag.after = result.transform.after;
            }
            mark_authoring_dirty();
        }

        if (!mouse.left)
            finish_gizmo_drag(true);
    }

    void finish_gizmo_drag(bool commit) {
        if (!gizmo_drag.active)
            return;
        const GizmoDragEdit drag = gizmo_drag;
        gizmo_drag.active = false;
        if (!commit || replaying_history)
            return;
        if (drag.before.translation.x == drag.after.translation.x &&
            drag.before.translation.y == drag.after.translation.y &&
            drag.before.translation.z == drag.after.translation.z &&
            drag.before.rotation.x == drag.after.rotation.x &&
            drag.before.rotation.y == drag.after.rotation.y &&
            drag.before.rotation.z == drag.after.rotation.z &&
            drag.before.rotation.w == drag.after.rotation.w &&
            drag.before.scale.x == drag.after.scale.x && drag.before.scale.y == drag.after.scale.y &&
            drag.before.scale.z == drag.after.scale.z) {
            return;
        }
        edit_history.push_callback(
            "Transform entity",
            [this, entity = drag.entity, before = drag.before]() {
                scene::Scene* active = app ? app->active_scene() : nullptr;
                if (!active || !active->registry().alive(entity))
                    return;
                replaying_history = true;
                active->set_transform(entity, before);
                editor::publish_web_scene_hierarchy(active);
                replaying_history = false;
            },
            [this, entity = drag.entity, after = drag.after]() {
                scene::Scene* active = app ? app->active_scene() : nullptr;
                if (!active || !active->registry().alive(entity))
                    return;
                replaying_history = true;
                active->set_transform(entity, after);
                editor::publish_web_scene_hierarchy(active);
                replaying_history = false;
            });
    }

    void stopped(app::WindowApp&) {
        if (audio_started) {
            audio::stop_chiptune();
            audio::Engine::Get().stop();
            audio_started = false;
        }
        if (g_active_arcade == this)
            g_active_arcade = nullptr;
    }

    static math::Vec3 primitive_spawn_position(u32 index) noexcept {
        const f32 x = static_cast<f32>(index % 5u) - 2.0f;
        const f32 z = -static_cast<f32>(index / 5u) * 1.25f;
        return math::Vec3{x, 0.0f, z};
    }

    static f32 primitive_grounded_y(render::RenderingSystem& renderer,
                                    render::MeshId mesh,
                                    const math::Vec3& scale) {
        const render::MeshDesc desc = renderer.meshes().get(mesh);
        return -desc.local_bounds.min.y * scale.y;
    }

    scene::Scene* ensure_active_scene() {
        if (!app)
            return nullptr;
        if (!app->active_scene())
            create_blank_scene();
        return app->active_scene();
    }

    std::string numbered_label(std::string_view prefix) const {
        std::string label{prefix};
        label += " #";
        label += std::to_string(primitive_spawn_count);
        return label;
    }

    static u32 primitive_material_color(u32 index) noexcept {
        constexpr std::array<u32, 8> kPalette{
            ui::imm::rgba(0xFF, 0x7A, 0x59),
            ui::imm::rgba(0x4F, 0xC3, 0xF7),
            ui::imm::rgba(0xB8, 0xF2, 0x7D),
            ui::imm::rgba(0xFF, 0xD1, 0x66),
            ui::imm::rgba(0xD3, 0x8B, 0xFF),
            ui::imm::rgba(0x6E, 0xE7, 0xB7),
            ui::imm::rgba(0xFF, 0x8F, 0xB3),
            ui::imm::rgba(0xA3, 0xBF, 0xFA),
        };
        return kPalette[index % kPalette.size()];
    }

    static render::MaterialDesc primitive_material_desc(std::string_view preset, u32 index) {
        render::MaterialDesc material{};
        material.albedo_rgba8 = primitive_material_color(index);
        material.reflectivity = 0.05f;
        material.roughness = 0.78f;
        material.raster_shadow_mode = render::MaterialRasterShadowMode::ProjectedDecal;

        if (preset == "clay") {
            material.albedo_rgba8 = ui::imm::rgba(0xB9, 0x68, 0x4A);
            material.roughness = 0.92f;
        } else if (preset == "metal") {
            material.albedo_rgba8 = ui::imm::rgba(0xA8, 0xB0, 0xB6);
            material.reflectivity = 0.7f;
            material.roughness = 0.35f;
        } else if (preset == "glass") {
            material.albedo_rgba8 = ui::imm::rgba(0xB7, 0xDD, 0xFF, 0x59);
            material.blend = render::MaterialBlendMode::AlphaBlend;
            material.reflectivity = 0.25f;
            material.roughness = 0.05f;
        } else if (preset == "emissive") {
            material.albedo_rgba8 = ui::imm::rgba(0x7F, 0xDC, 0xFF);
            material.emissive = 1.0f;
            material.roughness = 0.45f;
        }
        return material;
    }

    std::string duplicate_label(Entity original, std::string_view fallback_prefix) const {
        std::string label = editor::web_entity_label(original);
        if (label.empty()) {
            label.assign(fallback_prefix);
            label += " #";
            label += std::to_string(original.index());
        }
        label += " Copy";
        return label;
    }

    static std::string_view save_mesh_name(void* user, render::MeshId mesh) {
        auto* self = static_cast<PlayerApp*>(user);
        if (!self || !self->app)
            return {};
        render::RenderingSystem& renderer = self->app->rendering_system();
        if (mesh == renderer.builtin_mesh(render::BuiltInMesh::UnitCube))
            return "builtin.unit_cube";
        if (mesh == renderer.builtin_mesh(render::BuiltInMesh::TexturedTriangle))
            return "builtin.textured_triangle";
        if (mesh == renderer.builtin_mesh(render::BuiltInMesh::Pyramid))
            return "builtin.pyramid";
        if (mesh == renderer.builtin_mesh(render::BuiltInMesh::Cone))
            return "builtin.cone";
        if (mesh == renderer.builtin_mesh(render::BuiltInMesh::UvSphere))
            return "builtin.uv_sphere";
        if (mesh == renderer.builtin_mesh(render::BuiltInMesh::GeodesicSphere))
            return "builtin.geodesic_sphere";
        return {};
    }

    static std::string_view save_material_name(void*, render::MaterialId material) {
        static thread_local std::string name;
        name = "arcade.material.";
        name += std::to_string(material.raw);
        return name;
    }

    static std::string_view save_mesh_group_name(void*, Entity entity, scene::SceneNode) {
        static thread_local std::string name;
        name = editor::web_entity_label(entity);
        return name;
    }

    void reset_active_authoring_scene() {
        app->reset_scenes();
        scene::EcsRegistry::Get().clear();
        editor::clear_web_scene_authoring_state();
        load_target_scene = nullptr;
        primitive_spawn_count = 0u;
        set_authoring_dirty(false);
        edit_history.clear();
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

Entity add_active_arcade_primitive(std::string_view kind, std::string_view material_preset) {
    if (!g_active_arcade)
        return {};
    return g_active_arcade->add_primitive(kind, material_preset);
}

Entity add_active_arcade_empty_entity() {
    if (!g_active_arcade)
        return {};
    return g_active_arcade->add_empty_entity();
}

Entity add_active_arcade_camera() {
    if (!g_active_arcade)
        return {};
    return g_active_arcade->add_camera_entity();
}

Entity add_active_arcade_light(scene::LightKind kind) {
    if (!g_active_arcade)
        return {};
    return g_active_arcade->add_light_entity(kind);
}

bool rename_active_arcade_entity(Entity entity, std::string_view name) {
    return g_active_arcade && g_active_arcade->rename_entity(entity, name);
}

bool delete_active_arcade_entity(Entity entity) {
    return g_active_arcade && g_active_arcade->delete_entity(entity);
}

Entity duplicate_active_arcade_entity(Entity entity) {
    if (!g_active_arcade)
        return {};
    return g_active_arcade->duplicate_entity(entity);
}

bool reparent_active_arcade_entity(Entity entity, Entity parent) {
    return g_active_arcade && g_active_arcade->reparent_entity(entity, parent);
}

bool set_active_arcade_component_field(Entity entity,
                                       std::string_view component,
                                       std::string_view field,
                                       std::string_view value) {
    return g_active_arcade &&
           g_active_arcade->set_component_field(entity, component, field, value);
}

bool apply_active_arcade_material_preset(Entity entity, std::string_view preset) {
    return g_active_arcade && g_active_arcade->apply_material_preset(entity, preset);
}

bool undo_active_arcade_editor_command(std::string& label) {
    return g_active_arcade && g_active_arcade->undo_editor_command(label);
}

bool redo_active_arcade_editor_command(std::string& label) {
    return g_active_arcade && g_active_arcade->redo_editor_command(label);
}

bool save_active_arcade_scene(std::string_view path, std::string& error) {
    return g_active_arcade && g_active_arcade->save_scene(path, error);
}

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(PlayerApp)
