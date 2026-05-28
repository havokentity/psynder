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
#include "editor/ipc/Ipc.h"
#include "editor/play/PlayComponents.h"
#include "editor/play/PlayRuntime.h"
#include "editor/render/EditorRender.h"
#include "editor/world/LevelSource.h"
#include "math/MathExt.h"
#include "platform/App.h"
#include "platform/RuntimeConfig.h"
#include "render/Image.h"
#include "render/TextureGenerators.h"
#include "scene/SceneFile.h"
#include "ui/imm/Imm.h"
#include "ui/imm/Overlay.h"
#include "ui/imm/detail/Font.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
void create_active_arcade_fps_template();
Entity add_active_arcade_primitive(std::string_view kind, std::string_view material_preset = {});
Entity add_active_arcade_empty_entity();
Entity add_active_arcade_camera();
Entity add_active_arcade_light(scene::LightKind kind = scene::LightKind::Point);
Entity add_active_arcade_gameplay(std::string_view kind);
bool add_active_arcade_rigid_body(bool make_static);
bool set_active_arcade_material_color(u32 rgba8);
bool set_active_arcade_material_texture(std::string_view name);
bool add_active_arcade_vehicle();
bool add_active_arcade_helicopter();
bool add_active_arcade_rigid_body_to(Entity entity, bool make_static);
bool add_active_arcade_vehicle_to(Entity entity);
bool add_active_arcade_helicopter_to(Entity entity);
void set_active_arcade_rt_mode(bool on);
bool active_arcade_rt_mode();
bool bake_active_arcade_lightmaps();
bool world_new_active_arcade_indoor();
bool world_new_active_arcade_terrain();
bool rename_active_arcade_entity(Entity entity, std::string_view name);
bool delete_active_arcade_entity(Entity entity);
Entity duplicate_active_arcade_entity(Entity entity);
bool reparent_active_arcade_entity(Entity entity, Entity parent);
bool set_active_arcade_component_field(Entity entity,
                                       std::string_view component,
                                       std::string_view field,
                                       std::string_view value);
bool apply_active_arcade_material_preset(Entity entity, std::string_view preset);
bool apply_active_arcade_material_texture(Entity entity, std::string_view texture_name);
bool apply_active_arcade_material_texture_to_selection(std::string_view texture_name);
bool undo_active_arcade_editor_command(std::string& label);
bool redo_active_arcade_editor_command(std::string& label);
bool save_active_arcade_scene(std::string_view path, std::string& error);
void apply_active_arcade_component_edit(const editor::ipc::SelectionComponentEdit& edit);
void apply_active_arcade_component_add(const editor::ipc::SelectionComponentAdd& add);
std::optional<Entity> parse_entity_arg(std::string_view text);
std::string joined_name_args(std::span<const std::string_view> args, usize first);
std::string_view material_preset_arg(std::span<const std::string_view> args);
std::optional<scene::LightKind> parse_light_kind_arg(std::string_view text);
std::optional<scene::GameplayRole> parse_gameplay_role_arg(std::string_view text);

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
    out.PrintLine("  arcade_new_scene          Create a blank live scene");
    out.PrintLine("  arcade_fps_template       Create a playable FPS starter scene");
    out.PrintLine("  arcade_load_scene <path>  Load a cooked .psyscene");
    out.PrintLine("  primitive_add <kind>      Add box, sphere, plane, cone, pyramid, triangle");
    out.PrintLine("  light_add [kind]          Add point, spot, or directional light");
    out.PrintLine("  gameplay_add <kind>       Add player_start, fps_player, enemy, pickup, trigger, door");
    out.PrintLine("  entity_reparent <e> <p>   Reparent entity; use 0/root for scene root");
    out.PrintLine("  scene_dirty               Print backend dirty state");
    out.PrintLine("  arcade_play_mode          Switch to playable runtime mode");
    out.PrintLine("  arcade_edit_mode          Switch to scene editing mode");
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
        "arcade_play_mode",
        "Switch Psynder Arcade to playable runtime mode.",
        [](std::span<const std::string_view>, console::Output& out) {
            if (editor::current_mode() != editor::Mode::Play)
                editor::toggle_mode();
            platform::request_window_focus();
            out.PrintLine("arcade_play_mode: PLAY");
        });
    console_ref.RegisterCommand(
        "arcade_focus_window",
        "Bring the Psynder Arcade engine window to the front.",
        [](std::span<const std::string_view>, console::Output& out) {
            platform::request_window_focus();
            out.PrintLine("arcade_focus_window: requested");
        });
    console_ref.RegisterCommand(
        "arcade_edit_mode",
        "Switch Psynder Arcade to scene editing mode.",
        [](std::span<const std::string_view>, console::Output& out) {
            if (editor::current_mode() != editor::Mode::Edit)
                editor::toggle_mode();
            out.PrintLine("arcade_edit_mode: EDIT");
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
        "arcade_fps_template",
        "Create a playable FPS starter scene with player, props, lights, and gameplay markers.",
        [](std::span<const std::string_view>, console::Output& out) {
            if (!g_active_arcade) {
                out.PrintLine("arcade_fps_template: Psynder Arcade is not running");
                return;
            }
            create_active_arcade_fps_template();
            out.PrintLine("arcade_fps_template: created FPS starter scene");
        });
    console_ref.RegisterCommand(
        "template_create",
        "Create a named scene template: template_create fps_starter.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.empty() || !(args[0] == "fps_starter" || args[0] == "fps")) {
                out.PrintLine("template_create: expected fps_starter");
                return;
            }
            if (!g_active_arcade) {
                out.PrintLine("template_create: Psynder Arcade is not running");
                return;
            }
            create_active_arcade_fps_template();
            out.PrintLine("template_create: created fps_starter");
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
        "gameplay_add",
        "Add an FPS gameplay authoring object: gameplay_add <player_start|fps_player|enemy|pickup|trigger|door>.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.empty()) {
                out.PrintLine("gameplay_add: expected a kind");
                out.PrintLine("kinds: player_start, fps_player, enemy, pickup, trigger, door");
                return;
            }
            const Entity entity = add_active_arcade_gameplay(args[0]);
            if (!entity.valid()) {
                out.PrintLine("gameplay_add: failed");
                out.PrintLine("kinds: player_start, fps_player, enemy, pickup, trigger, door");
                return;
            }
            out.FormatLine("gameplay_add: added {} as entity {}", args[0], entity.raw);
        });
    console_ref.RegisterCommand(
        "phys_rigidbody",
        "Tag the selected object as a rigid body for Play mode: phys_rigidbody [static].",
        [](std::span<const std::string_view> args, console::Output& out) {
            const bool make_static = !args.empty() && args[0] == "static";
            if (add_active_arcade_rigid_body(make_static))
                out.FormatLine("phys_rigidbody: selected object is now a {} rigid body",
                               make_static ? "static" : "dynamic");
            else
                out.PrintLine("phys_rigidbody: no valid selection / no active scene");
        });
    console_ref.RegisterCommand(
        "mat_color",
        "Set the selected object's material albedo: mat_color <r> <g> <b> [a] (0-255).",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.size() < 3u) {
                out.PrintLine("mat_color: expected <r> <g> <b> [a] (0-255)");
                return;
            }
            auto parse = [](std::string_view s) -> u32 {
                u32 v = 0u;
                std::from_chars(s.data(), s.data() + s.size(), v);
                return std::min(v, 255u);
            };
            const u32 r = parse(args[0]);
            const u32 g = parse(args[1]);
            const u32 b = parse(args[2]);
            const u32 a = args.size() >= 4u ? parse(args[3]) : 255u;
            const u32 rgba8 = r | (g << 8) | (b << 16) | (a << 24);
            if (set_active_arcade_material_color(rgba8))
                out.FormatLine("mat_color: albedo r={} g={} b={} a={}", r, g, b, a);
            else
                out.PrintLine("mat_color: no valid selection / no material");
        });
    console_ref.RegisterCommand(
        "mat_tex",
        "Set the selected object's base-color texture: mat_tex <name|none>. Procedural names: "
        "textures.procedural.{checker,grid,bricks,wooden_crate,wood_planks,building_facade}.",
        [](std::span<const std::string_view> args, console::Output& out) {
            const std::string_view name = args.empty() ? std::string_view{"none"} : args[0];
            if (set_active_arcade_material_texture(name))
                out.FormatLine("mat_tex: texture = {}", name);
            else
                out.PrintLine("mat_tex: no valid selection / no material");
        });
    console_ref.RegisterCommand(
        "phys_vehicle",
        "Tag the selected object as a drivable vehicle for Play mode: phys_vehicle.",
        [](std::span<const std::string_view>, console::Output& out) {
            if (add_active_arcade_vehicle())
                out.PrintLine("phys_vehicle: selected object is now a drivable vehicle");
            else
                out.PrintLine("phys_vehicle: no valid selection / no active scene");
        });
    console_ref.RegisterCommand(
        "phys_helicopter",
        "Tag the selected object as a flyable helicopter for Play mode: phys_helicopter.",
        [](std::span<const std::string_view>, console::Output& out) {
            if (add_active_arcade_helicopter())
                out.PrintLine("phys_helicopter: selected object is now a flyable helicopter");
            else
                out.PrintLine("phys_helicopter: no valid selection / no active scene");
        });
    console_ref.RegisterCommand(
        "rt_mode",
        "Toggle the viewport raytracer: rt_mode <0|1> (no arg = enable).",
        [](std::span<const std::string_view> args, console::Output& out) {
            const bool on = args.empty() || !(args[0] == "0" || args[0] == "off");
            set_active_arcade_rt_mode(on);
            out.FormatLine("rt_mode: viewport renderer = {}", on ? "raytracer" : "raster");
        });
    console_ref.RegisterCommand(
        "bake_lightmaps",
        "Bake static lightmaps for the active scene (offline lm_bake).",
        [](std::span<const std::string_view>, console::Output& out) {
            if (bake_active_arcade_lightmaps())
                out.PrintLine("bake_lightmaps: baked (stored for baked/flat toggle)");
            else
                out.PrintLine("bake_lightmaps: nothing bakeable / no active scene");
        });
    console_ref.RegisterCommand(
        "world_new_indoor",
        "Spawn the demo indoor BSP level (two rooms + doorway) into the active scene.",
        [](std::span<const std::string_view>, console::Output& out) {
            if (world_new_active_arcade_indoor())
                out.PrintLine("world_new_indoor: indoor map added");
            else
                out.PrintLine("world_new_indoor: failed / no active scene");
        });
    console_ref.RegisterCommand(
        "world_new_terrain",
        "Spawn the demo heightfield terrain mesh into the active scene.",
        [](std::span<const std::string_view>, console::Output& out) {
            if (world_new_active_arcade_terrain())
                out.PrintLine("world_new_terrain: terrain added");
            else
                out.PrintLine("world_new_terrain: failed / no active scene");
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
        "material_texture_apply",
        "Apply a base-color texture to a renderable: material_texture_apply <entity_id> <texture_path|none>.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.size() < 2u) {
                out.PrintLine("material_texture_apply: expected <entity_id> <texture_path|none>");
                return;
            }
            const std::optional<Entity> entity = parse_entity_arg(args[0]);
            const std::string texture_name = joined_name_args(args, 1u);
            if (!entity || !apply_active_arcade_material_texture(*entity, texture_name)) {
                out.PrintLine("material_texture_apply: failed");
                return;
            }
            out.FormatLine("material_texture_apply: {} -> {}", entity->raw, texture_name);
        });
    console_ref.RegisterCommand(
        "material_texture_apply_selected",
        "Apply a base-color texture to the selected renderable.",
        [](std::span<const std::string_view> args, console::Output& out) {
            if (args.empty()) {
                out.PrintLine("material_texture_apply_selected: expected <texture_path|none>");
                return;
            }
            const std::string texture_name = joined_name_args(args, 0u);
            if (!apply_active_arcade_material_texture_to_selection(texture_name)) {
                out.PrintLine("material_texture_apply_selected: no selected renderable");
                return;
            }
            out.FormatLine("material_texture_apply_selected: {}", texture_name);
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

std::optional<scene::GameplayRole> parse_gameplay_role_arg(std::string_view text) {
    if (text == "none" || text == "None")
        return scene::GameplayRole::None;
    if (text == "player_start" || text == "PlayerStart" || text == "spawn")
        return scene::GameplayRole::PlayerStart;
    if (text == "fps_player" || text == "player" || text == "PlayerController")
        return scene::GameplayRole::PlayerController;
    if (text == "enemy" || text == "Enemy")
        return scene::GameplayRole::Enemy;
    if (text == "pickup" || text == "Pickup")
        return scene::GameplayRole::Pickup;
    if (text == "trigger" || text == "Trigger")
        return scene::GameplayRole::Trigger;
    if (text == "door" || text == "Door")
        return scene::GameplayRole::Door;
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

void sync_scene_names_to_web_labels(scene::Scene* scene) {
    if (!scene)
        return;
    auto& registry = scene->registry();
    const u32 total = registry.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> entities(total);
    const u32 copied = registry.snapshot_live_entities(entities);
    entities.resize(copied);
    for (Entity entity : entities) {
        const std::string_view name = scene->entity_name(entity);
        if (!name.empty())
            editor::set_web_entity_label(entity, name);
    }
}

void sync_web_labels_to_scene_names(scene::Scene* scene) {
    if (!scene)
        return;
    auto& registry = scene->registry();
    const u32 total = registry.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> entities(total);
    const u32 copied = registry.snapshot_live_entities(entities);
    entities.resize(copied);
    for (Entity entity : entities) {
        const std::string label = editor::web_entity_label(entity);
        if (!label.empty())
            scene->set_entity_name(entity, label);
    }
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

std::string compact_f64(f64 value) {
    if (!std::isfinite(value))
        return "0";
    std::string out = std::to_string(value);
    while (out.size() > 1u && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out.empty() ? std::string{"0"} : out;
}

std::string component_edit_value_text(const editor::ipc::SelectionComponentEditValue& value) {
    using Kind = editor::ipc::SelectionComponentEditValueKind;
    switch (value.kind) {
        case Kind::Bool:
            return value.bool_value ? "true" : "false";
        case Kind::I64:
            return std::to_string(value.i64_value);
        case Kind::U64:
            return std::to_string(value.u64_value);
        case Kind::F64:
            return compact_f64(value.f64_value);
        case Kind::String:
            return value.string_value;
        case Kind::BoolArray: {
            std::string out = "[";
            for (usize i = 0u; i < value.bool_values.size(); ++i) {
                if (i != 0u)
                    out += ",";
                out += value.bool_values[i] ? "1" : "0";
            }
            out += "]";
            return out;
        }
        case Kind::F64Array: {
            std::string out = "[";
            for (usize i = 0u; i < value.f64_values.size(); ++i) {
                if (i != 0u)
                    out += ",";
                out += compact_f64(value.f64_values[i]);
            }
            out += "]";
            return out;
        }
        case Kind::StringArray: {
            std::string out = "[";
            for (usize i = 0u; i < value.string_values.size(); ++i) {
                if (i != 0u)
                    out += ",";
                out += value.string_values[i];
            }
            out += "]";
            return out;
        }
        case Kind::Null:
            break;
    }
    return {};
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
    const std::array<std::string_view, 12> lines{
        status,
        "Press `~` for the console.",
        "arcade_load_scene <path>  Load a cooked .psyscene",
        "primitive_add box         Add primitives to the active scene",
        "web_console               Open the web editor workbench",
        "arcade_new_scene          Create a blank scene",
        "arcade_play_mode          Test authored FPS/player controls",
        "arcade_edit_mode          Return to transform/selection editing",
        "RMB + WASD/QE            Fly the editor viewport camera",
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
    std::string loading_scene_path{};
    std::string idle_status = "No scene loaded.";
    u32 primitive_spawn_count = 0u;
    bool show_idle_panel = true;
    bool audio_started = false;
    bool replaying_history = false;
    editor::command_history::History edit_history{128u};
    std::vector<scene::SceneLightItem> gathered_lights{};
    std::vector<scene::SceneRenderItem> pick_render_items{};
    std::unordered_map<std::string, render::Texture2D> authored_textures{};
    scene::detail::AlignedVector<u8> saved_scene_checkpoint{};
    bool has_saved_scene_checkpoint = false;
    editor::viewport::GizmoState gizmo_state{};
    bool viewport_mouse_left_prev = false;
    // Play-mode physics runtime (DOTS: bodies live in RigidBodyComponent). On
    // the Edit->Play edge begin() builds the world; Play->Edit end() restores
    // authored transforms. tick() steps + writes poses back each play frame.
    editor::play::PlayRuntime play_runtime_{};
    editor::Mode play_prev_mode_ = editor::Mode::Edit;
    // Viewport render mode: false = software raster (default), true = software
    // raytracer (editor::render::render_scene_rt). Toggled via `rt_mode`.
    bool rt_mode_ = false;
    // Level-source geometry must outlive the scene: MeshDesc holds non-owning
    // pointers into these pools, registered into the RenderingSystem MeshLibrary.
    std::vector<std::unique_ptr<editor::world::LevelGeometry>> level_geometries_{};
    // Last lightmap bake (stored for a future baked/flat toggle).
    editor::render::BakeResult last_bake_{};
    Entity fps_controlled_entity{};
    f32 fps_yaw = 0.0f;
    f32 fps_pitch = 0.0f;
    Entity editor_camera_entity{};
    f32 editor_camera_yaw = 0.0f;
    f32 editor_camera_pitch = 0.0f;
    struct GizmoDragEdit {
        Entity entity{};
        editor::viewport::GizmoMode mode = editor::viewport::GizmoMode::Translate;
        scene::LocalTransform before{};
        scene::LocalTransform after{};
        bool active = false;
    };
    GizmoDragEdit gizmo_drag{};
    struct SceneSnapshot {
        bool has_scene = false;
        scene::detail::AlignedVector<u8> bytes{};
        u32 selected_raw = 0u;
        u32 primitive_spawn_count = 0u;
        bool show_idle_panel = true;
        std::string idle_status{};
    };

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
        loading_scene_path.assign(path.data(), path.size());
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

    void create_fps_template_scene() {
        if (!app)
            return;
        const std::optional<SceneSnapshot> before =
            replaying_history ? std::nullopt : capture_scene_snapshot();
        reset_active_authoring_scene();

        scene::Scene& scene = app->create_active_scene();
        scene.environment().set_clear_color(ui::imm::rgba(0x08, 0x0D, 0x16));
        scene.environment().set_sky(scene::EnvironmentSkySettings{
            .zenith_rgba8 = ui::imm::rgba(0x16, 0x24, 0x36),
            .horizon_rgba8 = ui::imm::rgba(0x2E, 0x45, 0x58),
            .intensity = 1.1f,
        });

        auto make_material = [](u32 color, f32 roughness = 0.8f, f32 emissive = 0.0f) {
            render::MaterialDesc material{};
            material.albedo_rgba8 = color;
            material.roughness = roughness;
            material.emissive = emissive;
            return material;
        };
        auto create_mesh = [&](render::BuiltInMesh mesh_kind,
                               render::MaterialDesc material,
                               scene::LocalTransform local,
                               std::string_view label,
                               scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
            const render::MeshId mesh = app->rendering_system().builtin_mesh(mesh_kind);
            const render::MaterialId material_id = scene.materials().create(material);
            const render::SceneMeshEntity created =
                app->rendering_system().create_mesh_instance(scene,
                                                             mesh,
                                                             material_id,
                                                             local,
                                                             scene::kInvalidSceneNode,
                                                             scene::RenderableFlags::Visible,
                                                             mobility);
            if (created.entity.valid()) {
                editor::set_web_entity_label(created.entity, label);
                scene.set_entity_name(created.entity, label);
            }
            return created.entity;
        };

        scene::LocalTransform floor{};
        floor.translation = {0.0f, -0.04f, -2.5f};
        floor.scale = {8.0f, 0.04f, 8.0f};
        create_mesh(render::BuiltInMesh::UnitCube,
                    make_material(ui::imm::rgba(0x44, 0x4B, 0x54), 0.92f),
                    floor,
                    "Arena Floor",
                    scene::ObjectMobility::Static);

        scene::LocalTransform cover_a{};
        cover_a.translation = {-1.6f, 0.45f, -2.4f};
        cover_a.scale = {0.75f, 0.45f, 0.75f};
        create_mesh(render::BuiltInMesh::UnitCube,
                    make_material(ui::imm::rgba(0x62, 0x7B, 0x90), 0.85f),
                    cover_a,
                    "Cover Box A",
                    scene::ObjectMobility::Static);

        scene::LocalTransform cover_b{};
        cover_b.translation = {1.7f, 0.45f, -3.6f};
        cover_b.scale = {0.55f, 0.9f, 0.55f};
        create_mesh(render::BuiltInMesh::UnitCube,
                    make_material(ui::imm::rgba(0x75, 0x67, 0x9A), 0.78f),
                    cover_b,
                    "Cover Box B",
                    scene::ObjectMobility::Static);

        const Entity player = scene.create_entity(scene::LocalTransform{
            .translation = {0.0f, 0.0f, 1.4f},
            .rotation = math::quat_identity(),
            .scale = {1.0f, 1.0f, 1.0f},
        });
        scene::GameplayTagComponent player_tag{};
        player_tag.role = scene::GameplayRole::PlayerController;
        scene.registry().add<scene::GameplayTagComponent>(
            player, scene::sanitize_gameplay_tag(player_tag));
        scene::PlayerControllerComponent controller{};
        scene.registry().add<scene::PlayerControllerComponent>(
            player, scene::sanitize_player_controller(controller));
        scene::HealthComponent player_health{};
        scene.registry().add<scene::HealthComponent>(
            player, scene::sanitize_health_component(player_health));
        scene::WeaponComponent weapon{};
        scene.registry().add<scene::WeaponComponent>(
            player, scene::sanitize_weapon_component(weapon));
        editor::set_web_entity_label(player, "FPS Player");
        scene.set_entity_name(player, "FPS Player");

        scene::CameraDesc fps_camera{};
        fps_camera.position = {0.0f, controller.height, 0.0f};
        fps_camera.look_at = {0.0f, controller.height, -1.0f};
        fps_camera.active = false;
        const Entity fps_camera_entity = scene.spawn_camera(fps_camera, scene.node(player));
        if (fps_camera_entity.valid()) {
            editor::set_web_entity_label(fps_camera_entity, "FPS Camera");
            scene.set_entity_name(fps_camera_entity, "FPS Camera");
        }

        const Entity player_start = scene.create_entity(scene::LocalTransform{
            .translation = {0.0f, 0.0f, 1.4f},
            .rotation = math::quat_identity(),
            .scale = {1.0f, 1.0f, 1.0f},
        });
        scene::GameplayTagComponent start_tag{};
        start_tag.role = scene::GameplayRole::PlayerStart;
        scene.registry().add<scene::GameplayTagComponent>(
            player_start, scene::sanitize_gameplay_tag(start_tag));
        editor::set_web_entity_label(player_start, "Player Start");
        scene.set_entity_name(player_start, "Player Start");

        scene::LocalTransform enemy_local{};
        enemy_local.translation = {0.0f, 0.65f, -4.2f};
        enemy_local.scale = {0.55f, 0.75f, 0.55f};
        const Entity enemy = create_mesh(render::BuiltInMesh::UvSphere,
                                         make_material(ui::imm::rgba(0xEF, 0x5A, 0x5A), 0.55f),
                                         enemy_local,
                                         "Enemy Dummy");
        scene::GameplayTagComponent enemy_tag{};
        enemy_tag.role = scene::GameplayRole::Enemy;
        scene.registry().add<scene::GameplayTagComponent>(
            enemy, scene::sanitize_gameplay_tag(enemy_tag));
        scene::HealthComponent enemy_health{};
        enemy_health.max_health = 75.0f;
        enemy_health.current_health = 75.0f;
        enemy_health.faction = 1u;
        scene.registry().add<scene::HealthComponent>(
            enemy, scene::sanitize_health_component(enemy_health));

        scene::LocalTransform pickup_local{};
        pickup_local.translation = {2.4f, 0.25f, -1.2f};
        pickup_local.scale = {0.22f, 0.22f, 0.22f};
        const Entity pickup =
            create_mesh(render::BuiltInMesh::UvSphere,
                        make_material(ui::imm::rgba(0x68, 0xF0, 0xB6), 0.35f, 0.85f),
                        pickup_local,
                        "Health Pickup");
        scene::GameplayTagComponent pickup_tag{};
        pickup_tag.role = scene::GameplayRole::Pickup;
        scene.registry().add<scene::GameplayTagComponent>(
            pickup, scene::sanitize_gameplay_tag(pickup_tag));

        scene::LocalTransform door_local{};
        door_local.translation = {-2.8f, 0.8f, -4.1f};
        door_local.scale = {0.55f, 0.8f, 0.12f};
        const Entity door = create_mesh(render::BuiltInMesh::UnitCube,
                                        make_material(ui::imm::rgba(0x5C, 0xA8, 0xFF), 0.5f),
                                        door_local,
                                        "Blue Door",
                                        scene::ObjectMobility::Static);
        scene::GameplayTagComponent door_tag{};
        door_tag.role = scene::GameplayRole::Door;
        scene.registry().add<scene::GameplayTagComponent>(
            door, scene::sanitize_gameplay_tag(door_tag));

        scene::LocalTransform trigger_local{};
        trigger_local.translation = {-2.8f, 0.05f, -3.2f};
        const Entity trigger = scene.create_entity(trigger_local);
        scene::GameplayTagComponent trigger_tag{};
        trigger_tag.role = scene::GameplayRole::Trigger;
        scene.registry().add<scene::GameplayTagComponent>(
            trigger, scene::sanitize_gameplay_tag(trigger_tag));
        editor::set_web_entity_label(trigger, "Door Trigger");
        scene.set_entity_name(trigger, "Door Trigger");

        scene::LocalTransform light_marker{};
        light_marker.translation = {0.0f, 2.7f, -1.6f};
        light_marker.scale = {0.18f, 0.18f, 0.18f};
        const u32 light_color = ui::imm::rgba(0xFF, 0xE6, 0xA8);
        const Entity light = create_mesh(render::BuiltInMesh::UvSphere,
                                         make_material(light_color, 0.3f, 1.8f),
                                         light_marker,
                                         "Key Light");
        scene::LightComponent key_light{};
        key_light.kind = scene::LightKind::Point;
        key_light.color_rgba8 = light_color;
        key_light.intensity = 5.0f;
        key_light.range = 9.0f;
        key_light.casts_shadow = 1u;
        scene.attach_light(light, key_light);

        const Entity editor_camera = scene.spawn_camera(scene::CameraDesc{
            .position = math::Vec3{0.0f, 3.0f, 5.8f},
            .look_at = math::Vec3{0.0f, 0.65f, -2.0f},
            .up = math::Vec3{0.0f, 1.0f, 0.0f},
            .fov_y_rad = 58.0f * math::kDegToRad,
            .active = true,
        });
        if (editor_camera.valid()) {
            editor::set_web_entity_label(editor_camera, "Editor Camera");
            scene.set_entity_name(editor_camera, "Editor Camera");
        }

        primitive_spawn_count = 12u;
        load_target_scene = nullptr;
        show_idle_panel = false;
        idle_status = "FPS starter scene active.";
        editor::selection::select_scene_entity(&scene, player);
        mark_authoring_dirty();
        editor::publish_web_scene_hierarchy(&scene);
        push_snapshot_history_after("Create FPS template", before);
        PSY_LOG_INFO("psynder_arcade: created FPS starter scene");
    }

    Entity add_primitive(std::string_view kind, std::string_view material_preset = {}) {
        if (!app)
            return {};
        const std::optional<SceneSnapshot> before =
            replaying_history ? std::nullopt : capture_scene_snapshot();
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
        push_snapshot_history_after("Add primitive", before);
        return created.entity;
    }

    Entity add_empty_entity() {
        const std::optional<SceneSnapshot> before =
            replaying_history ? std::nullopt : capture_scene_snapshot();
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
            push_snapshot_history_after("Add empty entity", before);
        }
        return entity;
    }

    Entity add_camera_entity() {
        const std::optional<SceneSnapshot> before =
            replaying_history ? std::nullopt : capture_scene_snapshot();
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
            push_snapshot_history_after("Add camera", before);
        }
        return entity;
    }

    Entity add_light_entity(scene::LightKind kind = scene::LightKind::Point) {
        const std::optional<SceneSnapshot> before =
            replaying_history ? std::nullopt : capture_scene_snapshot();
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
            push_snapshot_history_after("Add light", before);
        }
        return created.entity;
    }

    Entity add_gameplay_entity(std::string_view kind) {
        const std::optional<SceneSnapshot> before =
            replaying_history ? std::nullopt : capture_scene_snapshot();
        scene::Scene* scene = ensure_active_scene();
        if (!scene)
            return {};

        const std::optional<scene::GameplayRole> role = parse_gameplay_role_arg(kind);
        if (!role || *role == scene::GameplayRole::None)
            return {};

        scene::LocalTransform local{};
        local.translation = primitive_spawn_position(primitive_spawn_count++);
        local.translation.y = 0.0f;
        const Entity entity = scene->create_entity(local);
        if (!entity.valid())
            return {};

        scene::GameplayTagComponent tag{};
        tag.role = *role;
        scene->registry().add<scene::GameplayTagComponent>(
            entity, scene::sanitize_gameplay_tag(tag));

        std::string label;
        switch (*role) {
            case scene::GameplayRole::PlayerStart:
                label = numbered_label("Player Start");
                break;
            case scene::GameplayRole::PlayerController: {
                label = numbered_label("FPS Player");
                scene::PlayerControllerComponent controller{};
                scene->registry().add<scene::PlayerControllerComponent>(
                    entity, scene::sanitize_player_controller(controller));
                scene::HealthComponent health{};
                scene->registry().add<scene::HealthComponent>(
                    entity, scene::sanitize_health_component(health));
                scene::WeaponComponent weapon{};
                scene->registry().add<scene::WeaponComponent>(
                    entity, scene::sanitize_weapon_component(weapon));

                scene::CameraDesc camera_desc{};
                camera_desc.position = {0.0f, controller.height, 0.0f};
                camera_desc.look_at = {0.0f, controller.height, -1.0f};
                camera_desc.active = scene->active_camera_entity().valid() ? false : true;
                const Entity camera =
                    scene->spawn_camera(camera_desc, scene->node(entity));
                if (camera.valid())
                    editor::set_web_entity_label(camera, "FPS Camera");
                break;
            }
            case scene::GameplayRole::Enemy: {
                label = numbered_label("Enemy");
                scene::HealthComponent health{};
                health.max_health = 75.0f;
                health.current_health = 75.0f;
                health.faction = 1u;
                scene->registry().add<scene::HealthComponent>(
                    entity, scene::sanitize_health_component(health));
                break;
            }
            case scene::GameplayRole::Pickup:
                label = numbered_label("Pickup");
                break;
            case scene::GameplayRole::Trigger:
                label = numbered_label("Trigger");
                break;
            case scene::GameplayRole::Door:
                label = numbered_label("Door");
                break;
            case scene::GameplayRole::None:
                break;
        }

        editor::set_web_entity_label(entity, label.empty() ? numbered_label("Gameplay") : label);
        show_idle_panel = false;
        mark_authoring_dirty();
        editor::publish_web_scene_hierarchy(scene);
        push_snapshot_history_after("Add gameplay entity", before);
        return entity;
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
        const std::optional<SceneSnapshot> before =
            replaying_history ? std::nullopt : capture_scene_snapshot();
        if (editor::selection::is_scene_selected(entity))
            editor::selection::clear_selection();
        if (gizmo_state.drag.active && gizmo_state.drag.entity == entity)
            editor::viewport::cancel_gizmo_drag(gizmo_state);
        if (gizmo_drag.active && gizmo_drag.entity == entity)
            gizmo_drag.active = false;
        const bool deleted = scene->destroy_entity(entity);
        editor::set_web_entity_label(entity, {});
        editor::publish_web_scene_hierarchy(scene);
        if (deleted)
            mark_authoring_dirty();
        if (deleted)
            push_snapshot_history_after("Delete entity", before);
        return deleted;
    }

    Entity duplicate_entity(Entity entity) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene || !scene->registry().alive(entity))
            return {};
        const std::optional<SceneSnapshot> before =
            replaying_history ? std::nullopt : capture_scene_snapshot();
        auto& registry = scene->registry();
        scene::LocalTransform local = scene->transform(entity);
        local.translation.x += 0.75f;
        local.translation.z -= 0.25f;
        const auto copy_gameplay_components = [&](Entity duplicate) {
            if (!duplicate.valid())
                return;
            if (const auto* tag = registry.get<scene::GameplayTagComponent>(entity))
                registry.add<scene::GameplayTagComponent>(
                    duplicate, scene::sanitize_gameplay_tag(*tag));
            if (const auto* controller = registry.get<scene::PlayerControllerComponent>(entity)) {
                registry.add<scene::PlayerControllerComponent>(
                    duplicate, scene::sanitize_player_controller(*controller));
            }
            if (const auto* health = registry.get<scene::HealthComponent>(entity))
                registry.add<scene::HealthComponent>(
                    duplicate, scene::sanitize_health_component(*health));
            if (const auto* weapon = registry.get<scene::WeaponComponent>(entity))
                registry.add<scene::WeaponComponent>(
                    duplicate, scene::sanitize_weapon_component(*weapon));
        };

        if (const auto* renderable = registry.get<scene::RenderableComponent>(entity)) {
            scene::RenderableComponent copy = *renderable;
            if (copy.material.valid())
                copy.material = scene->materials().create(scene->materials().get(copy.material));
            const Entity duplicate = scene->create_renderable(copy, local);
            if (duplicate.valid()) {
                if (const auto* light = registry.get<scene::LightComponent>(entity))
                    scene->attach_light(duplicate, *light);
                copy_gameplay_components(duplicate);
                editor::set_web_entity_label(duplicate, duplicate_label(entity, "Renderable"));
                editor::publish_web_scene_hierarchy(scene);
                mark_authoring_dirty();
                push_snapshot_history_after("Duplicate entity", before);
            }
            return duplicate;
        }

        if (const auto* camera = registry.get<scene::CameraComponent>(entity)) {
            const Entity duplicate = scene->create_camera(*camera, local);
            if (duplicate.valid()) {
                copy_gameplay_components(duplicate);
                editor::set_web_entity_label(duplicate, duplicate_label(entity, "Camera"));
                editor::publish_web_scene_hierarchy(scene);
                mark_authoring_dirty();
                push_snapshot_history_after("Duplicate entity", before);
            }
            return duplicate;
        }

        const Entity duplicate = scene->create_entity(local);
        if (duplicate.valid()) {
            copy_gameplay_components(duplicate);
            editor::set_web_entity_label(duplicate, duplicate_label(entity, "Entity"));
            editor::publish_web_scene_hierarchy(scene);
            mark_authoring_dirty();
            push_snapshot_history_after("Duplicate entity", before);
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
            if (field == "base_color_texture_name")
                return editor::web_material_texture_name(renderable->material.raw);
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

        if (component == "GameplayTagComponent") {
            const auto* tag = registry.get<scene::GameplayTagComponent>(entity);
            if (!tag)
                return std::nullopt;
            if (field == "role")
                return std::to_string(static_cast<u32>(tag->role));
            if (field == "flags")
                return std::to_string(tag->flags);
            return std::nullopt;
        }

        if (component == "PlayerControllerComponent") {
            const auto* controller = registry.get<scene::PlayerControllerComponent>(entity);
            if (!controller)
                return std::nullopt;
            if (field == "walk_speed")
                return f32_text(controller->walk_speed);
            if (field == "run_speed")
                return f32_text(controller->run_speed);
            if (field == "jump_speed")
                return f32_text(controller->jump_speed);
            if (field == "mouse_sensitivity")
                return f32_text(controller->mouse_sensitivity);
            if (field == "height")
                return f32_text(controller->height);
            if (field == "radius")
                return f32_text(controller->radius);
            return std::nullopt;
        }

        if (component == "HealthComponent") {
            const auto* health = registry.get<scene::HealthComponent>(entity);
            if (!health)
                return std::nullopt;
            if (field == "max_health")
                return f32_text(health->max_health);
            if (field == "current_health")
                return f32_text(health->current_health);
            if (field == "faction")
                return std::to_string(health->faction);
            return std::nullopt;
        }

        if (component == "WeaponComponent") {
            const auto* weapon = registry.get<scene::WeaponComponent>(entity);
            if (!weapon)
                return std::nullopt;
            if (field == "damage")
                return f32_text(weapon->damage);
            if (field == "range")
                return f32_text(weapon->range);
            if (field == "fire_rate")
                return f32_text(weapon->fire_rate);
            if (field == "ammo")
                return std::to_string(weapon->ammo);
            if (field == "automatic")
                return bool_text(weapon->automatic != 0u);
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
            if (field == "base_color_texture_name")
                return update_material_texture_binding(entity, value);
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

        if (component == "GameplayTagComponent") {
            const auto* tag = registry.get<scene::GameplayTagComponent>(entity);
            if (!tag)
                return false;
            scene::GameplayTagComponent updated = *tag;
            u32 u = 0u;
            if (field == "role") {
                if (!parse_u32_value(value, u) || u > static_cast<u32>(scene::GameplayRole::Door))
                    return false;
                updated.role = static_cast<scene::GameplayRole>(u);
            } else if (field == "flags") {
                if (!parse_u32_value(value, u))
                    return false;
                updated.flags = u;
            } else {
                return false;
            }
            registry.add<scene::GameplayTagComponent>(
                entity, scene::sanitize_gameplay_tag(updated));
            editor::publish_web_scene_hierarchy(scene);
            return true;
        }

        if (component == "PlayerControllerComponent") {
            const auto* controller = registry.get<scene::PlayerControllerComponent>(entity);
            if (!controller)
                return false;
            scene::PlayerControllerComponent updated = *controller;
            f32 f = 0.0f;
            if (field == "walk_speed") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.walk_speed = f;
            } else if (field == "run_speed") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.run_speed = f;
            } else if (field == "jump_speed") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.jump_speed = f;
            } else if (field == "mouse_sensitivity") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.mouse_sensitivity = f;
            } else if (field == "height") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.height = f;
            } else if (field == "radius") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.radius = f;
            } else {
                return false;
            }
            registry.add<scene::PlayerControllerComponent>(
                entity, scene::sanitize_player_controller(updated));
            editor::publish_web_scene_hierarchy(scene);
            return true;
        }

        if (component == "HealthComponent") {
            const auto* health = registry.get<scene::HealthComponent>(entity);
            if (!health)
                return false;
            scene::HealthComponent updated = *health;
            f32 f = 0.0f;
            u32 u = 0u;
            if (field == "max_health") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.max_health = f;
            } else if (field == "current_health") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.current_health = f;
            } else if (field == "faction") {
                if (!parse_u32_value(value, u))
                    return false;
                updated.faction = u;
            } else {
                return false;
            }
            registry.add<scene::HealthComponent>(
                entity, scene::sanitize_health_component(updated));
            editor::publish_web_scene_hierarchy(scene);
            return true;
        }

        if (component == "WeaponComponent") {
            const auto* weapon = registry.get<scene::WeaponComponent>(entity);
            if (!weapon)
                return false;
            scene::WeaponComponent updated = *weapon;
            f32 f = 0.0f;
            u32 u = 0u;
            bool b = false;
            if (field == "damage") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.damage = f;
            } else if (field == "range") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.range = f;
            } else if (field == "fire_rate") {
                if (!parse_f32_value(value, f))
                    return false;
                updated.fire_rate = f;
            } else if (field == "ammo") {
                if (!parse_u32_value(value, u))
                    return false;
                updated.ammo = u;
            } else if (field == "automatic") {
                if (!parse_bool_value(value, b))
                    return false;
                updated.automatic = b ? 1u : 0u;
            } else {
                return false;
            }
            registry.add<scene::WeaponComponent>(
                entity, scene::sanitize_weapon_component(updated));
            editor::publish_web_scene_hierarchy(scene);
            return true;
        }

        return false;
    }

    // Console-friendly material editing on the current selection. The web
    // Inspector lacks a material editor yet (#61), so these route through the
    // same apply_component_field("MaterialComponent", ...) path the IPC uses.
    bool set_selected_material_color(u32 rgba8) {
        return set_component_field(editor::selection::selected_scene_entity(),
                                   "MaterialComponent", "albedo_rgba8",
                                   std::to_string(rgba8));
    }
    bool set_selected_material_texture(std::string_view name) {
        return set_component_field(editor::selection::selected_scene_entity(),
                                   "MaterialComponent", "base_color_texture_name", name);
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

    render::TextureView resolve_authoring_texture(std::string_view texture_name) {
        if (texture_name.empty() || texture_name == "none" || texture_name == "None" ||
            texture_name == "clear") {
            return {};
        }

        const std::string key{texture_name};
        if (const auto it = authored_textures.find(key); it != authored_textures.end())
            return it->second.view();

        render::Texture2D texture{};
        if (texture_name == "textures.procedural.wooden_crate") {
            texture = render::texture_generators::wooden_crate();
        } else if (texture_name == "textures.procedural.checker") {
            texture = render::texture_generators::checker();
        } else if (texture_name == "textures.procedural.grid") {
            texture = render::texture_generators::grid();
        } else if (texture_name == "textures.procedural.bricks") {
            texture = render::texture_generators::bricks();
        } else if (texture_name == "textures.procedural.wood_planks") {
            texture = render::texture_generators::wood_planks();
        } else if (texture_name == "textures.procedural.building_facade") {
            texture = render::texture_generators::building_facade();
        } else {
            render::Rgba8Image image{};
            const asset::Blob blob = asset::Vault::Get().read(texture_name);
            if (blob.data && render::image_detail::decode_ppm_rgba8(
                                 std::span<const u8>{blob.data, blob.bytes}, image)) {
                texture = render::Texture2D{std::move(image)};
            } else {
                texture = render::fallback_checker_texture();
                PSY_LOG_WARN("psynder_arcade: texture '{}' unavailable; using fallback checker",
                             texture_name);
            }
        }

        const auto [it, inserted] = authored_textures.emplace(key, std::move(texture));
        (void)inserted;
        return it->second.view();
    }

    bool update_material_texture_binding(Entity entity, std::string_view texture_name) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene || !scene->registry().alive(entity))
            return false;
        const auto* renderable = scene->registry().get<scene::RenderableComponent>(entity);
        if (!renderable || !scene->materials().valid(renderable->material))
            return false;

        render::MaterialDesc material = scene->materials().get(renderable->material);
        const bool clear = texture_name.empty() || texture_name == "none" ||
                           texture_name == "None" || texture_name == "clear";
        material.base_color_texture = 0u;
        material.base_color = clear ? render::TextureView{} : resolve_authoring_texture(texture_name);
        material.base_color_asset = nullptr;
        if (!scene->materials().update(renderable->material, material))
            return false;
        editor::set_web_material_texture_name(renderable->material.raw,
                                              clear ? std::string_view{} : texture_name);
        editor::publish_web_scene_hierarchy(scene);
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
        render::MaterialDesc after = primitive_material_desc(preset, entity.index());
        after.base_color_texture = before.base_color_texture;
        after.base_color = before.base_color;
        after.base_color_asset = before.base_color_asset;
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

    bool apply_material_texture(Entity entity, std::string_view texture_name) {
        return set_component_field(entity,
                                   "MaterialComponent",
                                   "base_color_texture_name",
                                   texture_name);
    }

    bool apply_material_texture_to_selection(std::string_view texture_name) {
        const Entity selected = editor::selection::selected_scene_entity();
        return selected.valid() && apply_material_texture(selected, texture_name);
    }

    bool save_scene(std::string_view path, std::string& error) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene) {
            error = "no active scene";
            return false;
        }
        sync_web_labels_to_scene_names(scene);

        scene::detail::AlignedVector<u8> bytes;
        scene::SceneFileSaveStats stats{};
        const scene::SceneFileSaveHooks hooks{
            .user = this,
            .mesh_name = &PlayerApp::save_mesh_name,
            .material_name = &PlayerApp::save_material_name,
            .material_base_color_texture_name = &PlayerApp::save_material_texture_name,
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
        saved_scene_checkpoint = bytes;
        has_saved_scene_checkpoint = true;
        set_authoring_dirty(false);
        return true;
    }

    bool undo_editor_command(std::string& label) {
        editor::command_history::Command command;
        if (!edit_history.undo(command))
            return false;
        label = std::string{command.label()};
        set_authoring_dirty(!active_scene_matches_saved_checkpoint());
        if (app)
            editor::publish_web_scene_hierarchy(app->active_scene());
        return true;
    }

    bool redo_editor_command(std::string& label) {
        editor::command_history::Command command;
        if (!edit_history.redo(command))
            return false;
        label = std::string{command.label()};
        set_authoring_dirty(!active_scene_matches_saved_checkpoint());
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

    Entity find_fps_controller_entity(scene::Scene& scene) {
        auto& registry = scene.registry();
        if (fps_controlled_entity.valid() && registry.alive(fps_controlled_entity) &&
            registry.get<scene::PlayerControllerComponent>(fps_controlled_entity)) {
            return fps_controlled_entity;
        }

        Entity fallback{};
        const u32 total = registry.snapshot_live_entities(std::span<Entity>{});
        std::vector<Entity> entities(total);
        registry.snapshot_live_entities(entities);
        for (Entity entity : entities) {
            if (!registry.get<scene::PlayerControllerComponent>(entity))
                continue;
            if (!fallback.valid())
                fallback = entity;
            const auto* tag = registry.get<scene::GameplayTagComponent>(entity);
            if (tag && tag->role == scene::GameplayRole::PlayerController)
                return entity;
        }
        return fallback;
    }

    Entity find_child_camera(scene::Scene& scene, Entity parent) const {
        if (!parent.valid())
            return {};
        const scene::SceneNode parent_node = scene.node(parent);
        if (!parent_node.valid())
            return {};
        auto& registry = scene.registry();
        const u32 total = registry.snapshot_live_entities(std::span<Entity>{});
        std::vector<Entity> entities(total);
        registry.snapshot_live_entities(entities);
        for (Entity entity : entities) {
            if (!registry.get<scene::CameraComponent>(entity))
                continue;
            const scene::SceneNode node = scene.node(entity);
            if (node.valid() && scene.graph().parent(node).raw == parent_node.raw)
                return entity;
        }
        return {};
    }

    // Drive the play-mode physics runtime. begin/end fire on the Edit<->Play
    // edge; tick steps the world while playing. Sim pauses when a console/web
    // overlay is capturing input (same gate as the FPS controller).
    void update_play_runtime(f32 dt) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene)
            return;
        const editor::Mode mode = editor::current_mode();
        if (mode != play_prev_mode_) {
            if (mode == editor::Mode::Play)
                play_runtime_.begin(*scene);
            else
                play_runtime_.end(*scene);
            play_prev_mode_ = mode;
        }
        if (mode == editor::Mode::Play && dt > 0.0f && !editor::overlays_capturing()) {
            // Feed WASD driving intent to any player vehicle before the tick:
            // W = throttle, S = brake/reverse, A/D = steer +/-0.5 rad. The
            // runtime applies it to every is_player VehicleComponent and drives
            // the chase camera inside tick().
            if (const platform::Input* input = platform::input()) {
                const f32 throttle = input->key_down(platform::KeyCode::W) ? 1.0f : 0.0f;
                const f32 brake = input->key_down(platform::KeyCode::S) ? 1.0f : 0.0f;
                f32 steer = 0.0f;
                if (input->key_down(platform::KeyCode::A))
                    steer += 0.5f;
                if (input->key_down(platform::KeyCode::D))
                    steer -= 0.5f;
                play_runtime_.set_vehicle_input(throttle, brake, steer);

                // Helicopter flight intent (only a player heli reacts):
                // Space = ascend, LeftCtrl/LeftShift = descend (collective);
                // W/S = pitch fwd/back, A/D = roll left/right, Q/E = yaw
                // left/right. All cyclic/pedal channels are +/-1.
                f32 collective = 0.0f;
                if (input->key_down(platform::KeyCode::Space))
                    collective += 1.0f;
                if (input->key_down(platform::KeyCode::LeftCtrl) ||
                    input->key_down(platform::KeyCode::LeftShift))
                    collective -= 1.0f;
                f32 pitch = 0.0f;
                if (input->key_down(platform::KeyCode::W))
                    pitch += 1.0f;  // nose down -> fly forward
                if (input->key_down(platform::KeyCode::S))
                    pitch -= 1.0f;
                f32 roll = 0.0f;
                if (input->key_down(platform::KeyCode::A))
                    roll -= 1.0f;  // roll left
                if (input->key_down(platform::KeyCode::D))
                    roll += 1.0f;  // roll right
                f32 yaw = 0.0f;
                if (input->key_down(platform::KeyCode::Q))
                    yaw += 1.0f;  // yaw left
                if (input->key_down(platform::KeyCode::E))
                    yaw -= 1.0f;  // yaw right
                play_runtime_.set_helicopter_input(collective, pitch, roll, yaw);
            }
            play_runtime_.tick(*scene, dt);
        }
    }

    // Tag a specific entity as a dynamic rigid body so it simulates in Play
    // mode. Box collider sized from the renderable's local bounds (half-extent).
    // `entity` is validated alive here so an IPC add_component that was queued
    // on a socket thread and drained a frame later still targets a live entity.
    bool add_rigid_body_to_entity(Entity entity, bool make_static) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene)
            return false;
        if (!entity.valid() || !scene->registry().alive(entity))
            return false;
        auto& reg = scene->registry();
        if (auto* existing = reg.get<editor::play::RigidBodyComponent>(entity)) {
            existing->mass = make_static ? 0.0f : 1.0f;  // retag in place
            return true;
        }
        editor::play::RigidBodyComponent rb{};
        rb.shape = physics::Shape::Box;
        rb.mass = make_static ? 0.0f : 1.0f;  // 0 = static floor/wall
        if (const auto* r = reg.get<scene::RenderableComponent>(entity)) {
            const math::Vec3 ext = math::mul(math::sub(r->local_bounds.max, r->local_bounds.min), 0.5f);
            rb.half_extent = math::Vec3{std::max(0.05f, ext.x), std::max(0.05f, ext.y),
                                        std::max(0.05f, ext.z)};
        }
        reg.add<editor::play::RigidBodyComponent>(entity, rb);
        return true;
    }

    bool add_rigid_body_to_selected(bool make_static) {
        return add_rigid_body_to_entity(editor::selection::selected_scene_entity(), make_static);
    }

    // Tag a specific entity as a drivable player vehicle so it simulates +
    // accepts WASD in Play mode. Chassis box sized from the renderable's local
    // bounds (half-extent), falling back to the component default.
    bool add_vehicle_to_entity(Entity entity) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene)
            return false;
        if (!entity.valid() || !scene->registry().alive(entity))
            return false;
        auto& reg = scene->registry();
        if (reg.get<editor::play::VehicleComponent>(entity) != nullptr)
            return true;  // already a vehicle
        editor::play::VehicleComponent vc{};
        vc.is_player = true;
        if (const auto* r = reg.get<scene::RenderableComponent>(entity)) {
            const math::Vec3 ext = math::mul(math::sub(r->local_bounds.max, r->local_bounds.min), 0.5f);
            vc.half_extent = math::Vec3{std::max(0.1f, ext.x), std::max(0.1f, ext.y),
                                        std::max(0.1f, ext.z)};
        }
        reg.add<editor::play::VehicleComponent>(entity, vc);
        return true;
    }

    bool add_vehicle_to_selected() {
        return add_vehicle_to_entity(editor::selection::selected_scene_entity());
    }

    // Tag a specific entity as a flyable player helicopter so it simulates +
    // accepts flight input in Play mode. Chassis box sized from the renderable's
    // local bounds (half-extent), falling back to the component default.
    bool add_helicopter_to_entity(Entity entity) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene)
            return false;
        if (!entity.valid() || !scene->registry().alive(entity))
            return false;
        auto& reg = scene->registry();
        if (reg.get<editor::play::HelicopterComponent>(entity) != nullptr)
            return true;  // already a helicopter
        editor::play::HelicopterComponent hc{};
        hc.is_player = true;
        if (const auto* r = reg.get<scene::RenderableComponent>(entity)) {
            const math::Vec3 ext = math::mul(math::sub(r->local_bounds.max, r->local_bounds.min), 0.5f);
            hc.half_extent = math::Vec3{std::max(0.1f, ext.x), std::max(0.1f, ext.y),
                                        std::max(0.1f, ext.z)};
        }
        reg.add<editor::play::HelicopterComponent>(entity, hc);
        return true;
    }

    bool add_helicopter_to_selected() {
        return add_helicopter_to_entity(editor::selection::selected_scene_entity());
    }

    void set_rt_mode(bool on) noexcept { rt_mode_ = on; }
    [[nodiscard]] bool rt_mode() const noexcept { return rt_mode_; }

    // Bake static lightmaps for the active scene; store for a later baked/flat
    // toggle. Returns true when something bakeable was found.
    bool bake_scene_lightmaps() {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene)
            return false;
        last_bake_ = editor::render::bake_lightmaps(*scene, app->rendering_system());
        return last_bake_.ok;
    }

    // After a level-source load, upload each LevelMesh into the RenderingSystem
    // MeshLibrary (which assigns real MeshIds) and remap the spawned entities'
    // RenderableComponent.geometry_id from the loader's provisional ids to the
    // real MeshId.raw, so the standard renderer resolves them.
    void register_level_meshes(scene::Scene& scene,
                               editor::world::LevelGeometry& geom,
                               const std::vector<Entity>& entities) {
        std::unordered_map<u32, u32> remap;
        for (const editor::world::LevelMesh& m : geom.meshes()) {
            const render::MeshId id = app->rendering_system().meshes().create_mesh(m.desc);
            remap[m.geometry_id] = id.raw;
        }
        auto& reg = scene.registry();
        for (Entity e : entities) {
            if (auto* r = reg.get<scene::RenderableComponent>(e)) {
                const auto it = remap.find(r->geometry_id);
                if (it != remap.end())
                    r->geometry_id = it->second;
            }
        }
    }

    bool world_new_indoor() {
        scene::Scene* scene = ensure_active_scene();
        if (!scene)
            return false;
        auto geom = std::make_unique<editor::world::LevelGeometry>(0x40000000u);
        editor::world::BspLevelSource src{};
        editor::world::build_demo_bsp_level(src);
        std::vector<Entity> ents;
        const editor::world::LoadResult res =
            editor::world::load_bsp_into_scene(*scene, src, *geom, {}, &ents);
        register_level_meshes(*scene, *geom, ents);
        level_geometries_.push_back(std::move(geom));
        mark_authoring_dirty();
        editor::publish_web_scene_hierarchy(scene);
        return res.entities_created > 0u;
    }

    bool world_new_terrain() {
        scene::Scene* scene = ensure_active_scene();
        if (!scene)
            return false;
        auto geom = std::make_unique<editor::world::LevelGeometry>(0x50000000u);
        world::outdoor::HeightmapDesc desc{};
        const std::vector<u16> heights = editor::world::build_demo_heightmap(desc);
        std::vector<Entity> ents;
        const editor::world::LoadResult res =
            editor::world::load_terrain_into_scene(*scene, desc, *geom, {}, &ents);
        register_level_meshes(*scene, *geom, ents);
        level_geometries_.push_back(std::move(geom));
        mark_authoring_dirty();
        editor::publish_web_scene_hierarchy(scene);
        return res.entities_created > 0u;
    }

    void update_fps_player(f32 dt) {
        if (editor::current_mode() != editor::Mode::Play || editor::overlays_capturing())
            return;
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        const platform::Input* input = platform::input();
        if (!scene || !input || dt <= 0.0f)
            return;

        Entity player = find_fps_controller_entity(*scene);
        if (!player.valid())
            return;
        auto& registry = scene->registry();
        const auto* controller = registry.get<scene::PlayerControllerComponent>(player);
        if (!controller)
            return;
        const scene::PlayerControllerComponent cfg =
            scene::sanitize_player_controller(*controller);

        scene::LocalTransform local = scene->transform(player);
        if (fps_controlled_entity.raw != player.raw) {
            const math::Vec3 euler = rotation_degrees(local.rotation);
            fps_yaw = euler.y * math::kDegToRad;
            fps_pitch = 0.0f;
            fps_controlled_entity = player;
        }

        const platform::MouseState& mouse = input->mouse();
        fps_yaw += mouse.dx * cfg.mouse_sensitivity * 0.01f;
        fps_pitch = std::clamp(fps_pitch + mouse.dy * cfg.mouse_sensitivity * 0.01f,
                               -math::kHalfPi + 0.02f,
                               math::kHalfPi - 0.02f);

        const math::Vec3 forward{std::sin(fps_yaw), 0.0f, -std::cos(fps_yaw)};
        const math::Vec3 right{std::cos(fps_yaw), 0.0f, std::sin(fps_yaw)};
        math::Vec3 intent{};
        if (input->key_down(platform::KeyCode::W))
            intent = math::add(intent, forward);
        if (input->key_down(platform::KeyCode::S))
            intent = math::sub(intent, forward);
        if (input->key_down(platform::KeyCode::D))
            intent = math::add(intent, right);
        if (input->key_down(platform::KeyCode::A))
            intent = math::sub(intent, right);
        if (input->key_down(platform::KeyCode::Space))
            intent.y += 1.0f;
        if (input->key_down(platform::KeyCode::LeftCtrl) ||
            input->key_down(platform::KeyCode::RightCtrl)) {
            intent.y -= 1.0f;
        }

        const f32 intent_len = std::sqrt(math::dot(intent, intent));
        if (intent_len > 0.0001f) {
            const bool running = input->key_down(platform::KeyCode::LeftShift) ||
                                 input->key_down(platform::KeyCode::RightShift);
            const f32 speed = running ? cfg.run_speed : cfg.walk_speed;
            local.translation =
                math::add(local.translation, math::mul(intent, (speed * dt) / intent_len));
        }
        local.rotation = math::quat_normalize(math::quat_from_euler(0.0f, fps_yaw, 0.0f));
        (void)scene->set_transform(player, local);

        const Entity camera = find_child_camera(*scene, player);
        if (camera.valid()) {
            scene::LocalTransform camera_local = scene->transform(camera);
            camera_local.translation = {0.0f, cfg.height, 0.0f};
            camera_local.rotation =
                math::quat_normalize(math::quat_from_euler(fps_pitch, 0.0f, 0.0f));
            (void)scene->set_transform(camera, camera_local);
            scene->set_active_camera(camera);
        }
    }

    void update_editor_viewport_camera(f32 dt) {
        if (editor::current_mode() != editor::Mode::Edit || editor::overlays_capturing())
            return;
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        const platform::Input* input = platform::input();
        if (!scene || !input || dt <= 0.0f)
            return;

        const Entity camera = scene->active_camera_entity();
        if (!camera.valid() || !scene->registry().alive(camera) ||
            !scene->registry().get<scene::CameraComponent>(camera)) {
            editor_camera_entity = {};
            return;
        }

        scene::LocalTransform local = scene->transform(camera);
        if (editor_camera_entity.raw != camera.raw) {
            const math::Vec3 euler = math::quat_to_euler(math::quat_normalize(local.rotation));
            editor_camera_pitch = euler.x;
            editor_camera_yaw = euler.y;
            editor_camera_entity = camera;
        }

        if (input->key_pressed(platform::KeyCode::F))
            focus_editor_camera_on_selection(*scene, camera, local);

        const platform::MouseState& mouse = input->mouse();
        if (!mouse.right)
            return;

        bool changed = false;
        constexpr f32 kLookSensitivity = 0.0065f;
        if (std::fabs(mouse.dx) > 0.0001f || std::fabs(mouse.dy) > 0.0001f) {
            editor_camera_yaw += mouse.dx * kLookSensitivity;
            editor_camera_pitch = std::clamp(editor_camera_pitch + mouse.dy * kLookSensitivity,
                                             -math::kHalfPi + 0.02f,
                                             math::kHalfPi - 0.02f);
            changed = true;
        }

        const math::Quat rotation = math::quat_normalize(
            math::quat_from_euler(editor_camera_pitch, editor_camera_yaw, 0.0f));
        const math::Vec3 forward =
            math::normalize(math::quat_rotate(rotation, math::Vec3{0.0f, 0.0f, -1.0f}));
        const math::Vec3 right =
            math::normalize(math::quat_rotate(rotation, math::Vec3{1.0f, 0.0f, 0.0f}));
        constexpr math::Vec3 kWorldUp{0.0f, 1.0f, 0.0f};
        math::Vec3 intent{};
        if (input->key_down(platform::KeyCode::W))
            intent = math::add(intent, forward);
        if (input->key_down(platform::KeyCode::S))
            intent = math::sub(intent, forward);
        if (input->key_down(platform::KeyCode::D))
            intent = math::add(intent, right);
        if (input->key_down(platform::KeyCode::A))
            intent = math::sub(intent, right);
        if (input->key_down(platform::KeyCode::E))
            intent = math::add(intent, kWorldUp);
        if (input->key_down(platform::KeyCode::Q))
            intent = math::sub(intent, kWorldUp);

        const f32 intent_len = std::sqrt(math::dot(intent, intent));
        if (intent_len > 0.0001f) {
            const bool fast = input->key_down(platform::KeyCode::LeftShift) ||
                              input->key_down(platform::KeyCode::RightShift);
            constexpr f32 kBaseSpeed = 5.5f;
            constexpr f32 kFastSpeed = 16.0f;
            const f32 speed = fast ? kFastSpeed : kBaseSpeed;
            local.translation = math::add(local.translation,
                                          math::mul(intent, (speed * dt) / intent_len));
            changed = true;
        }

        if (!changed)
            return;

        local.rotation = rotation;
        if (scene->set_transform(camera, local)) {
            mark_authoring_dirty();
            editor::publish_web_scene_hierarchy(scene);
        }
    }

    void focus_editor_camera_on_selection(scene::Scene& scene,
                                          Entity camera,
                                          scene::LocalTransform& camera_local) {
        const Entity selected = editor::selection::selected_scene_entity();
        if (!selected.valid() || !scene.registry().alive(selected))
            return;

        scene.update_transforms();

        math::Vec3 target{};
        f32 radius = 1.0f;
        bool found_bounds = false;
        pick_render_items.clear();
        scene.gather_render_items(pick_render_items);
        for (const scene::SceneRenderItem& item : pick_render_items) {
            if (item.entity.raw != selected.raw || math::is_empty(item.world_bounds))
                continue;
            target = math::center(item.world_bounds);
            radius = std::max(0.35f, math::length(math::extents(item.world_bounds)));
            found_bounds = true;
            break;
        }

        if (!found_bounds) {
            if (const auto* node = scene.registry().get<scene::SceneNodeComponent>(selected);
                node && scene.graph().alive(node->node)) {
                const math::Mat4 world = scene.graph().world_matrix(node->node);
                target = math::Vec3{world.m[12], world.m[13], world.m[14]};
            } else {
                target = scene.transform(selected).translation;
            }
        }

        const math::Quat current_rotation = math::quat_normalize(camera_local.rotation);
        math::Vec3 forward =
            math::normalize(math::quat_rotate(current_rotation, math::Vec3{0.0f, 0.0f, -1.0f}));
        if (math::length(forward) <= 0.0f)
            forward = math::Vec3{0.0f, 0.0f, -1.0f};

        const f32 distance = std::clamp(radius * 3.0f, 2.5f, 40.0f);
        camera_local.translation = math::sub(target, math::mul(forward, distance));
        camera_local.rotation = math::quat_normalize(
            scene::camera_rotation_towards(camera_local.translation, target, {0.0f, 1.0f, 0.0f}));

        const math::Vec3 euler = math::quat_to_euler(camera_local.rotation);
        editor_camera_pitch = euler.x;
        editor_camera_yaw = euler.y;
        editor_camera_entity = camera;

        if (scene.set_transform(camera, camera_local)) {
            mark_authoring_dirty();
            editor::publish_web_scene_hierarchy(&scene);
        }
    }

    void sync_loaded_material_texture_names(const scene::SceneLoadResult& result) {
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene)
            return;
        const scene::SceneFileView& view = scene_load.loaded_file().view;
        for (usize i = 0u; i < view.mesh_instances.size() && i < result.mesh_entities.size();
             ++i) {
            const Entity entity = result.mesh_entities[i];
            if (!entity.valid() || !scene->registry().alive(entity))
                continue;
            const auto* renderable = scene->registry().get<scene::RenderableComponent>(entity);
            if (!renderable)
                continue;
            const scene::SceneFileMeshInstance& mesh = view.mesh_instances[i];
            const std::string_view material_name =
                scene::scene_file_string(view, mesh.material_name_offset);
            if (material_name.empty())
                continue;
            for (const scene::SceneFileMaterial& material : view.materials) {
                if (scene::scene_file_string(view, material.name_offset) != material_name)
                    continue;
                const std::string_view texture_name =
                    scene::scene_file_string(view, material.base_color_texture_name_offset);
                editor::set_web_material_texture_name(renderable->material.raw, texture_name);
                break;
            }
        }
    }

    void started(app::WindowApp& app_ref, const PlayerArgs& args) {
        app = &app_ref;
        g_active_arcade = this;
        editor::ensure_web_panel_commands_registered();
        editor::ipc::Server::Get().set_selection_component_edit_handler(
            apply_active_arcade_component_edit);
        editor::ipc::Server::Get().set_selection_component_add_handler(
            apply_active_arcade_component_add);
        register_arcade_console_commands();
        platform::runtime_config::register_console_commands();
        platform::runtime_config::register_console_archive_autosave();
        (void)platform::runtime_config::load_console_archive();
        start_boot_audio(args);

        scene_load
            .on_ready([this](const scene::SceneLoadResult& result) {
                if (app && load_target_scene)
                    app->set_scene(*load_target_scene);
                sync_loaded_material_texture_names(result);
                sync_scene_names_to_web_labels(app ? app->active_scene() : nullptr);
                remember_active_scene_as_saved_checkpoint();
                set_authoring_dirty(false);
                if (app)
                    editor::publish_web_scene_hierarchy(app->active_scene());
                loading_scene_path.clear();
                show_idle_panel = false;
                PSY_LOG_INFO("psynder_arcade: scene ready ({} mesh instances)",
                             result.instantiate.mesh_instances);
            })
            .on_error([this](std::string_view error) {
                idle_status = "Load failed: ";
                idle_status.append(error.data(), error.size());
                show_idle_panel = true;
                load_target_scene = nullptr;
                has_saved_scene_checkpoint = false;
                saved_scene_checkpoint.clear();
                set_authoring_dirty(false);
                editor::publish_web_scene_load_failed(loading_scene_path, error);
                editor::publish_web_scene_hierarchy(app ? app->active_scene() : nullptr);
                loading_scene_path.clear();
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
        update_play_runtime(ctx.dt);
        update_fps_player(ctx.dt);
        (void)ctx.app.engine_frame_update(ctx.dt);
        update_editor_viewport_camera(ctx.dt);
        handle_editor_undo_redo_keys();
        handle_editor_delete_key();
        if (show_idle_panel)
            draw_attract_mode(ctx.framebuffer, ctx.seconds, idle_status);
        (void)cr;
    }

    void frame_post(app::WindowFrameContextT<PlayerArgs>& ctx, app::WindowFrameCacheReady&) {
        if (!render_scene_raytraced(ctx))
            ctx.app.engine_frame_render();
        draw_scene_light_post(ctx);
        draw_viewport_gizmo_with_undo(ctx);
        ctx.app.engine_frame_post();
    }

    // Raytraced viewport path (rt_mode). Returns true if it rendered into the
    // framebuffer (so the caller skips the raster pass). Falls back to raster
    // when off, no scene, or no active camera.
    bool render_scene_raytraced(app::WindowFrameContextT<PlayerArgs>& ctx) {
        if (!rt_mode_)
            return false;
        scene::Scene* scene = ctx.app.active_scene();
        if (!scene)
            return false;
        scene::SceneCameraView view{};
        if (!scene->active_camera_view(framebuffer_aspect(ctx.framebuffer), view))
            return false;
        const editor::render::SceneRtStats stats = editor::render::render_scene_rt(
            *scene, view, ctx.app.rendering_system(), ctx.framebuffer);
        return stats.rendered;
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

    static bool project_to_screen_unclipped(const math::Vec3& world,
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
        out = {
            (ndc_x * 0.5f + 0.5f) * framebuffer_size.x,
            (1.0f - (ndc_y * 0.5f + 0.5f)) * framebuffer_size.y,
        };
        out_depth = clip.w;
        return true;
    }

    static bool project_bounds_to_screen_rect(const math::Aabb& bounds,
                                              const math::Mat4& view_projection,
                                              math::Vec2 framebuffer_size,
                                              math::Vec2& out_min,
                                              math::Vec2& out_max,
                                              f32& out_depth) noexcept {
        const std::array<math::Vec3, 8> corners{{
            {bounds.min.x, bounds.min.y, bounds.min.z},
            {bounds.max.x, bounds.min.y, bounds.min.z},
            {bounds.min.x, bounds.max.y, bounds.min.z},
            {bounds.max.x, bounds.max.y, bounds.min.z},
            {bounds.min.x, bounds.min.y, bounds.max.z},
            {bounds.max.x, bounds.min.y, bounds.max.z},
            {bounds.min.x, bounds.max.y, bounds.max.z},
            {bounds.max.x, bounds.max.y, bounds.max.z},
        }};

        math::Vec2 rect_min{std::numeric_limits<f32>::infinity(),
                            std::numeric_limits<f32>::infinity()};
        math::Vec2 rect_max{-std::numeric_limits<f32>::infinity(),
                            -std::numeric_limits<f32>::infinity()};
        f32 depth_sum = 0.0f;
        u32 projected = 0u;
        for (const math::Vec3& corner : corners) {
            math::Vec2 screen{};
            f32 depth = 0.0f;
            if (!project_to_screen_unclipped(corner, view_projection, framebuffer_size, screen, depth))
                continue;
            rect_min.x = std::min(rect_min.x, screen.x);
            rect_min.y = std::min(rect_min.y, screen.y);
            rect_max.x = std::max(rect_max.x, screen.x);
            rect_max.y = std::max(rect_max.y, screen.y);
            depth_sum += depth;
            ++projected;
        }
        if (projected == 0u)
            return false;

        out_min = rect_min;
        out_max = rect_max;
        out_depth = depth_sum / static_cast<f32>(projected);
        return true;
    }

    struct ViewportPickRay {
        math::Vec3 origin{};
        math::Vec3 dir{0.0f, 0.0f, -1.0f};
    };

    static bool unproject_ndc(const math::Mat4& inv_view_projection,
                              f32 ndc_x,
                              f32 ndc_y,
                              f32 ndc_z,
                              math::Vec3& out) noexcept {
        const math::Vec4 world =
            math::mul(inv_view_projection, math::Vec4{ndc_x, ndc_y, ndc_z, 1.0f});
        if (std::fabs(world.w) <= 0.000001f)
            return false;
        const f32 inv_w = 1.0f / world.w;
        out = {world.x * inv_w, world.y * inv_w, world.z * inv_w};
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }

    static std::optional<ViewportPickRay> viewport_pick_ray(
        const scene::SceneCameraView& camera,
        const platform::MouseState& mouse,
        math::Vec2 framebuffer_size) noexcept {
        if (framebuffer_size.x <= 0.0f || framebuffer_size.y <= 0.0f)
            return std::nullopt;

        const f32 ndc_x = (mouse.x / framebuffer_size.x) * 2.0f - 1.0f;
        const f32 ndc_y = 1.0f - (mouse.y / framebuffer_size.y) * 2.0f;
        const math::Mat4 view_projection = math::mul(camera.projection, camera.view);
        const f32 det = math::determinant(view_projection);
        if (std::fabs(det) <= 0.0000001f || !std::isfinite(det))
            return std::nullopt;
        const math::Mat4 inv_view_projection = math::inverse(view_projection);

        math::Vec3 near_world{};
        math::Vec3 far_world{};
        if (!unproject_ndc(inv_view_projection, ndc_x, ndc_y, -1.0f, near_world) ||
            !unproject_ndc(inv_view_projection, ndc_x, ndc_y, 1.0f, far_world)) {
            return std::nullopt;
        }

        const math::Vec3 delta = math::sub(far_world, near_world);
        const f32 len = math::length(delta);
        if (len <= 0.000001f || !std::isfinite(len))
            return std::nullopt;
        return ViewportPickRay{near_world, math::mul(delta, 1.0f / len)};
    }

    static bool ray_intersects_aabb(const ViewportPickRay& ray,
                                    const math::Aabb& bounds,
                                    f32& out_t) noexcept {
        f32 t_min = 0.0f;
        f32 t_max = std::numeric_limits<f32>::infinity();
        const f32 origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
        const f32 dir[3] = {ray.dir.x, ray.dir.y, ray.dir.z};
        const f32 mins[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
        const f32 maxs[3] = {bounds.max.x, bounds.max.y, bounds.max.z};

        for (u32 axis = 0u; axis < 3u; ++axis) {
            if (std::fabs(dir[axis]) <= 0.0000001f) {
                if (origin[axis] < mins[axis] || origin[axis] > maxs[axis])
                    return false;
                continue;
            }

            f32 t1 = (mins[axis] - origin[axis]) / dir[axis];
            f32 t2 = (maxs[axis] - origin[axis]) / dir[axis];
            if (t1 > t2)
                std::swap(t1, t2);
            t_min = std::max(t_min, t1);
            t_max = std::min(t_max, t2);
            if (t_min > t_max)
                return false;
        }

        out_t = t_min;
        return std::isfinite(out_t);
    }

    static f32 projected_bounds_pick_score(const math::Aabb& bounds,
                                           const math::Mat4& view_projection,
                                           const platform::MouseState& mouse,
                                           math::Vec2 framebuffer_size) noexcept {
        math::Vec2 rect_min{};
        math::Vec2 rect_max{};
        f32 depth = 0.0f;
        if (!project_bounds_to_screen_rect(bounds,
                                           view_projection,
                                           framebuffer_size,
                                           rect_min,
                                           rect_max,
                                           depth)) {
            return std::numeric_limits<f32>::infinity();
        }

        constexpr f32 kMinPickSize = 18.0f;
        constexpr f32 kPickPad = 8.0f;
        const f32 width = rect_max.x - rect_min.x;
        const f32 height = rect_max.y - rect_min.y;
        const f32 pad_x = std::max(kPickPad, (kMinPickSize - width) * 0.5f);
        const f32 pad_y = std::max(kPickPad, (kMinPickSize - height) * 0.5f);
        rect_min.x -= pad_x;
        rect_min.y -= pad_y;
        rect_max.x += pad_x;
        rect_max.y += pad_y;

        if (mouse.x < rect_min.x || mouse.x > rect_max.x || mouse.y < rect_min.y ||
            mouse.y > rect_max.y) {
            return std::numeric_limits<f32>::infinity();
        }

        const math::Vec2 center{(rect_min.x + rect_max.x) * 0.5f,
                                (rect_min.y + rect_max.y) * 0.5f};
        const f32 dx = center.x - mouse.x;
        const f32 dy = center.y - mouse.y;
        return std::sqrt(dx * dx + dy * dy) + depth * 0.04f;
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

    std::optional<Entity> pick_scene_entity_at(scene::Scene& scene,
                                               const scene::SceneCameraView& camera,
                                               const platform::MouseState& mouse,
                                               math::Vec2 framebuffer_size) {
        const math::Mat4 view_projection = math::mul(camera.projection, camera.view);
        const std::optional<ViewportPickRay> ray = viewport_pick_ray(camera, mouse, framebuffer_size);
        pick_render_items.clear();
        scene.gather_render_items(pick_render_items);

        std::optional<Entity> best_ray_entity;
        f32 best_ray_t = std::numeric_limits<f32>::infinity();
        std::optional<Entity> best_projected_entity;
        f32 best_projected_score = std::numeric_limits<f32>::infinity();
        for (const scene::SceneRenderItem& item : pick_render_items) {
            if (!item.entity.valid() || math::is_empty(item.world_bounds))
                continue;

            if (ray) {
                f32 t = 0.0f;
                if (ray_intersects_aabb(*ray, item.world_bounds, t) && t < best_ray_t) {
                    best_ray_t = t;
                    best_ray_entity = item.entity;
                }
            }

            const f32 projected_score = projected_bounds_pick_score(item.world_bounds,
                                                                    view_projection,
                                                                    mouse,
                                                                    framebuffer_size);
            if (projected_score < best_projected_score) {
                best_projected_score = projected_score;
                best_projected_entity = item.entity;
            }
        }
        return best_ray_entity ? best_ray_entity : best_projected_entity;
    }

    void handle_viewport_selection_click(scene::Scene& scene,
                                         const scene::SceneCameraView& camera,
                                         const platform::MouseState& mouse,
                                         math::Vec2 framebuffer_size,
                                         bool gizmo_hot) {
        const bool left_pressed = mouse.left && !viewport_mouse_left_prev;
        viewport_mouse_left_prev = mouse.left;
        if (!left_pressed || gizmo_hot)
            return;

        if (const std::optional<Entity> picked =
                pick_scene_entity_at(scene, camera, mouse, framebuffer_size)) {
            editor::selection::select_scene_entity(&scene, *picked);
        } else {
            editor::selection::clear_selection();
        }
        editor::publish_web_scene_hierarchy(&scene);
    }

    void handle_editor_delete_key() {
        if (editor::current_mode() != editor::Mode::Edit || editor::overlays_capturing())
            return;
        const platform::Input* input = platform::input();
        if (!input || !input->key_pressed(platform::KeyCode::Delete))
            return;
        scene::Scene* scene = app ? app->active_scene() : nullptr;
        const Entity selected = editor::selection::selected_scene_entity();
        if (!scene || !selected.valid() || !scene->registry().alive(selected))
            return;
        finish_gizmo_drag(false);
        (void)delete_entity(selected);
    }

    void handle_editor_undo_redo_keys() {
        if (editor::current_mode() != editor::Mode::Edit || editor::overlays_capturing())
            return;
        const platform::Input* input = platform::input();
        if (!input)
            return;
        const bool command_down = input->key_down(platform::KeyCode::LeftCtrl) ||
                                  input->key_down(platform::KeyCode::RightCtrl) ||
                                  input->key_down(platform::KeyCode::LeftSuper) ||
                                  input->key_down(platform::KeyCode::RightSuper);
        if (!command_down)
            return;
        const bool shift_down = input->key_down(platform::KeyCode::LeftShift) ||
                                input->key_down(platform::KeyCode::RightShift);
        std::string label;
        if (input->key_pressed(platform::KeyCode::Z)) {
            if (shift_down)
                (void)redo_editor_command(label);
            else
                (void)undo_editor_command(label);
        } else if (input->key_pressed(platform::KeyCode::Y)) {
            (void)redo_editor_command(label);
        }
    }

    void draw_viewport_gizmo_with_undo(app::WindowFrameContextT<PlayerArgs>& ctx) {
        if (editor::current_mode() != editor::Mode::Edit || editor::overlays_capturing()) {
            finish_gizmo_drag(false);
            viewport_mouse_left_prev = false;
            return;
        }
        scene::Scene* scene = ctx.app.active_scene();
        if (!scene) {
            finish_gizmo_drag(false);
            viewport_mouse_left_prev = false;
            return;
        }

        scene::SceneCameraView camera{};
        if (!scene->active_camera_view(framebuffer_aspect(ctx.framebuffer), camera)) {
            finish_gizmo_drag(false);
            viewport_mouse_left_prev = false;
            return;
        }

        const platform::Input* input = platform::input();
        if (!input) {
            finish_gizmo_drag(false);
            viewport_mouse_left_prev = false;
            return;
        }
        const platform::MouseState mouse =
            platform::mouse_to_framebuffer_space(input->mouse(),
                                                 ctx.framebuffer.width,
                                                 ctx.framebuffer.height);
        const math::Vec2 framebuffer_size{static_cast<f32>(ctx.framebuffer.width),
                                          static_cast<f32>(ctx.framebuffer.height)};
        const math::Mat4 view_projection = math::mul(camera.projection, camera.view);
        const Entity selected = editor::selection::selected_scene_entity();
        const bool selected_alive = selected.valid() && scene->registry().alive(selected);
        // Gizmo mode switching: Tab cycles translate -> rotate -> scale; G/R/Y
        // bind directly. Only while not mid-drag so a live drag keeps its mode.
        if (!gizmo_state.drag.active) {
            using editor::viewport::GizmoMode;
            if (input->key_pressed(platform::KeyCode::Tab))
                editor::viewport::set_gizmo_mode(
                    gizmo_state, editor::viewport::next_gizmo_mode(gizmo_state.mode));
            else if (input->key_pressed(platform::KeyCode::G))
                editor::viewport::set_gizmo_mode(gizmo_state, GizmoMode::Translate);
            else if (input->key_pressed(platform::KeyCode::R))
                editor::viewport::set_gizmo_mode(gizmo_state, GizmoMode::Rotate);
            else if (input->key_pressed(platform::KeyCode::Y))
                editor::viewport::set_gizmo_mode(gizmo_state, GizmoMode::Scale);
        }
        ui::imm::begin_frame(ctx.framebuffer);
        ui::imm::set_input({mouse.x, mouse.y}, mouse.left);
        editor::viewport::GizmoResult result{};
        if (selected_alive) {
            result = editor::viewport::draw_apply_gizmo(
                editor::viewport::GizmoFrame{
                    .scene = scene,
                    .selected_entity = selected,
                    .view_projection = view_projection,
                    .mouse = mouse,
                    .framebuffer_size = framebuffer_size,
                    .mode = gizmo_state.mode,
                    .apply_transform = true,
                },
                &gizmo_state);
        } else {
            finish_gizmo_drag(false);
        }
        ui::imm::end_frame();

        handle_viewport_selection_click(*scene, camera, mouse, framebuffer_size, result.hot);

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
        // Tear the editor IPC server down deterministically, while Console /
        // Scene / the registries it touches are still alive. Relying on the
        // singleton's static-destruction ~Server() would join worker threads
        // only after other singletons may already be gone (UAF). Safe no-op if
        // the server was never started.
        editor::ipc::Server::Get().stop();
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

    static std::string_view save_material_texture_name(
        void*,
        render::MaterialId material,
        const render::MaterialDesc&) {
        static thread_local std::string name;
        name = editor::web_material_texture_name(material.raw);
        return name;
    }

    static std::string_view save_mesh_group_name(void*, Entity entity, scene::SceneNode) {
        static thread_local std::string name;
        name = editor::web_entity_label(entity);
        return name;
    }

    static render::MaterialDesc material_desc_from_scene_file(
        const scene::SceneFileMaterial& material_file) {
        render::MaterialDesc material{};
        material.albedo_rgba8 = material_file.albedo_rgba8;
        material.alpha_cutoff = material_file.alpha_cutoff;
        material.reflectivity = material_file.reflectivity;
        material.roughness = material_file.roughness;
        material.emissive = material_file.emissive;
        material.winding = material_file.winding;
        material.blend = material_file.blend;
        material.raster_shadow_mode = material_file.raster_shadow_mode;
        material.shadow_alpha = material_file.shadow_alpha;
        material.shadow_opacity = material_file.shadow_opacity;
        material.shadow_softness = material_file.shadow_softness;
        material.flags = material_file.flags;
        return material;
    }

    render::MeshId resolve_snapshot_mesh(std::string_view mesh_name) {
        if (!app)
            return {};
        render::RenderingSystem& renderer = app->rendering_system();
        if (mesh_name == "builtin.unit_cube")
            return renderer.builtin_mesh(render::BuiltInMesh::UnitCube);
        if (mesh_name == "builtin.textured_triangle")
            return renderer.builtin_mesh(render::BuiltInMesh::TexturedTriangle);
        if (mesh_name == "builtin.pyramid")
            return renderer.builtin_mesh(render::BuiltInMesh::Pyramid);
        if (mesh_name == "builtin.cone")
            return renderer.builtin_mesh(render::BuiltInMesh::Cone);
        if (mesh_name == "builtin.uv_sphere")
            return renderer.builtin_mesh(render::BuiltInMesh::UvSphere);
        if (mesh_name == "builtin.geodesic_sphere")
            return renderer.builtin_mesh(render::BuiltInMesh::GeodesicSphere);
        return {};
    }

    std::optional<SceneSnapshot> capture_scene_snapshot() {
        SceneSnapshot snapshot{};
        snapshot.selected_raw = editor::selection::selected_scene_entity_raw();
        snapshot.primitive_spawn_count = primitive_spawn_count;
        snapshot.show_idle_panel = show_idle_panel;
        snapshot.idle_status = idle_status;

        scene::Scene* scene = app ? app->active_scene() : nullptr;
        if (!scene)
            return snapshot;

        sync_web_labels_to_scene_names(scene);
        snapshot.has_scene = true;
        std::string error;
        scene::SceneFileSaveStats stats{};
        const scene::SceneFileSaveHooks hooks{
            .user = this,
            .mesh_name = &PlayerApp::save_mesh_name,
            .material_name = &PlayerApp::save_material_name,
            .material_base_color_texture_name = &PlayerApp::save_material_texture_name,
            .material_preset_name = nullptr,
            .mesh_instance_group_name = &PlayerApp::save_mesh_group_name,
        };
        if (!scene::save_scene_file(*scene, hooks, snapshot.bytes, &stats, &error)) {
            PSY_LOG_WARN("psynder_arcade: could not capture editor undo snapshot: {}", error);
            return std::nullopt;
        }
        return snapshot;
    }

    bool restore_scene_snapshot(const SceneSnapshot& snapshot) {
        if (!app)
            return false;

        finish_gizmo_drag(false);
        editor::viewport::cancel_gizmo_drag(gizmo_state);
        app->reset_scenes();
        scene::EcsRegistry::Get().clear();
        editor::clear_web_scene_authoring_state();
        load_target_scene = nullptr;
        gathered_lights.clear();
        pick_render_items.clear();
        authored_textures.clear();
        primitive_spawn_count = snapshot.primitive_spawn_count;
        show_idle_panel = snapshot.show_idle_panel;
        idle_status = snapshot.idle_status;

        if (!snapshot.has_scene) {
            editor::publish_web_scene_hierarchy(nullptr);
            return true;
        }

        scene::SceneFileView view{};
        std::string error;
        if (!scene::parse_scene_file(
                std::span<const u8>{snapshot.bytes.data(), snapshot.bytes.size()},
                view,
                &error)) {
            PSY_LOG_WARN("psynder_arcade: could not parse editor undo snapshot: {}", error);
            return false;
        }

        scene::Scene& scene = app->create_active_scene();
        scene.prewarm_capacity(scene::scene_file_prewarm_config(view));
        app->reserve_scene_capacity(static_cast<u32>(view.mesh_instances.size()), 1u);

        std::vector<scene::SceneMaterialBinding> material_bindings;
        material_bindings.reserve(view.materials.size());
        for (const scene::SceneFileMaterial& material_file : view.materials) {
            const std::string_view material_name =
                ::psynder::scene::scene_file_string(view, material_file.name_offset);
            if (material_name.empty())
                continue;
            const std::string_view texture_name =
                ::psynder::scene::scene_file_string(
                    view, material_file.base_color_texture_name_offset);
            render::MaterialDesc material_desc = material_desc_from_scene_file(material_file);
            material_desc.base_color = resolve_authoring_texture(texture_name);
            const render::MaterialId material =
                scene.materials().create(material_desc);
            editor::set_web_material_texture_name(material.raw, texture_name);
            material_bindings.push_back(scene::SceneMaterialBinding{
                .material_name = material_name,
                .material = material,
            });
        }

        std::vector<scene::SceneMeshBinding> mesh_bindings;
        mesh_bindings.reserve(view.mesh_instances.size());
        for (const scene::SceneFileMeshInstance& mesh_file : view.mesh_instances) {
            const std::string_view mesh_name =
                ::psynder::scene::scene_file_string(view, mesh_file.mesh_name_offset);
            if (mesh_name.empty())
                continue;
            const auto existing = std::find_if(mesh_bindings.begin(),
                                               mesh_bindings.end(),
                                               [&](const scene::SceneMeshBinding& binding) {
                                                   return binding.mesh_name == mesh_name;
                                               });
            if (existing != mesh_bindings.end())
                continue;
            const render::MeshId mesh = resolve_snapshot_mesh(mesh_name);
            if (mesh.valid()) {
                mesh_bindings.push_back(scene::SceneMeshBinding{
                    .mesh_name = mesh_name,
                    .mesh = mesh,
                    .material = {},
                });
            }
        }

        std::vector<Entity> mesh_entities(view.mesh_instances.size());
        scene::SceneFileInstantiateResult result{};
        const bool restore_deferred = scene.structural_deferred();
        scene.set_structural_deferred(false);
        result = ::psynder::scene::instantiate_scene_file(scene,
                                                          view,
                                                          mesh_bindings,
                                                          material_bindings,
                                                          mesh_entities);
        scene.set_structural_deferred(restore_deferred);
        if (result.missing_mesh_bindings != 0u || result.missing_material_bindings != 0u) {
            PSY_LOG_WARN("psynder_arcade: restored snapshot with {} missing mesh and {} missing material binding(s)",
                         result.missing_mesh_bindings,
                         result.missing_material_bindings);
        }

        sync_scene_names_to_web_labels(&scene);
        const Entity selected{snapshot.selected_raw};
        if (selected.valid() && scene.registry().alive(selected))
            editor::selection::select_scene_entity(&scene, selected);
        else
            editor::selection::clear_selection();
        editor::publish_web_scene_hierarchy(&scene);
        return true;
    }

    void push_snapshot_history_after(std::string label,
                                     const std::optional<SceneSnapshot>& before) {
        if (replaying_history || !before)
            return;
        std::optional<SceneSnapshot> after = capture_scene_snapshot();
        if (!after)
            return;
        edit_history.push_callback(
            std::move(label),
            [this, snapshot = *before]() {
                replaying_history = true;
                (void)restore_scene_snapshot(snapshot);
                replaying_history = false;
            },
            [this, snapshot = *after]() {
                replaying_history = true;
                (void)restore_scene_snapshot(snapshot);
                replaying_history = false;
            });
    }

    bool active_scene_matches_saved_checkpoint() {
        if (!has_saved_scene_checkpoint)
            return false;
        const std::optional<SceneSnapshot> snapshot = capture_scene_snapshot();
        if (!snapshot || !snapshot->has_scene)
            return saved_scene_checkpoint.empty();
        return snapshot->bytes.size() == saved_scene_checkpoint.size() &&
               std::equal(snapshot->bytes.begin(),
                          snapshot->bytes.end(),
                          saved_scene_checkpoint.begin());
    }

    void remember_active_scene_as_saved_checkpoint() {
        const std::optional<SceneSnapshot> snapshot = capture_scene_snapshot();
        if (!snapshot || !snapshot->has_scene) {
            saved_scene_checkpoint.clear();
            has_saved_scene_checkpoint = false;
            return;
        }
        saved_scene_checkpoint = snapshot->bytes;
        has_saved_scene_checkpoint = true;
    }

    void reset_active_authoring_scene() {
        app->reset_scenes();
        scene::EcsRegistry::Get().clear();
        editor::clear_web_scene_authoring_state();
        load_target_scene = nullptr;
        primitive_spawn_count = 0u;
        authored_textures.clear();
        saved_scene_checkpoint.clear();
        has_saved_scene_checkpoint = false;
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

void create_active_arcade_fps_template() {
    if (g_active_arcade)
        g_active_arcade->create_fps_template_scene();
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

Entity add_active_arcade_gameplay(std::string_view kind) {
    if (!g_active_arcade)
        return {};
    return g_active_arcade->add_gameplay_entity(kind);
}

bool add_active_arcade_rigid_body(bool make_static) {
    return g_active_arcade && g_active_arcade->add_rigid_body_to_selected(make_static);
}

bool set_active_arcade_material_color(u32 rgba8) {
    return g_active_arcade && g_active_arcade->set_selected_material_color(rgba8);
}

bool set_active_arcade_material_texture(std::string_view name) {
    return g_active_arcade && g_active_arcade->set_selected_material_texture(name);
}

bool add_active_arcade_vehicle() {
    return g_active_arcade && g_active_arcade->add_vehicle_to_selected();
}

bool add_active_arcade_helicopter() {
    return g_active_arcade && g_active_arcade->add_helicopter_to_selected();
}

bool add_active_arcade_rigid_body_to(Entity entity, bool make_static) {
    return g_active_arcade && g_active_arcade->add_rigid_body_to_entity(entity, make_static);
}

bool add_active_arcade_vehicle_to(Entity entity) {
    return g_active_arcade && g_active_arcade->add_vehicle_to_entity(entity);
}

bool add_active_arcade_helicopter_to(Entity entity) {
    return g_active_arcade && g_active_arcade->add_helicopter_to_entity(entity);
}

void set_active_arcade_rt_mode(bool on) {
    if (g_active_arcade)
        g_active_arcade->set_rt_mode(on);
}

bool active_arcade_rt_mode() {
    return g_active_arcade && g_active_arcade->rt_mode();
}

bool bake_active_arcade_lightmaps() {
    return g_active_arcade && g_active_arcade->bake_scene_lightmaps();
}

bool world_new_active_arcade_indoor() {
    return g_active_arcade && g_active_arcade->world_new_indoor();
}

bool world_new_active_arcade_terrain() {
    return g_active_arcade && g_active_arcade->world_new_terrain();
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

void apply_active_arcade_component_edit(const editor::ipc::SelectionComponentEdit& edit) {
    const std::string value = component_edit_value_text(edit.value);
    const Entity entity{edit.entity_id};
    const bool ok =
        set_active_arcade_component_field(entity, edit.component, edit.field, value);
    std::string text;
    if (ok) {
        text = "applied ";
        text += edit.component;
        text += ".";
        text += edit.field;
    } else {
        text = "component edit failed for ";
        text += edit.component;
        text += ".";
        text += edit.field;
        PSY_LOG_WARN("psynder_arcade: component edit failed for {}.{} on {}",
                     edit.component,
                     edit.field,
                     edit.entity_id);
    }
    editor::publish_web_selection_command_ack("component_edit",
                                              ok,
                                              text,
                                              entity,
                                              edit.component,
                                              edit.field);
}

// Inspector "Add Component" intent. Routes the requested component (with an
// optional variant) to the matching host entry point, targeting the entity the
// client picked (carried as `add.entity_id` over the wire). This runs on the
// engine main thread via Server::pump(), so the structural ECS mutation is safe
// against the per-frame registry iteration. The target entity is validated
// alive at apply time inside the host add_*_to_entity functions; if it died
// between the client click and this dispatch the op no-ops and the ack reports
// the actual (now-dead) target so the panel can resync.
void apply_active_arcade_component_add(const editor::ipc::SelectionComponentAdd& add) {
    const Entity entity{add.entity_id};
    bool ok = false;
    std::string text;
    const bool is_rigid_body =
        add.component == "RigidBody" || add.component == "RigidBodyComponent" ||
        add.component == "RigidBodyStatic";
    const bool is_vehicle =
        add.component == "Vehicle" || add.component == "VehicleComponent";
    const bool is_helicopter =
        add.component == "Helicopter" || add.component == "HelicopterComponent";
    if (is_rigid_body) {
        const bool make_static =
            add.component == "RigidBodyStatic" || add.variant == "static";
        ok = add_active_arcade_rigid_body_to(entity, make_static);
        if (ok) {
            text = make_static ? "added static RigidBody" : "added dynamic RigidBody";
        } else {
            text = "add RigidBody failed (entity not alive / no active scene)";
            PSY_LOG_WARN("psynder_arcade: add RigidBody failed on {}", add.entity_id);
        }
    } else if (is_vehicle) {
        ok = add_active_arcade_vehicle_to(entity);
        if (ok) {
            text = "added Vehicle";
        } else {
            text = "add Vehicle failed (entity not alive / no active scene)";
            PSY_LOG_WARN("psynder_arcade: add Vehicle failed on {}", add.entity_id);
        }
    } else if (is_helicopter) {
        ok = add_active_arcade_helicopter_to(entity);
        if (ok) {
            text = "added Helicopter";
        } else {
            text = "add Helicopter failed (entity not alive / no active scene)";
            PSY_LOG_WARN("psynder_arcade: add Helicopter failed on {}", add.entity_id);
        }
    } else {
        text = "add component not supported: ";
        text += add.component;
        PSY_LOG_WARN("psynder_arcade: unsupported add_component '{}'", add.component);
    }
    editor::publish_web_selection_command_ack("add_component",
                                              ok,
                                              text,
                                              entity,
                                              add.component);
}

bool apply_active_arcade_material_preset(Entity entity, std::string_view preset) {
    return g_active_arcade && g_active_arcade->apply_material_preset(entity, preset);
}

bool apply_active_arcade_material_texture(Entity entity, std::string_view texture_name) {
    return g_active_arcade && g_active_arcade->apply_material_texture(entity, texture_name);
}

bool apply_active_arcade_material_texture_to_selection(std::string_view texture_name) {
    return g_active_arcade && g_active_arcade->apply_material_texture_to_selection(texture_name);
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
