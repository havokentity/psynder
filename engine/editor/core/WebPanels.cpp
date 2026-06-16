// SPDX-License-Identifier: MIT
// Psynder editor web-panel bridge.

#include "WebPanels.h"

#include "core/console/Console.h"
#include "editor/core/Editor.h"
#include "editor/core/Selection.h"
#include "editor/ipc/Ipc.h"
#include "editor/ipc/internal/Msgpack.h"
#include "editor/ipc/proto/Protocol.gen.h"
#include "asset/Vault.h"
#include "math/MathExt.h"
#include "platform/Platform.h"
#include "scene/GameplayComponents.h"
#include "scene/PhysicsComponents.h"
#include "scene/SceneEcs.h"
#include "scene/TrackComponent.h"
#include "ui/console/ConsoleOverlay.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace psynder::editor {
namespace {

struct HierarchyEntry {
    u32 id = 0;
    u32 parent = 0;
    std::string label;
    std::string_view kind = "entity";
    bool visible = true;
    u32 component_count = 0;
};

std::atomic<u32>& selected_entity_raw() noexcept {
    static std::atomic<u32> selected{0};
    return selected;
}

std::atomic<u32>& selected_entity_generation() noexcept {
    static std::atomic<u32> generation{0};
    return generation;
}

void bump_selected_entity_generation() noexcept {
    selected_entity_generation().fetch_add(1u, std::memory_order_acq_rel);
}

void clear_web_selection_mirror() noexcept {
    selected_entity_raw().store(0u, std::memory_order_release);
    selection::clear_selection();
    bump_selected_entity_generation();
}

std::unordered_map<u32, std::string>& entity_labels() {
    static std::unordered_map<u32, std::string> labels;
    return labels;
}

std::unordered_map<u32, std::string>& material_texture_names() {
    static std::unordered_map<u32, std::string> names;
    return names;
}

std::atomic<u32>& hierarchy_generation() noexcept {
    static std::atomic<u32> generation{0};
    return generation;
}

std::atomic<bool>& scene_dirty_flag() noexcept {
    static std::atomic<bool> dirty{false};
    return dirty;
}

std::atomic<u32>& scene_dirty_generation() noexcept {
    static std::atomic<u32> generation{0};
    return generation;
}

void on_web_selection_select(u32 entity_id) {
    selected_entity_raw().store(entity_id, std::memory_order_release);
    selection::mirror_scene_entity_raw(entity_id);
    bump_selected_entity_generation();
}

u64 hierarchy_hash_combine(u64 seed, u64 value) noexcept {
    seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6u) + (seed >> 2u);
    return seed;
}

u64 hash_f32(u64 seed, f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return hierarchy_hash_combine(seed, bits);
}

u64 hierarchy_signature(std::span<const HierarchyEntry> entries) noexcept {
    u64 out = 0xC0FFEE123456789ull;
    for (const HierarchyEntry& entry : entries) {
        out = hierarchy_hash_combine(out, entry.id);
        out = hierarchy_hash_combine(out, entry.parent);
        out = hierarchy_hash_combine(out, entry.component_count);
        out = hierarchy_hash_combine(out, entry.visible ? 1u : 0u);
        for (const char ch : entry.kind)
            out = hierarchy_hash_combine(out, static_cast<u8>(ch));
        for (const char ch : entry.label)
            out = hierarchy_hash_combine(out, static_cast<u8>(ch));
    }
    return out;
}

std::string hierarchy_label_for(scene::Scene& scene, Entity entity) {
    const auto label_it = entity_labels().find(entity.raw);
    if (label_it != entity_labels().end())
        return label_it->second;
    if (const std::string_view scene_name = scene.entity_name(entity); !scene_name.empty())
        return std::string{scene_name};
    const bool has_camera = scene.registry().get<scene::CameraComponent>(entity) != nullptr;
    const bool has_renderable = scene.registry().get<scene::RenderableComponent>(entity) != nullptr;
    std::string label = has_camera ? "Camera" : (has_renderable ? "Renderable" : "Entity");
    label += " #";
    label += std::to_string(entity.index());
    return label;
}

std::string_view hierarchy_kind_for(scene::Scene& scene, Entity entity) noexcept {
    if (scene.registry().get<scene::LightComponent>(entity))
        return "light";
    if (scene.registry().get<scene::CameraComponent>(entity))
        return "camera";
    if (scene.registry().get<scene::RenderableComponent>(entity))
        return "entity";
    return "group";
}

std::vector<HierarchyEntry> snapshot_scene_hierarchy(scene::Scene* scene) {
    std::vector<HierarchyEntry> entries;
    entries.push_back(HierarchyEntry{
        .id = 0,
        .parent = 0,
        .label = scene ? std::string{"Arcade Scene"} : std::string{"No Scene Loaded"},
        .kind = "root",
        .visible = true,
        .component_count = 0,
    });
    if (!scene)
        return entries;

    auto& registry = scene->registry();
    std::vector<Entity> entities(registry.snapshot_live_entities({}));
    const u32 total = registry.snapshot_live_entities(entities);
    if (total > entities.size()) {
        entities.resize(total);
        registry.snapshot_live_entities(entities);
    }
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.raw < b.raw; });

    std::unordered_map<u32, u32> entity_by_node;
    entity_by_node.reserve(entities.size());
    for (Entity entity : entities) {
        const scene::SceneNode node = scene->node(entity);
        if (node.valid())
            entity_by_node[node.raw] = entity.raw;
    }

    entries.reserve(entities.size() + 1u);
    for (Entity entity : entities) {
        const auto* node_component = registry.get<scene::SceneNodeComponent>(entity);
        if (!node_component || !scene->graph().alive(node_component->node))
            continue;

        u32 parent_id = 0;
        const scene::SceneNode parent_node = scene->graph().parent(node_component->node);
        if (parent_node.valid()) {
            const auto found = entity_by_node.find(parent_node.raw);
            if (found != entity_by_node.end())
                parent_id = found->second;
        }

        bool visible = true;
        if (const auto* renderable = registry.get<scene::RenderableComponent>(entity))
            visible = scene::renderable_is_visible(*renderable);

        entries.push_back(HierarchyEntry{
            .id = entity.raw,
            .parent = parent_id,
            .label = hierarchy_label_for(*scene, entity),
            .kind = hierarchy_kind_for(*scene, entity),
            .visible = visible,
            .component_count = registry.component_count(entity),
        });
    }
    return entries;
}

std::vector<u8> encode_hierarchy_envelope(std::span<const HierarchyEntry> entries) {
    ipc::msgpack::Writer w;
    w.map_header(4);
    w.str("v");
    w.u32_(ipc::proto::kProtocolVersion);
    w.str("ch");
    w.str(ipc::proto::channels::kscene);
    w.str("type");
    w.str("hierarchy");
    w.str("payload");
    w.map_header(2);
    w.str("entity_count");
    w.u32_(entries.empty() ? 0u : static_cast<u32>(entries.size() - 1u));
    w.str("nodes");
    w.array_header(entries.size());
    for (const HierarchyEntry& entry : entries) {
        w.map_header(6);
        w.str("id");
        w.u32_(entry.id);
        w.str("parent");
        if (entry.id == 0)
            w.nil();
        else
            w.u32_(entry.parent);
        w.str("label");
        w.str(entry.label);
        w.str("kind");
        w.str(entry.kind);
        w.str("visible");
        w.boolean(entry.visible);
        w.str("component_count");
        w.u32_(entry.component_count);
    }
    return w.buffer();
}

std::vector<u8> encode_dirty_state_envelope(WebSceneDirtyState state) {
    ipc::msgpack::Writer w;
    w.map_header(4);
    w.str("v");
    w.u32_(ipc::proto::kProtocolVersion);
    w.str("ch");
    w.str(ipc::proto::channels::kscene);
    w.str("type");
    w.str("dirty_state");
    w.str("payload");
    w.map_header(2);
    w.str("dirty");
    w.boolean(state.dirty);
    w.str("generation");
    w.u32_(state.generation);
    return w.buffer();
}

std::vector<u8> encode_scene_load_failed_envelope(std::string_view path, std::string_view error) {
    ipc::msgpack::Writer w;
    w.map_header(4);
    w.str("v");
    w.u32_(ipc::proto::kProtocolVersion);
    w.str("ch");
    w.str(ipc::proto::channels::kscene);
    w.str("type");
    w.str("load_failed");
    w.str("payload");
    w.map_header(4);
    w.str("path");
    w.str(path);
    w.str("error");
    w.str(error);
    w.str("message");
    w.str(error);
    w.str("text");
    w.str(error);
    return w.buffer();
}

std::vector<u8> encode_selection_command_ack_envelope(std::string_view command,
                                                      bool ok,
                                                      std::string_view text,
                                                      Entity entity,
                                                      std::string_view component,
                                                      std::string_view field) {
    ipc::msgpack::Writer w;
    w.map_header(4);
    w.str("v");
    w.u32_(ipc::proto::kProtocolVersion);
    w.str("ch");
    w.str(ipc::proto::channels::kselection);
    w.str("type");
    w.str("command_ack");
    w.str("payload");
    w.map_header(9);
    w.str("command");
    w.str(command);
    w.str("ok");
    w.boolean(ok);
    w.str("text");
    w.str(text);
    w.str("value_kind");
    w.str("text");
    w.str("output");
    w.str(ok ? text : std::string_view{});
    w.str("error");
    w.str(ok ? std::string_view{} : text);
    w.str("entity_id");
    if (entity.valid() || entity.raw == 0u)
        w.u32_(entity.raw);
    else
        w.nil();
    w.str("component");
    if (component.empty())
        w.nil();
    else
        w.str(component);
    w.str("field");
    if (field.empty())
        w.nil();
    else
        w.str(field);
    return w.buffer();
}

void write_numeric_hint(ipc::msgpack::Writer& w,
                        f32 step,
                        std::string_view unit = {},
                        std::optional<f32> min = {},
                        std::optional<f32> max = {}) {
    usize count = 1u + (unit.empty() ? 0u : 1u) + (min ? 1u : 0u) + (max ? 1u : 0u);
    w.map_header(count);
    w.str("step");
    w.f32_(step);
    if (!unit.empty()) {
        w.str("unit");
        w.str(unit);
    }
    if (min) {
        w.str("min");
        w.f32_(*min);
    }
    if (max) {
        w.str("max");
        w.f32_(*max);
    }
}

struct EnumOption {
    std::string_view label;
    u32 value = 0;
};

void write_enum_hint(ipc::msgpack::Writer& w, std::span<const EnumOption> options) {
    w.map_header(1);
    w.str("options");
    w.array_header(options.size());
    for (const EnumOption& option : options) {
        w.map_header(2);
        w.str("label");
        w.str(option.label);
        w.str("value");
        w.u32_(option.value);
    }
}

void write_field_schema(ipc::msgpack::Writer& w,
                        std::string_view name,
                        std::string_view kind,
                        bool readonly = true,
                        std::optional<f32> step = {},
                        std::string_view unit = {},
                        std::span<const EnumOption> enum_options = {}) {
    const bool has_numeric = step.has_value() || !unit.empty();
    const bool has_enum = !enum_options.empty();
    w.map_header(3u + (has_numeric ? 1u : 0u) + (has_enum ? 1u : 0u));
    w.str("name");
    w.str(name);
    w.str("kind");
    w.str(kind);
    w.str("readonly");
    w.boolean(readonly);
    if (has_numeric) {
        w.str("numeric");
        write_numeric_hint(w, step.value_or(0.01f), unit);
    }
    if (has_enum) {
        w.str("enum");
        write_enum_hint(w, enum_options);
    }
}

void write_component_schema_header(ipc::msgpack::Writer& w,
                                   std::string_view name,
                                   std::string_view hash,
                                   usize field_count) {
    w.map_header(3);
    w.str("name");
    w.str(name);
    w.str("layout_hash");
    w.str(hash);
    w.str("fields");
    w.array_header(field_count);
}

std::vector<u8> encode_schema_catalog_envelope() {
    ipc::msgpack::Writer w;
    w.map_header(4);
    w.str("v");
    w.u32_(ipc::proto::kProtocolVersion);
    w.str("ch");
    w.str(ipc::proto::channels::kschemas);
    w.str("type");
    w.str("catalog");
    w.str("payload");
    w.map_header(1);
    w.str("components");
    w.array_header(23);

    write_component_schema_header(w, "EnvironmentComponent", "native-environment-v1", 10);
    write_field_schema(w, "clear_color_rgba8", "color", false);
    write_field_schema(w, "clear_color", "bool", false);
    write_field_schema(w, "clear_depth", "bool", false);
    write_field_schema(w, "sky_zenith_rgba8", "color", false);
    write_field_schema(w, "sky_horizon_rgba8", "color", false);
    write_field_schema(w, "sky_intensity", "f32", false, 0.01f);
    write_field_schema(w, "cloud_enabled", "bool", false);
    write_field_schema(w, "cloud_coverage", "f32", false, 0.01f);
    write_field_schema(w, "cloud_density", "f32", false, 0.01f);
    write_field_schema(w, "cloud_height", "f32", false, 1.0f, "m");

    // Scene-level render settings. Published on the synthetic scene root
    // (entity 0, "Arcade Scene") alongside EnvironmentComponent; edits route
    // back through the component_edit path (RenderSettingsComponent branch in
    // the host apply_component_field) on the main thread.
    constexpr std::array<EnumOption, 3> kRenderModeOptions{{
        {"Raster", static_cast<u32>(scene::RenderMode::Raster)},
        {"Raytraced", static_cast<u32>(scene::RenderMode::Raytraced)},
        {"Hybrid", static_cast<u32>(scene::RenderMode::Hybrid)},
    }};
    write_component_schema_header(w, "RenderSettingsComponent", "native-render-settings-v1", 14);
    write_field_schema(w, "render_mode", "enum", false, {}, {}, kRenderModeOptions);
    write_field_schema(w, "sun_enabled", "bool", false);
    write_field_schema(w, "sun_direction", "vec3", false, 0.01f);
    write_field_schema(w, "sun_color_rgba8", "color", false);
    write_field_schema(w, "sun_intensity", "f32", false, 0.05f);
    write_field_schema(w, "ambient_color_rgba8", "color", false);
    write_field_schema(w, "ambient_intensity", "f32", false, 0.05f);
    write_field_schema(w, "shadows_enabled", "bool", false);
    write_field_schema(w, "shadow_softness", "f32", false, 0.01f);
    write_field_schema(w, "shadow_opacity", "f32", false, 0.01f);
    write_field_schema(w, "rt_trace_downscale", "u32", false, 1.0f);
    write_field_schema(w, "rt_ao", "u32", false, 1.0f);
    write_field_schema(w, "rt_reflection_bounces", "u32", false, 1.0f);
    write_field_schema(w, "rt_samples", "u32", false, 1.0f);

    write_component_schema_header(w, "TransformComponent", "native-transform-v2", 3);
    write_field_schema(w, "translation", "vec3", false, 0.01f, "m");
    write_field_schema(w, "rotation", "vec3", false, 0.1f, "deg");
    write_field_schema(w, "scale", "vec3", false, 0.01f);

    write_component_schema_header(w, "SceneNodeComponent", "native-scene-node-v1", 2);
    write_field_schema(w, "node_raw", "u32");
    write_field_schema(w, "parent_entity", "u32");

    write_component_schema_header(w, "CameraComponent", "native-camera-v1", 7);
    write_field_schema(w, "fov_y_deg", "f32", false, 0.1f, "deg");
    write_field_schema(w, "aspect", "f32", false, 0.01f);
    write_field_schema(w, "near_z", "f32", false, 0.01f, "m");
    write_field_schema(w, "far_z", "f32", false, 1.0f, "m");
    write_field_schema(w, "tile_w", "u32");
    write_field_schema(w, "tile_h", "u32");
    write_field_schema(w, "active", "bool", false);

    constexpr std::array<EnumOption, 3> kLightOptions{{
        {"Point", static_cast<u32>(scene::LightKind::Point)},
        {"Directional", static_cast<u32>(scene::LightKind::Directional)},
        {"Spot", static_cast<u32>(scene::LightKind::Spot)},
    }};
    write_component_schema_header(w, "LightComponent", "native-light-v1", 7);
    write_field_schema(w, "kind", "enum", false, {}, {}, kLightOptions);
    write_field_schema(w, "color_rgba8", "color", false);
    write_field_schema(w, "intensity", "f32", false, 0.05f);
    write_field_schema(w, "range", "f32", false, 0.1f, "m");
    write_field_schema(w, "inner_cone_deg", "f32", false, 0.5f, "deg");
    write_field_schema(w, "outer_cone_deg", "f32", false, 0.5f, "deg");
    write_field_schema(w, "casts_shadow", "bool", false);

    constexpr std::array<EnumOption, 3> kGeometryOptions{{
        {"None", static_cast<u32>(scene::GeometryKind::None)},
        {"Mesh", static_cast<u32>(scene::GeometryKind::Mesh)},
        {"AnalyticSphere", static_cast<u32>(scene::GeometryKind::AnalyticSphere)},
    }};
    constexpr std::array<EnumOption, 2> kMobilityOptions{{
        {"Static", static_cast<u32>(scene::ObjectMobility::Static)},
        {"Dynamic", static_cast<u32>(scene::ObjectMobility::Dynamic)},
    }};
    write_component_schema_header(w, "RenderableComponent", "native-renderable-v1", 8);
    write_field_schema(w, "geometry", "enum", true, {}, {}, kGeometryOptions);
    write_field_schema(w, "geometry_id", "u32");
    write_field_schema(w, "material", "u32");
    write_field_schema(w, "visible", "bool", false);
    write_field_schema(w, "mobility", "enum", true, {}, {}, kMobilityOptions);
    write_field_schema(w, "local_bounds_min", "vec3", true, 0.01f, "m");
    write_field_schema(w, "local_bounds_max", "vec3", true, 0.01f, "m");
    write_field_schema(w, "casts_shadow_override", "bool");

    constexpr std::array<EnumOption, 3> kBlendOptions{{
        {"Opaque", static_cast<u32>(render::MaterialBlendMode::Opaque)},
        {"AlphaTest", static_cast<u32>(render::MaterialBlendMode::AlphaTest)},
        {"AlphaBlend", static_cast<u32>(render::MaterialBlendMode::AlphaBlend)},
    }};
    write_component_schema_header(w, "MaterialComponent", "native-material-v2", 9);
    write_field_schema(w, "albedo_rgba8", "color", false);
    write_field_schema(w, "base_color_texture_name", "string", false);
    write_field_schema(w, "reflectivity", "f32", false, 0.01f);
    write_field_schema(w, "roughness", "f32", false, 0.01f);
    write_field_schema(w, "emissive", "f32", false, 0.01f);
    write_field_schema(w, "alpha_cutoff", "f32", false, 0.01f);
    write_field_schema(w, "blend", "enum", false, {}, {}, kBlendOptions);
    write_field_schema(w, "shadow_opacity", "f32", false, 0.01f);
    write_field_schema(w, "shadow_softness", "f32", false, 0.01f);

    constexpr std::array<EnumOption, 7> kGameplayRoleOptions{{
        {"None", static_cast<u32>(scene::GameplayRole::None)},
        {"PlayerStart", static_cast<u32>(scene::GameplayRole::PlayerStart)},
        {"PlayerController", static_cast<u32>(scene::GameplayRole::PlayerController)},
        {"Enemy", static_cast<u32>(scene::GameplayRole::Enemy)},
        {"Pickup", static_cast<u32>(scene::GameplayRole::Pickup)},
        {"Trigger", static_cast<u32>(scene::GameplayRole::Trigger)},
        {"Door", static_cast<u32>(scene::GameplayRole::Door)},
    }};
    write_component_schema_header(w, "GameplayTagComponent", "native-gameplay-tag-v1", 2);
    write_field_schema(w, "role", "enum", false, {}, {}, kGameplayRoleOptions);
    write_field_schema(w, "flags", "u32");

    write_component_schema_header(w, "PlayerControllerComponent", "native-player-controller-v1", 6);
    write_field_schema(w, "walk_speed", "f32", false, 0.05f, "m/s");
    write_field_schema(w, "run_speed", "f32", false, 0.05f, "m/s");
    write_field_schema(w, "jump_speed", "f32", false, 0.05f, "m/s");
    write_field_schema(w, "mouse_sensitivity", "f32", false, 0.01f);
    write_field_schema(w, "height", "f32", false, 0.01f, "m");
    write_field_schema(w, "radius", "f32", false, 0.01f, "m");

    write_component_schema_header(w, "HealthComponent", "native-health-v1", 3);
    write_field_schema(w, "max_health", "f32", false, 1.0f);
    write_field_schema(w, "current_health", "f32", false, 1.0f);
    write_field_schema(w, "faction", "u32");

    write_component_schema_header(w, "WeaponComponent", "native-weapon-v1", 5);
    write_field_schema(w, "damage", "f32", false, 1.0f);
    write_field_schema(w, "range", "f32", false, 0.5f, "m");
    write_field_schema(w, "fire_rate", "f32", false, 0.1f, "hz");
    write_field_schema(w, "ammo", "u32");
    write_field_schema(w, "automatic", "bool", false);

    // ── Physics authoring components (#60a). Editable AUTHORING fields only;
    // every runtime_* opaque physics handle is published RUNTIME-only/read-only
    // and is never user-editable (apply_component_field rejects it).
    constexpr std::array<EnumOption, 8> kColliderShapeOptions{{
        {"Sphere", static_cast<u32>(scene::ColliderShape::Sphere)},
        {"Capsule", static_cast<u32>(scene::ColliderShape::Capsule)},
        {"Box", static_cast<u32>(scene::ColliderShape::Box)},
        {"ConvexHull", static_cast<u32>(scene::ColliderShape::ConvexHull)},
        {"Compound", static_cast<u32>(scene::ColliderShape::Compound)},
        {"Heightfield", static_cast<u32>(scene::ColliderShape::Heightfield)},
        {"TriangleMesh", static_cast<u32>(scene::ColliderShape::TriangleMesh)},
        {"Plane", static_cast<u32>(scene::ColliderShape::Plane)},
    }};
    write_component_schema_header(w, "RigidBodyComponent", "native-rigid-body-v1", 6);
    write_field_schema(w, "shape", "enum", false, {}, {}, kColliderShapeOptions);
    write_field_schema(w, "mass", "f32", false, 0.1f, "kg");
    write_field_schema(w, "half_extent", "vec3", false, 0.01f, "m");
    write_field_schema(w, "friction", "f32", false, 0.01f);
    write_field_schema(w, "restitution", "f32", false, 0.01f);
    write_field_schema(w, "runtime_body", "u32");

    constexpr std::array<EnumOption, 2> kVehicleGroundOptions{{
        {"Plane", static_cast<u32>(scene::VehicleGroundMode::Plane)},
        {"Heightfield", static_cast<u32>(scene::VehicleGroundMode::Heightfield)},
    }};
    write_component_schema_header(w, "VehicleComponent", "native-vehicle-v2", 20);
    write_field_schema(w, "half_extent", "vec3", false, 0.01f, "m");
    write_field_schema(w, "mass", "f32", false, 1.0f, "kg");
    write_field_schema(w, "engine_max_torque", "f32", false, 1.0f, "N.m");
    write_field_schema(w, "drag", "f32", false, 0.01f);
    write_field_schema(w, "wheel_radius", "f32", false, 0.01f, "m");
    write_field_schema(w, "suspension", "f32", false, 0.01f, "m");
    write_field_schema(w, "stiffness", "f32", false, 100.0f, "N/m");
    write_field_schema(w, "damping", "f32", false, 10.0f, "N.s/m");
    write_field_schema(w, "max_speed", "f32", false, 0.5f, "m/s");
    write_field_schema(w, "steer_full_speed", "f32", false, 0.5f, "m/s");
    write_field_schema(w, "steer_taper_speed", "f32", false, 0.5f, "m/s");
    write_field_schema(w, "steer_min_authority", "f32", false, 0.01f);
    write_field_schema(w, "ground_mode", "enum", false, {}, {}, kVehicleGroundOptions);
    write_field_schema(w, "plane_y", "f32", false, 0.05f, "m");
    write_field_schema(w, "hf_base_y", "f32", false, 0.05f, "m");
    write_field_schema(w, "hf_amplitude", "f32", false, 0.05f, "m");
    write_field_schema(w, "hf_frequency", "f32", false, 0.005f, "rad/m");
    write_field_schema(w, "is_player", "bool", false);
    write_field_schema(w, "runtime_vehicle", "u32");
    write_field_schema(w, "runtime_chassis", "u32");

    write_component_schema_header(w, "HelicopterComponent", "native-helicopter-v1", 10);
    write_field_schema(w, "half_extent", "vec3", false, 0.01f, "m");
    write_field_schema(w, "mass", "f32", false, 1.0f, "kg");
    write_field_schema(w, "max_thrust_n", "f32", false, 10.0f, "N");
    write_field_schema(w, "pitch_torque", "f32", false, 10.0f, "N.m");
    write_field_schema(w, "roll_torque", "f32", false, 10.0f, "N.m");
    write_field_schema(w, "yaw_torque", "f32", false, 10.0f, "N.m");
    write_field_schema(w, "angular_damping", "f32", false, 0.05f);
    write_field_schema(w, "hover_assist", "bool", false);
    write_field_schema(w, "is_player", "bool", false);
    write_field_schema(w, "runtime_body", "u32");

    write_component_schema_header(w, "CharacterControllerComponent", "native-character-controller-v1", 4);
    write_field_schema(w, "height", "f32", false, 0.01f, "m");
    write_field_schema(w, "radius", "f32", false, 0.01f, "m");
    write_field_schema(w, "move_speed", "f32", false, 0.05f, "m/s");
    write_field_schema(w, "runtime_character", "u32");

    // ── No-code gameplay/AI authoring components. Scene-level proxies that
    // PlayRuntime maps into the live combat/AI components when Play begins; all
    // fields are designer-editable authoring fields (no runtime handles).
    write_component_schema_header(w, "FactionComponent", "native-faction-v1", 1);
    write_field_schema(w, "faction", "u32");

    write_component_schema_header(w, "HitboxComponent", "native-hitbox-v1", 4);
    write_field_schema(w, "offset", "vec3", false, 0.01f, "m");
    write_field_schema(w, "half_extent", "vec3", false, 0.01f, "m");
    write_field_schema(w, "radius", "f32", false, 0.01f, "m");
    write_field_schema(w, "enabled", "bool", false);

    constexpr std::array<EnumOption, 2> kWeaponFireKindOptions{{
        {"Hitscan", static_cast<u32>(scene::WeaponFireKind::Hitscan)},
        {"Projectile", static_cast<u32>(scene::WeaponFireKind::Projectile)},
    }};
    write_component_schema_header(w, "WeaponModeComponent", "native-weapon-mode-v1", 3);
    write_field_schema(w, "kind", "enum", false, {}, {}, kWeaponFireKindOptions);
    write_field_schema(w, "projectile_speed", "f32", false, 0.5f, "m/s");
    write_field_schema(w, "projectile_life", "f32", false, 0.1f, "s");

    constexpr std::array<EnumOption, 5> kAiStateOptions{{
        {"Idle", static_cast<u32>(scene::AiState::Idle)},
        {"Patrol", static_cast<u32>(scene::AiState::Patrol)},
        {"Chase", static_cast<u32>(scene::AiState::Chase)},
        {"Attack", static_cast<u32>(scene::AiState::Attack)},
        {"Dead", static_cast<u32>(scene::AiState::Dead)},
    }};
    write_component_schema_header(w, "AiAgentComponent", "native-ai-agent-v1", 6);
    write_field_schema(w, "state", "enum", false, {}, {}, kAiStateOptions);
    write_field_schema(w, "sight_range", "f32", false, 0.5f, "m");
    write_field_schema(w, "fov_cos", "f32", false, 0.01f);
    write_field_schema(w, "attack_range", "f32", false, 0.5f, "m");
    write_field_schema(w, "think_interval", "f32", false, 0.01f, "s");
    write_field_schema(w, "move_speed", "f32", false, 0.05f, "m/s");

    // Perception is a tag-only proxy: presence marks the agent for a sense
    // snapshot. No editable fields, so the schema is empty.
    write_component_schema_header(w, "PerceptionComponent", "native-perception-v1", 0);

    write_component_schema_header(w, "PatrolComponent", "native-patrol-v1", 2);
    write_field_schema(w, "wait_time", "f32", false, 0.05f, "s");
    write_field_schema(w, "arrive_radius", "f32", false, 0.01f, "m");

    // Authored closed-loop race TRACK (Wave 11 racer DoD). The Bezier segment
    // geometry is bulk authoring data laid out by the track tool / public API, so
    // the Inspector exposes the segment_count read-only and lets the designer tune
    // the auto-driver (target speed / look-ahead / steer + throttle gains) and the
    // start/finish lap gate. PlayRuntime runs a track-follow driver over these so
    // a VehicleComponent on the same entity LAPS the loop with no bespoke racer C++.
    write_component_schema_header(w, "TrackComponent", "native-track-v1", 8);
    write_field_schema(w, "segment_count", "u32");
    write_field_schema(w, "target_speed", "f32", false, 0.5f, "m/s");
    write_field_schema(w, "look_ahead", "f32", false, 0.5f, "m");
    write_field_schema(w, "steer_gain", "f32", false, 0.01f);
    write_field_schema(w, "steer_clamp", "f32", false, 0.01f, "rad");
    write_field_schema(w, "throttle_kp", "f32", false, 0.01f);
    write_field_schema(w, "lap_gate_point", "vec3", false, 0.05f, "m");
    write_field_schema(w, "lap_gate_normal", "vec3", false, 0.01f);

    return w.buffer();
}

void write_vec3(ipc::msgpack::Writer& w, const math::Vec3& v) {
    w.array_header(3);
    w.f32_(v.x);
    w.f32_(v.y);
    w.f32_(v.z);
}

math::Vec3 rotation_degrees(math::Quat q) noexcept {
    const math::Vec3 euler = math::quat_to_euler(math::quat_normalize(q));
    return math::Vec3{
        euler.x * math::kRadToDeg,
        euler.y * math::kRadToDeg,
        euler.z * math::kRadToDeg,
    };
}

u32 parent_entity_for_node(scene::Scene& scene, const scene::SceneNode node) {
    const scene::SceneNode parent = scene.graph().parent(node);
    if (!parent.valid())
        return 0u;
    auto& registry = scene.registry();
    std::vector<Entity> entities(registry.snapshot_live_entities({}));
    registry.snapshot_live_entities(entities);
    for (const Entity entity : entities) {
        const scene::SceneNode candidate = scene.node(entity);
        if (candidate.raw == parent.raw)
            return entity.raw;
    }
    return 0u;
}

u64 selection_signature(scene::Scene& scene, Entity entity, u32 generation) {
    u64 out = hierarchy_hash_combine(0x515ECAFE1234ull, entity.raw);
    out = hierarchy_hash_combine(out, generation);
    if (!entity.valid()) {
        const scene::EnvironmentSettings& env = scene.environment().settings();
        out = hierarchy_hash_combine(out, env.clear_color ? 1u : 0u);
        out = hierarchy_hash_combine(out, env.clear_depth ? 1u : 0u);
        out = hierarchy_hash_combine(out, env.clear_color_rgba8);
        out = hierarchy_hash_combine(out, env.sky.zenith_rgba8);
        out = hierarchy_hash_combine(out, env.sky.horizon_rgba8);
        out = hash_f32(out, env.sky.intensity);
        out = hierarchy_hash_combine(out, env.clouds.enabled ? 1u : 0u);
        out = hash_f32(out, env.clouds.coverage);
        out = hash_f32(out, env.clouds.density);
        out = hash_f32(out, env.clouds.height);
        const scene::RenderSettings& rs = scene.render_settings();
        out = hierarchy_hash_combine(out, static_cast<u32>(rs.render_mode));
        out = hierarchy_hash_combine(out, rs.sun_enabled ? 1u : 0u);
        out = hash_f32(out, rs.sun_direction.x);
        out = hash_f32(out, rs.sun_direction.y);
        out = hash_f32(out, rs.sun_direction.z);
        out = hierarchy_hash_combine(out, rs.sun_color_rgba8);
        out = hash_f32(out, rs.sun_intensity);
        out = hierarchy_hash_combine(out, rs.ambient_color_rgba8);
        out = hash_f32(out, rs.ambient_intensity);
        out = hierarchy_hash_combine(out, rs.shadows_enabled ? 1u : 0u);
        out = hash_f32(out, rs.shadow_softness);
        out = hash_f32(out, rs.shadow_opacity);
        out = hierarchy_hash_combine(out, rs.rt_trace_downscale);
        out = hierarchy_hash_combine(out, rs.rt_ao);
        out = hierarchy_hash_combine(out, rs.rt_reflection_bounces);
        out = hierarchy_hash_combine(out, rs.rt_samples);
        return out;
    }

    auto& registry = scene.registry();
    if (auto* transform = registry.get<scene::TransformComponent>(entity)) {
        out = hash_f32(out, transform->local.translation.x);
        out = hash_f32(out, transform->local.translation.y);
        out = hash_f32(out, transform->local.translation.z);
        out = hash_f32(out, transform->local.rotation.x);
        out = hash_f32(out, transform->local.rotation.y);
        out = hash_f32(out, transform->local.rotation.z);
        out = hash_f32(out, transform->local.rotation.w);
        out = hash_f32(out, transform->local.scale.x);
        out = hash_f32(out, transform->local.scale.y);
        out = hash_f32(out, transform->local.scale.z);
    }
    if (auto* camera = registry.get<scene::CameraComponent>(entity)) {
        out = hash_f32(out, camera->fov_y_rad);
        out = hash_f32(out, camera->aspect);
        out = hash_f32(out, camera->near_z);
        out = hash_f32(out, camera->far_z);
        out = hierarchy_hash_combine(out, camera->tile_w);
        out = hierarchy_hash_combine(out, camera->tile_h);
        out = hierarchy_hash_combine(out, camera->active);
    }
    if (auto* light = registry.get<scene::LightComponent>(entity)) {
        out = hierarchy_hash_combine(out, static_cast<u32>(light->kind));
        out = hierarchy_hash_combine(out, light->color_rgba8);
        out = hash_f32(out, light->intensity);
        out = hash_f32(out, light->range);
        out = hash_f32(out, light->inner_cone_deg);
        out = hash_f32(out, light->outer_cone_deg);
        out = hierarchy_hash_combine(out, light->casts_shadow);
    }
    if (auto* renderable = registry.get<scene::RenderableComponent>(entity)) {
        out = hierarchy_hash_combine(out, static_cast<u32>(renderable->geometry));
        out = hierarchy_hash_combine(out, renderable->geometry_id);
        out = hierarchy_hash_combine(out, renderable->material.raw);
        out = hierarchy_hash_combine(out, scene::renderable_flags_bits(renderable->flags));
        const render::MaterialDesc material = scene.materials().get(renderable->material);
        out = hierarchy_hash_combine(out, material.albedo_rgba8);
        out = hash_f32(out, material.reflectivity);
        out = hash_f32(out, material.roughness);
        out = hash_f32(out, material.emissive);
        out = hierarchy_hash_combine(out, static_cast<u32>(material.blend));
        if (const auto it = material_texture_names().find(renderable->material.raw);
            it != material_texture_names().end()) {
            for (const char ch : it->second)
                out = hierarchy_hash_combine(out, static_cast<u8>(ch));
        }
    }
    if (auto* tag = registry.get<scene::GameplayTagComponent>(entity)) {
        out = hierarchy_hash_combine(out, static_cast<u32>(tag->role));
        out = hierarchy_hash_combine(out, tag->flags);
    }
    if (auto* controller = registry.get<scene::PlayerControllerComponent>(entity)) {
        out = hash_f32(out, controller->walk_speed);
        out = hash_f32(out, controller->run_speed);
        out = hash_f32(out, controller->jump_speed);
        out = hash_f32(out, controller->mouse_sensitivity);
        out = hash_f32(out, controller->height);
        out = hash_f32(out, controller->radius);
    }
    if (auto* health = registry.get<scene::HealthComponent>(entity)) {
        out = hash_f32(out, health->max_health);
        out = hash_f32(out, health->current_health);
        out = hierarchy_hash_combine(out, health->faction);
    }
    if (auto* weapon = registry.get<scene::WeaponComponent>(entity)) {
        out = hash_f32(out, weapon->damage);
        out = hash_f32(out, weapon->range);
        out = hash_f32(out, weapon->fire_rate);
        out = hierarchy_hash_combine(out, weapon->ammo);
        out = hierarchy_hash_combine(out, weapon->automatic);
    }
    if (auto* rigid_body = registry.get<scene::RigidBodyComponent>(entity)) {
        out = hierarchy_hash_combine(out, static_cast<u32>(rigid_body->shape));
        out = hash_f32(out, rigid_body->mass);
        out = hash_f32(out, rigid_body->half_extent.x);
        out = hash_f32(out, rigid_body->half_extent.y);
        out = hash_f32(out, rigid_body->half_extent.z);
        out = hash_f32(out, rigid_body->friction);
        out = hash_f32(out, rigid_body->restitution);
        out = hierarchy_hash_combine(out, rigid_body->runtime_body);
    }
    if (auto* vehicle = registry.get<scene::VehicleComponent>(entity)) {
        out = hash_f32(out, vehicle->half_extent.x);
        out = hash_f32(out, vehicle->half_extent.y);
        out = hash_f32(out, vehicle->half_extent.z);
        out = hash_f32(out, vehicle->mass);
        out = hash_f32(out, vehicle->engine_max_torque);
        out = hash_f32(out, vehicle->drag);
        out = hash_f32(out, vehicle->wheel_radius);
        out = hash_f32(out, vehicle->suspension);
        out = hash_f32(out, vehicle->stiffness);
        out = hash_f32(out, vehicle->damping);
        out = hash_f32(out, vehicle->max_speed);
        out = hash_f32(out, vehicle->steer_full_speed);
        out = hash_f32(out, vehicle->steer_taper_speed);
        out = hash_f32(out, vehicle->steer_min_authority);
        out = hierarchy_hash_combine(out, static_cast<u32>(vehicle->ground_mode));
        out = hash_f32(out, vehicle->plane_y);
        out = hash_f32(out, vehicle->hf_base_y);
        out = hash_f32(out, vehicle->hf_amplitude);
        out = hash_f32(out, vehicle->hf_frequency);
        out = hierarchy_hash_combine(out, vehicle->is_player);
        out = hierarchy_hash_combine(out, vehicle->runtime_vehicle);
        out = hierarchy_hash_combine(out, vehicle->runtime_chassis);
    }
    if (auto* helicopter = registry.get<scene::HelicopterComponent>(entity)) {
        out = hash_f32(out, helicopter->half_extent.x);
        out = hash_f32(out, helicopter->half_extent.y);
        out = hash_f32(out, helicopter->half_extent.z);
        out = hash_f32(out, helicopter->mass);
        out = hash_f32(out, helicopter->max_thrust_n);
        out = hash_f32(out, helicopter->pitch_torque);
        out = hash_f32(out, helicopter->roll_torque);
        out = hash_f32(out, helicopter->yaw_torque);
        out = hash_f32(out, helicopter->angular_damping);
        out = hierarchy_hash_combine(out, helicopter->hover_assist);
        out = hierarchy_hash_combine(out, helicopter->is_player);
        out = hierarchy_hash_combine(out, helicopter->runtime_body);
    }
    if (auto* character = registry.get<scene::CharacterControllerComponent>(entity)) {
        out = hash_f32(out, character->height);
        out = hash_f32(out, character->radius);
        out = hash_f32(out, character->move_speed);
        out = hierarchy_hash_combine(out, character->runtime_character);
    }
    if (auto* faction = registry.get<scene::FactionComponent>(entity)) {
        out = hierarchy_hash_combine(out, faction->faction);
    }
    if (auto* hitbox = registry.get<scene::HitboxComponent>(entity)) {
        out = hash_f32(out, hitbox->offset.x);
        out = hash_f32(out, hitbox->offset.y);
        out = hash_f32(out, hitbox->offset.z);
        out = hash_f32(out, hitbox->half_extent.x);
        out = hash_f32(out, hitbox->half_extent.y);
        out = hash_f32(out, hitbox->half_extent.z);
        out = hash_f32(out, hitbox->radius);
        out = hierarchy_hash_combine(out, hitbox->enabled);
    }
    if (auto* weapon_mode = registry.get<scene::WeaponModeComponent>(entity)) {
        out = hierarchy_hash_combine(out, static_cast<u32>(weapon_mode->kind));
        out = hash_f32(out, weapon_mode->projectile_speed);
        out = hash_f32(out, weapon_mode->projectile_life);
    }
    if (auto* ai_agent = registry.get<scene::AiAgentComponent>(entity)) {
        out = hierarchy_hash_combine(out, static_cast<u32>(ai_agent->state));
        out = hash_f32(out, ai_agent->sight_range);
        out = hash_f32(out, ai_agent->fov_cos);
        out = hash_f32(out, ai_agent->attack_range);
        out = hash_f32(out, ai_agent->think_interval);
        out = hash_f32(out, ai_agent->move_speed);
    }
    if (registry.get<scene::PerceptionComponent>(entity) != nullptr) {
        out = hierarchy_hash_combine(out, 0x50455243u);  // "PERC" presence tag
    }
    if (auto* patrol = registry.get<scene::PatrolComponent>(entity)) {
        out = hierarchy_hash_combine(out, patrol->count);
        out = hash_f32(out, patrol->wait_time);
        out = hash_f32(out, patrol->arrive_radius);
    }
    if (auto* track = registry.get<scene::TrackComponent>(entity)) {
        out = hierarchy_hash_combine(out, track->segment_count);
        out = hash_f32(out, track->target_speed);
        out = hash_f32(out, track->look_ahead);
        out = hash_f32(out, track->steer_gain);
        out = hash_f32(out, track->steer_clamp);
        out = hash_f32(out, track->throttle_kp);
        out = hash_f32(out, track->lap_gate_point.x);
        out = hash_f32(out, track->lap_gate_point.y);
        out = hash_f32(out, track->lap_gate_point.z);
        out = hash_f32(out, track->lap_gate_normal.x);
        out = hash_f32(out, track->lap_gate_normal.y);
        out = hash_f32(out, track->lap_gate_normal.z);
        for (u32 si = 0u; si < scene::TrackComponent::kMaxSegments; ++si) {
            out = hash_f32(out, track->segments[si].p0.x);
            out = hash_f32(out, track->segments[si].p3.x);
            out = hash_f32(out, track->segments[si].half_width);
        }
    }
    return out;
}

std::vector<u8> encode_scene_selection_state_envelope(scene::Scene& scene) {
    const scene::EnvironmentSettings& env = scene.environment().settings();
    const scene::RenderSettings& rs = scene.render_settings();
    ipc::msgpack::Writer w;
    w.map_header(4);
    w.str("v");
    w.u32_(ipc::proto::kProtocolVersion);
    w.str("ch");
    w.str(ipc::proto::channels::kselection);
    w.str("type");
    w.str("state");
    w.str("payload");
    w.map_header(3);
    w.str("entity_id");
    w.u32_(0u);
    w.str("entity_label");
    w.str("Arcade Scene");
    w.str("components");
    w.map_header(2);
    w.str("EnvironmentComponent");
    w.map_header(10);
    w.str("clear_color_rgba8");
    w.u32_(env.clear_color_rgba8);
    w.str("clear_color");
    w.boolean(env.clear_color);
    w.str("clear_depth");
    w.boolean(env.clear_depth);
    w.str("sky_zenith_rgba8");
    w.u32_(env.sky.zenith_rgba8);
    w.str("sky_horizon_rgba8");
    w.u32_(env.sky.horizon_rgba8);
    w.str("sky_intensity");
    w.f32_(env.sky.intensity);
    w.str("cloud_enabled");
    w.boolean(env.clouds.enabled);
    w.str("cloud_coverage");
    w.f32_(env.clouds.coverage);
    w.str("cloud_density");
    w.f32_(env.clouds.density);
    w.str("cloud_height");
    w.f32_(env.clouds.height);
    w.str("RenderSettingsComponent");
    w.map_header(14);
    w.str("render_mode");
    w.u32_(static_cast<u32>(rs.render_mode));
    w.str("sun_enabled");
    w.boolean(rs.sun_enabled != 0u);
    w.str("sun_direction");
    write_vec3(w, rs.sun_direction);
    w.str("sun_color_rgba8");
    w.u32_(rs.sun_color_rgba8);
    w.str("sun_intensity");
    w.f32_(rs.sun_intensity);
    w.str("ambient_color_rgba8");
    w.u32_(rs.ambient_color_rgba8);
    w.str("ambient_intensity");
    w.f32_(rs.ambient_intensity);
    w.str("shadows_enabled");
    w.boolean(rs.shadows_enabled != 0u);
    w.str("shadow_softness");
    w.f32_(rs.shadow_softness);
    w.str("shadow_opacity");
    w.f32_(rs.shadow_opacity);
    w.str("rt_trace_downscale");
    w.u32_(rs.rt_trace_downscale);
    w.str("rt_ao");
    w.u32_(rs.rt_ao);
    w.str("rt_reflection_bounces");
    w.u32_(rs.rt_reflection_bounces);
    w.str("rt_samples");
    w.u32_(rs.rt_samples);
    return w.buffer();
}

std::vector<u8> encode_selection_cleared_envelope() {
    ipc::msgpack::Writer w;
    w.map_header(4);
    w.str("v");
    w.u32_(ipc::proto::kProtocolVersion);
    w.str("ch");
    w.str(ipc::proto::channels::kselection);
    w.str("type");
    w.str("cleared");
    w.str("payload");
    w.nil();
    return w.buffer();
}

struct WebAssetEntry {
    std::string_view path;
    std::string_view category;
    std::string_view pack;
    u32 size_bytes = 0u;
    std::string_view hash;
};

std::vector<u8> encode_asset_catalog_envelope() {
    constexpr std::array<WebAssetEntry, 7> kBuiltInAssets{{
        {"textures.procedural.wooden_crate", "texture", "procedural", 128u * 128u * 4u, "procedural-wooden-crate"},
        {"textures.procedural.checker", "texture", "procedural", 64u * 64u * 4u, "procedural-checker"},
        {"textures.procedural.grid", "texture", "procedural", 128u * 128u * 4u, "procedural-grid"},
        {"textures.procedural.bricks", "texture", "procedural", 128u * 128u * 4u, "procedural-bricks"},
        {"textures.procedural.wood_planks", "texture", "procedural", 128u * 128u * 4u, "procedural-wood-planks"},
        {"textures.procedural.building_facade", "texture", "procedural", 64u * 64u * 4u, "procedural-building-facade"},
        {"samples/01_triangle/assets/crate.ppm", "texture", "loose", 32u * 32u * 3u, "loose-crate-ppm"},
    }};

    ipc::msgpack::Writer w;
    w.map_header(4);
    w.str("v");
    w.u32_(ipc::proto::kProtocolVersion);
    w.str("ch");
    w.str("assets");
    w.str("type");
    w.str("catalog");
    w.str("payload");
    w.map_header(1);
    w.str("entries");
    w.array_header(kBuiltInAssets.size());
    for (const WebAssetEntry& asset : kBuiltInAssets) {
        w.map_header(5);
        w.str("path");
        w.str(asset.path);
        w.str("category");
        w.str(asset.category);
        w.str("pack");
        w.str(asset.pack);
        w.str("size_bytes");
        w.u32_(asset.size_bytes);
        w.str("content_hash");
        w.str(asset.hash);
    }
    return w.buffer();
}

std::vector<u8> encode_selection_state_envelope(scene::Scene& scene, Entity entity) {
    auto& registry = scene.registry();
    const auto* transform = registry.get<scene::TransformComponent>(entity);
    const auto* node = registry.get<scene::SceneNodeComponent>(entity);
    const auto* camera = registry.get<scene::CameraComponent>(entity);
    const auto* light = registry.get<scene::LightComponent>(entity);
    const auto* renderable = registry.get<scene::RenderableComponent>(entity);
    const auto* gameplay_tag = registry.get<scene::GameplayTagComponent>(entity);
    const auto* player_controller = registry.get<scene::PlayerControllerComponent>(entity);
    const auto* health = registry.get<scene::HealthComponent>(entity);
    const auto* weapon = registry.get<scene::WeaponComponent>(entity);
    const auto* rigid_body = registry.get<scene::RigidBodyComponent>(entity);
    const auto* vehicle = registry.get<scene::VehicleComponent>(entity);
    const auto* helicopter = registry.get<scene::HelicopterComponent>(entity);
    const auto* character = registry.get<scene::CharacterControllerComponent>(entity);
    const auto* faction = registry.get<scene::FactionComponent>(entity);
    const auto* hitbox = registry.get<scene::HitboxComponent>(entity);
    const auto* weapon_mode = registry.get<scene::WeaponModeComponent>(entity);
    const auto* ai_agent = registry.get<scene::AiAgentComponent>(entity);
    const auto* perception = registry.get<scene::PerceptionComponent>(entity);
    const auto* patrol = registry.get<scene::PatrolComponent>(entity);
    const auto* track = registry.get<scene::TrackComponent>(entity);
    const bool has_material =
        renderable && scene.materials().valid(renderable->material);
    const usize component_count = (transform ? 1u : 0u) + (node ? 1u : 0u) +
                                  (camera ? 1u : 0u) + (light ? 1u : 0u) +
                                  (renderable ? 1u : 0u) + (has_material ? 1u : 0u) +
                                  (gameplay_tag ? 1u : 0u) +
                                  (player_controller ? 1u : 0u) + (health ? 1u : 0u) +
                                  (weapon ? 1u : 0u) + (rigid_body ? 1u : 0u) +
                                  (vehicle ? 1u : 0u) + (helicopter ? 1u : 0u) +
                                  (character ? 1u : 0u) + (faction ? 1u : 0u) +
                                  (hitbox ? 1u : 0u) + (weapon_mode ? 1u : 0u) +
                                  (ai_agent ? 1u : 0u) + (perception ? 1u : 0u) +
                                  (patrol ? 1u : 0u) + (track ? 1u : 0u);

    ipc::msgpack::Writer w;
    w.map_header(4);
    w.str("v");
    w.u32_(ipc::proto::kProtocolVersion);
    w.str("ch");
    w.str(ipc::proto::channels::kselection);
    w.str("type");
    w.str("state");
    w.str("payload");
    w.map_header(3);
    w.str("entity_id");
    w.u32_(entity.raw);
    w.str("entity_label");
    w.str(hierarchy_label_for(scene, entity));
    w.str("components");
    w.map_header(component_count);

    if (transform) {
        w.str("TransformComponent");
        w.map_header(3);
        w.str("translation");
        write_vec3(w, transform->local.translation);
        w.str("rotation");
        write_vec3(w, rotation_degrees(transform->local.rotation));
        w.str("scale");
        write_vec3(w, transform->local.scale);
    }

    if (node) {
        w.str("SceneNodeComponent");
        w.map_header(2);
        w.str("node_raw");
        w.u32_(node->node.raw);
        w.str("parent_entity");
        w.u32_(parent_entity_for_node(scene, node->node));
    }

    if (camera) {
        w.str("CameraComponent");
        w.map_header(7);
        w.str("fov_y_deg");
        w.f32_(camera->fov_y_rad * math::kRadToDeg);
        w.str("aspect");
        w.f32_(camera->aspect);
        w.str("near_z");
        w.f32_(camera->near_z);
        w.str("far_z");
        w.f32_(camera->far_z);
        w.str("tile_w");
        w.u32_(camera->tile_w);
        w.str("tile_h");
        w.u32_(camera->tile_h);
        w.str("active");
        w.boolean(camera->active != 0u);
    }

    if (light) {
        w.str("LightComponent");
        w.map_header(7);
        w.str("kind");
        w.u32_(static_cast<u32>(light->kind));
        w.str("color_rgba8");
        w.u32_(light->color_rgba8);
        w.str("intensity");
        w.f32_(light->intensity);
        w.str("range");
        w.f32_(light->range);
        w.str("inner_cone_deg");
        w.f32_(light->inner_cone_deg);
        w.str("outer_cone_deg");
        w.f32_(light->outer_cone_deg);
        w.str("casts_shadow");
        w.boolean(light->casts_shadow != 0u);
    }

    if (renderable) {
        w.str("RenderableComponent");
        w.map_header(8);
        w.str("geometry");
        w.u32_(static_cast<u32>(renderable->geometry));
        w.str("geometry_id");
        w.u32_(renderable->geometry_id);
        w.str("material");
        w.u32_(renderable->material.raw);
        w.str("visible");
        w.boolean(scene::renderable_is_visible(*renderable));
        w.str("mobility");
        w.u32_(static_cast<u32>(renderable->mobility));
        w.str("local_bounds_min");
        write_vec3(w, renderable->local_bounds.min);
        w.str("local_bounds_max");
        write_vec3(w, renderable->local_bounds.max);
        w.str("casts_shadow_override");
        w.boolean((renderable->flags & scene::RenderableFlags::CastsShadowOverride) != 0u);
    }

    if (has_material) {
        const render::MaterialDesc material = scene.materials().get(renderable->material);
        w.str("MaterialComponent");
        w.map_header(9);
        w.str("albedo_rgba8");
        w.u32_(material.albedo_rgba8);
        w.str("base_color_texture_name");
        if (const auto it = material_texture_names().find(renderable->material.raw);
            it != material_texture_names().end()) {
            w.str(it->second);
        } else {
            w.str("");
        }
        w.str("reflectivity");
        w.f32_(material.reflectivity);
        w.str("roughness");
        w.f32_(material.roughness);
        w.str("emissive");
        w.f32_(material.emissive);
        w.str("alpha_cutoff");
        w.f32_(material.alpha_cutoff);
        w.str("blend");
        w.u32_(static_cast<u32>(material.blend));
        w.str("shadow_opacity");
        w.f32_(material.shadow_opacity);
        w.str("shadow_softness");
        w.f32_(material.shadow_softness);
    }

    if (gameplay_tag) {
        w.str("GameplayTagComponent");
        w.map_header(2);
        w.str("role");
        w.u32_(static_cast<u32>(gameplay_tag->role));
        w.str("flags");
        w.u32_(gameplay_tag->flags);
    }

    if (player_controller) {
        w.str("PlayerControllerComponent");
        w.map_header(6);
        w.str("walk_speed");
        w.f32_(player_controller->walk_speed);
        w.str("run_speed");
        w.f32_(player_controller->run_speed);
        w.str("jump_speed");
        w.f32_(player_controller->jump_speed);
        w.str("mouse_sensitivity");
        w.f32_(player_controller->mouse_sensitivity);
        w.str("height");
        w.f32_(player_controller->height);
        w.str("radius");
        w.f32_(player_controller->radius);
    }

    if (health) {
        w.str("HealthComponent");
        w.map_header(3);
        w.str("max_health");
        w.f32_(health->max_health);
        w.str("current_health");
        w.f32_(health->current_health);
        w.str("faction");
        w.u32_(health->faction);
    }

    if (weapon) {
        w.str("WeaponComponent");
        w.map_header(5);
        w.str("damage");
        w.f32_(weapon->damage);
        w.str("range");
        w.f32_(weapon->range);
        w.str("fire_rate");
        w.f32_(weapon->fire_rate);
        w.str("ammo");
        w.u32_(weapon->ammo);
        w.str("automatic");
        w.boolean(weapon->automatic != 0u);
    }

    if (rigid_body) {
        w.str("RigidBodyComponent");
        w.map_header(6);
        w.str("shape");
        w.u32_(static_cast<u32>(rigid_body->shape));
        w.str("mass");
        w.f32_(rigid_body->mass);
        w.str("half_extent");
        write_vec3(w, rigid_body->half_extent);
        w.str("friction");
        w.f32_(rigid_body->friction);
        w.str("restitution");
        w.f32_(rigid_body->restitution);
        w.str("runtime_body");
        w.u32_(rigid_body->runtime_body);
    }

    if (vehicle) {
        w.str("VehicleComponent");
        w.map_header(20);
        w.str("half_extent");
        write_vec3(w, vehicle->half_extent);
        w.str("mass");
        w.f32_(vehicle->mass);
        w.str("engine_max_torque");
        w.f32_(vehicle->engine_max_torque);
        w.str("drag");
        w.f32_(vehicle->drag);
        w.str("wheel_radius");
        w.f32_(vehicle->wheel_radius);
        w.str("suspension");
        w.f32_(vehicle->suspension);
        w.str("stiffness");
        w.f32_(vehicle->stiffness);
        w.str("damping");
        w.f32_(vehicle->damping);
        w.str("max_speed");
        w.f32_(vehicle->max_speed);
        w.str("steer_full_speed");
        w.f32_(vehicle->steer_full_speed);
        w.str("steer_taper_speed");
        w.f32_(vehicle->steer_taper_speed);
        w.str("steer_min_authority");
        w.f32_(vehicle->steer_min_authority);
        w.str("ground_mode");
        w.u32_(static_cast<u32>(vehicle->ground_mode));
        w.str("plane_y");
        w.f32_(vehicle->plane_y);
        w.str("hf_base_y");
        w.f32_(vehicle->hf_base_y);
        w.str("hf_amplitude");
        w.f32_(vehicle->hf_amplitude);
        w.str("hf_frequency");
        w.f32_(vehicle->hf_frequency);
        w.str("is_player");
        w.boolean(vehicle->is_player != 0u);
        w.str("runtime_vehicle");
        w.u32_(vehicle->runtime_vehicle);
        w.str("runtime_chassis");
        w.u32_(vehicle->runtime_chassis);
    }

    if (helicopter) {
        w.str("HelicopterComponent");
        w.map_header(10);
        w.str("half_extent");
        write_vec3(w, helicopter->half_extent);
        w.str("mass");
        w.f32_(helicopter->mass);
        w.str("max_thrust_n");
        w.f32_(helicopter->max_thrust_n);
        w.str("pitch_torque");
        w.f32_(helicopter->pitch_torque);
        w.str("roll_torque");
        w.f32_(helicopter->roll_torque);
        w.str("yaw_torque");
        w.f32_(helicopter->yaw_torque);
        w.str("angular_damping");
        w.f32_(helicopter->angular_damping);
        w.str("hover_assist");
        w.boolean(helicopter->hover_assist != 0u);
        w.str("is_player");
        w.boolean(helicopter->is_player != 0u);
        w.str("runtime_body");
        w.u32_(helicopter->runtime_body);
    }

    if (character) {
        w.str("CharacterControllerComponent");
        w.map_header(4);
        w.str("height");
        w.f32_(character->height);
        w.str("radius");
        w.f32_(character->radius);
        w.str("move_speed");
        w.f32_(character->move_speed);
        w.str("runtime_character");
        w.u32_(character->runtime_character);
    }

    if (faction) {
        w.str("FactionComponent");
        w.map_header(1);
        w.str("faction");
        w.u32_(faction->faction);
    }

    if (hitbox) {
        w.str("HitboxComponent");
        w.map_header(4);
        w.str("offset");
        write_vec3(w, hitbox->offset);
        w.str("half_extent");
        write_vec3(w, hitbox->half_extent);
        w.str("radius");
        w.f32_(hitbox->radius);
        w.str("enabled");
        w.boolean(hitbox->enabled != 0u);
    }

    if (weapon_mode) {
        w.str("WeaponModeComponent");
        w.map_header(3);
        w.str("kind");
        w.u32_(static_cast<u32>(weapon_mode->kind));
        w.str("projectile_speed");
        w.f32_(weapon_mode->projectile_speed);
        w.str("projectile_life");
        w.f32_(weapon_mode->projectile_life);
    }

    if (ai_agent) {
        w.str("AiAgentComponent");
        w.map_header(6);
        w.str("state");
        w.u32_(static_cast<u32>(ai_agent->state));
        w.str("sight_range");
        w.f32_(ai_agent->sight_range);
        w.str("fov_cos");
        w.f32_(ai_agent->fov_cos);
        w.str("attack_range");
        w.f32_(ai_agent->attack_range);
        w.str("think_interval");
        w.f32_(ai_agent->think_interval);
        w.str("move_speed");
        w.f32_(ai_agent->move_speed);
    }

    if (perception) {
        w.str("PerceptionComponent");
        w.map_header(0);
    }

    if (patrol) {
        w.str("PatrolComponent");
        w.map_header(2);
        w.str("wait_time");
        w.f32_(patrol->wait_time);
        w.str("arrive_radius");
        w.f32_(patrol->arrive_radius);
    }

    if (track) {
        w.str("TrackComponent");
        w.map_header(8);
        w.str("segment_count");
        w.u32_(track->segment_count);
        w.str("target_speed");
        w.f32_(track->target_speed);
        w.str("look_ahead");
        w.f32_(track->look_ahead);
        w.str("steer_gain");
        w.f32_(track->steer_gain);
        w.str("steer_clamp");
        w.f32_(track->steer_clamp);
        w.str("throttle_kp");
        w.f32_(track->throttle_kp);
        w.str("lap_gate_point");
        write_vec3(w, track->lap_gate_point);
        w.str("lap_gate_normal");
        write_vec3(w, track->lap_gate_normal);
    }

    return w.buffer();
}

std::string trimmed_editor_web_url() {
    constexpr std::string_view kFallback = "http://127.0.0.1:7654";
    constexpr std::string_view kOldDevDefault = "http://127.0.0.1:5173";
    auto* cvar = console::Console::Get().FindCVar("editor_web_url");
    auto* dev_mode = console::Console::Get().FindCVar("editor_web_dev_mode");
    std::string out = cvar ? cvar->value : std::string{kFallback};
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    if ((!dev_mode || !dev_mode->GetBool()) && out == kOldDevDefault)
        out = std::string{kFallback};
    return out.empty() ? std::string{kFallback} : out;
}

std::string editor_panel_url(std::string_view panel) {
    std::string url = trimmed_editor_web_url();
    url += "/panels/";
    url.append(panel.data(), panel.size());
    url += "?token=";
    url += ipc::Server::Get().session_token();
    return url;
}

bool start_editor_ipc(console::Output& out) {
    ipc::ServerDesc desc{};
    if (!ipc::Server::Get().start(desc)) {
        out.PrintLine("editor-ipc: failed to start server on 127.0.0.1:7654");
        return false;
    }
    out.PrintLine("editor-ipc: listening on 127.0.0.1:7654");
    out.FormatLine("editor-ipc: token {}", ipc::Server::Get().session_token());
    out.FormatLine("editor workbench: {}", editor_panel_url("workbench"));
    return true;
}

bool launch_chrome_app_window(std::string_view url) {
#if defined(__APPLE__)
    if (url.empty())
        return false;
    std::string owned{url};
    std::string app_arg = "--app=" + owned;
    char* argv[] = {
        const_cast<char*>("open"),
        const_cast<char*>("-na"),
        const_cast<char*>("Google Chrome"),
        const_cast<char*>("--args"),
        app_arg.data(),
        const_cast<char*>("--new-window"),
        nullptr,
    };
    pid_t pid = 0;
    if (::posix_spawnp(&pid, "open", nullptr, nullptr, argv, environ) != 0)
        return false;
    int status = 0;
    return ::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
    (void)url;
    return false;
#endif
}

void close_chrome_editor_windows() {
#if defined(__APPLE__)
    char* argv[] = {
        const_cast<char*>("osascript"),
        const_cast<char*>("-e"),
        const_cast<char*>("tell application \"Google Chrome\""),
        const_cast<char*>("-e"),
        const_cast<char*>("repeat with w in windows"),
        const_cast<char*>("-e"),
        const_cast<char*>(
            "try\nif (URL of active tab of w contains \"127.0.0.1:7654/panels/\") then close w\nend try"),
        const_cast<char*>("-e"),
        const_cast<char*>("end repeat"),
        const_cast<char*>("-e"),
        const_cast<char*>("end tell"),
        nullptr,
    };
    pid_t pid = 0;
    (void)::posix_spawnp(&pid, "osascript", nullptr, nullptr, argv, environ);
#endif
}

void open_web_console(console::Output& out) {
    ui::console::set_open(false);
    if (current_mode() != Mode::Edit)
        toggle_mode();
    if (!start_editor_ipc(out))
        return;
    const std::string url = editor_panel_url("workbench");
    out.FormatLine("web-console: {}", url);
    if (launch_chrome_app_window(url) || platform::open_external_url(url)) {
        out.PrintLine("web-console: opened editor window");
    } else {
        out.PrintLine("web-console: could not open browser; paste the URL above");
    }
}

void publish_schema_catalog_if_needed() {
    auto& server = ipc::Server::Get();
    if (!server.has_subscribers(ipc::proto::channels::kschemas))
        return;

    static u32 frames_until_refresh = 0;
    if (frames_until_refresh > 0u) {
        --frames_until_refresh;
        return;
    }
    frames_until_refresh = 20u;

    const std::vector<u8> payload = encode_schema_catalog_envelope();
    server.broadcast(ipc::proto::channels::kschemas, payload);
}

void publish_asset_catalog_if_needed() {
    auto& server = ipc::Server::Get();
    if (!server.has_subscribers("assets"))
        return;

    static u32 frames_until_refresh = 0;
    if (frames_until_refresh > 0u) {
        --frames_until_refresh;
        return;
    }
    frames_until_refresh = 60u;

    const std::vector<u8> payload = encode_asset_catalog_envelope();
    server.broadcast("assets", payload);
}

void publish_selection_if_needed(scene::Scene* scene) {
    auto& server = ipc::Server::Get();
    if (!server.has_subscribers(ipc::proto::channels::kselection))
        return;

    static u64 last_signature = 0;
    static u32 unchanged_frames = 0;
    static bool last_was_cleared = false;

    const u32 selection_raw = selection::selected_scene_entity_raw();
    u32 raw = selected_entity_raw().load(std::memory_order_acquire);
    u32 generation = selected_entity_generation().load(std::memory_order_acquire);
    if (selection_raw != raw) {
        raw = selection_raw;
        selected_entity_raw().store(raw, std::memory_order_release);
        generation = selected_entity_generation().fetch_add(1u, std::memory_order_acq_rel) + 1u;
    }
    if (!scene) {
        if (raw != 0u) {
            clear_web_selection_mirror();
        }
        if (last_was_cleared && unchanged_frames++ < 20u)
            return;
        last_signature = 0;
        unchanged_frames = 0;
        last_was_cleared = true;
        const std::vector<u8> payload = encode_selection_cleared_envelope();
        server.broadcast(ipc::proto::channels::kselection, payload);
        return;
    }

    Entity entity{raw};
    if (!entity.valid()) {
        const u64 signature = selection_signature(*scene, entity, generation);
        if (!last_was_cleared && signature == last_signature && unchanged_frames++ < 20u)
            return;
        last_signature = signature;
        unchanged_frames = 0;
        last_was_cleared = false;

        const std::vector<u8> payload = encode_scene_selection_state_envelope(*scene);
        server.broadcast(ipc::proto::channels::kselection, payload);
        return;
    }

    if (!scene->registry().alive(entity)) {
        clear_web_selection_mirror();
        if (last_was_cleared && unchanged_frames++ < 20u)
            return;
        last_signature = 0;
        unchanged_frames = 0;
        last_was_cleared = true;
        const std::vector<u8> payload = encode_selection_cleared_envelope();
        server.broadcast(ipc::proto::channels::kselection, payload);
        return;
    }

    const u64 signature = selection_signature(*scene, entity, generation);
    if (!last_was_cleared && signature == last_signature && unchanged_frames++ < 20u)
        return;
    last_signature = signature;
    unchanged_frames = 0;
    last_was_cleared = false;

    const std::vector<u8> payload = encode_selection_state_envelope(*scene, entity);
    server.broadcast(ipc::proto::channels::kselection, payload);
}

}  // namespace

void ensure_web_panel_commands_registered() {
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    std::atexit(close_chrome_editor_windows);

    auto& console_ref = console::Console::Get();
    ipc::Server::Get().set_selection_select_handler(on_web_selection_select);
    console_ref.RegisterCVar("editor_web_url",
                             "http://127.0.0.1:7654",
                             "Base URL for editor panels; set to Vite only while developing the web UI.",
                             console::CVarFlags::Archive);
    console_ref.RegisterCVar("editor_web_dev_mode",
                             "0",
                             "Honor editor_web_url for an external web dev server instead of the bundled panel.",
                             console::CVarFlags::Archive);
    console_ref.RegisterCommand("web_console",
                                "Open the Psynder web editor console/workbench.",
                                [](std::span<const std::string_view>, console::Output& out) {
                                    open_web_console(out);
                                });
    console_ref.RegisterCommand("editor_spawn_prop",
                                "Queue a prop spawn request from the web editor.",
                                [](std::span<const std::string_view> args, console::Output& out) {
                                    if (args.empty()) {
                                        out.PrintLine("editor_spawn_prop: expected prop id");
                                        return;
                                    }
                                    out.FormatLine("editor_spawn_prop: queued {}", args[0]);
                                });
}

void close_web_panel_windows() {
    close_chrome_editor_windows();
}

void clear_web_scene_authoring_state() {
    entity_labels().clear();
    clear_web_material_texture_names();
    clear_web_selection_mirror();
    hierarchy_generation().fetch_add(1u, std::memory_order_acq_rel);
    set_web_scene_dirty(false);
}

std::string web_entity_label(Entity entity) {
    if (!entity.valid())
        return {};
    const auto label_it = entity_labels().find(entity.raw);
    return label_it == entity_labels().end() ? std::string{} : label_it->second;
}

void set_web_entity_label(Entity entity, std::string_view label) {
    if (!entity.valid())
        return;
    bool changed = false;
    auto& labels = entity_labels();
    if (label.empty()) {
        changed = labels.erase(entity.raw) != 0u;
    } else {
        const auto found = labels.find(entity.raw);
        if (found == labels.end() || found->second != label) {
            labels[entity.raw] = std::string{label};
            changed = true;
        }
    }
    if (changed) {
        bump_selected_entity_generation();
        hierarchy_generation().fetch_add(1u, std::memory_order_acq_rel);
    }
}

std::string web_material_texture_name(u32 material_raw) {
    const auto it = material_texture_names().find(material_raw);
    return it == material_texture_names().end() ? std::string{} : it->second;
}

void set_web_material_texture_name(u32 material_raw, std::string_view texture_name) {
    if (material_raw == 0u)
        return;
    bool changed = false;
    auto& names = material_texture_names();
    if (texture_name.empty()) {
        changed = names.erase(material_raw) != 0u;
    } else {
        const auto found = names.find(material_raw);
        if (found == names.end() || found->second != texture_name) {
            names[material_raw] = std::string{texture_name};
            changed = true;
        }
    }
    if (changed) {
        bump_selected_entity_generation();
    }
}

void clear_web_material_texture_names() {
    if (material_texture_names().empty())
        return;
    material_texture_names().clear();
    bump_selected_entity_generation();
}

WebSceneDirtyState web_scene_dirty_state() {
    return WebSceneDirtyState{
        .dirty = scene_dirty_flag().load(std::memory_order_acquire),
        .generation = scene_dirty_generation().load(std::memory_order_acquire),
    };
}

void set_web_scene_dirty(bool dirty) {
    const bool previous = scene_dirty_flag().exchange(dirty, std::memory_order_acq_rel);
    if (previous != dirty)
        scene_dirty_generation().fetch_add(1u, std::memory_order_acq_rel);
}

void mark_web_scene_dirty() {
    scene_dirty_flag().store(true, std::memory_order_release);
    scene_dirty_generation().fetch_add(1u, std::memory_order_acq_rel);
}

void publish_web_scene_load_failed(std::string_view path, std::string_view error) {
    auto& server = ipc::Server::Get();
    if (!server.has_subscribers(ipc::proto::channels::kscene))
        return;
    const std::vector<u8> payload = encode_scene_load_failed_envelope(path, error);
    server.broadcast(ipc::proto::channels::kscene, payload);
}

void publish_web_scene_dirty() {
    auto& server = ipc::Server::Get();
    if (!server.has_subscribers(ipc::proto::channels::kscene))
        return;

    static bool last_dirty = false;
    static u32 last_generation = 0;
    static u32 unchanged_frames = 0;
    const WebSceneDirtyState state = web_scene_dirty_state();
    if (state.dirty == last_dirty && state.generation == last_generation &&
        unchanged_frames++ < 20u) {
        return;
    }

    last_dirty = state.dirty;
    last_generation = state.generation;
    unchanged_frames = 0;
    const std::vector<u8> payload = encode_dirty_state_envelope(state);
    server.broadcast(ipc::proto::channels::kscene, payload);
}

void publish_web_scene_hierarchy(scene::Scene* scene) {
    publish_schema_catalog_if_needed();
    publish_selection_if_needed(scene);
    publish_web_scene_dirty();

    auto& server = ipc::Server::Get();
    if (!server.has_subscribers(ipc::proto::channels::kscene))
        return;

    static u64 last_signature = 0;
    static u32 unchanged_frames = 0;

    const std::vector<HierarchyEntry> entries = snapshot_scene_hierarchy(scene);
    u64 signature = hierarchy_signature(entries);
    signature = hierarchy_hash_combine(signature,
                                       hierarchy_generation().load(std::memory_order_acquire));
    if (signature == last_signature && unchanged_frames++ < 20u)
        return;
    last_signature = signature;
    unchanged_frames = 0;

    const std::vector<u8> payload = encode_hierarchy_envelope(entries);
    server.broadcast(ipc::proto::channels::kscene, payload);
}

void publish_web_selection_command_ack(std::string_view command,
                                       bool ok,
                                       std::string_view text,
                                       Entity entity,
                                       std::string_view component,
                                       std::string_view field) {
    auto& server = ipc::Server::Get();
    if (!server.has_subscribers(ipc::proto::channels::kselection))
        return;
    const std::vector<u8> payload =
        encode_selection_command_ack_envelope(command, ok, text, entity, component, field);
    server.broadcast(ipc::proto::channels::kselection, payload);
}

void publish_web_profiler_frame(const WebProfilerFrame& frame) {
    auto& server = ipc::Server::Get();
    const bool has_profiler_subscribers = server.has_subscribers("profiler");
    const bool has_stats_subscribers = server.has_subscribers("stats");
    const bool has_perf_subscribers = server.has_subscribers("perf");
    if (!has_profiler_subscribers && !has_stats_subscribers && !has_perf_subscribers)
        return;

    server.broadcast_stats_tick(ipc::StatsTick{
        frame.frame_index,
        frame.cpu_ms,
        frame.render_ms,
        frame.draw_calls,
        frame.entities,
        frame.sections,
    });
}

void pump_web_panels() {
    publish_schema_catalog_if_needed();
    publish_asset_catalog_if_needed();
    ipc::Server::Get().pump();
}

}  // namespace psynder::editor
