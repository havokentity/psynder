// SPDX-License-Identifier: MIT
// Psynder editor web-panel bridge.

#include "WebPanels.h"

#include "core/console/Console.h"
#include "editor/core/Selection.h"
#include "editor/ipc/Ipc.h"
#include "editor/ipc/internal/Msgpack.h"
#include "editor/ipc/proto/Protocol.gen.h"
#include "math/MathExt.h"
#include "platform/Platform.h"
#include "scene/SceneEcs.h"
#include "ui/console/ConsoleOverlay.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
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

std::unordered_map<u32, std::string>& entity_labels() {
    static std::unordered_map<u32, std::string> labels;
    return labels;
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
    selected_entity_generation().fetch_add(1u, std::memory_order_acq_rel);
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
    w.array_header(7);

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
    write_component_schema_header(w, "MaterialComponent", "native-material-v1", 8);
    write_field_schema(w, "albedo_rgba8", "color", false);
    write_field_schema(w, "reflectivity", "f32", false, 0.01f);
    write_field_schema(w, "roughness", "f32", false, 0.01f);
    write_field_schema(w, "emissive", "f32", false, 0.01f);
    write_field_schema(w, "alpha_cutoff", "f32", false, 0.01f);
    write_field_schema(w, "blend", "enum", false, {}, {}, kBlendOptions);
    write_field_schema(w, "shadow_opacity", "f32", false, 0.01f);
    write_field_schema(w, "shadow_softness", "f32", false, 0.01f);

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
    }
    return out;
}

std::vector<u8> encode_scene_selection_state_envelope(scene::Scene& scene) {
    const scene::EnvironmentSettings& env = scene.environment().settings();
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
    w.map_header(1);
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

std::vector<u8> encode_selection_state_envelope(scene::Scene& scene, Entity entity) {
    auto& registry = scene.registry();
    const auto* transform = registry.get<scene::TransformComponent>(entity);
    const auto* node = registry.get<scene::SceneNodeComponent>(entity);
    const auto* camera = registry.get<scene::CameraComponent>(entity);
    const auto* light = registry.get<scene::LightComponent>(entity);
    const auto* renderable = registry.get<scene::RenderableComponent>(entity);
    const bool has_material =
        renderable && scene.materials().valid(renderable->material);
    const usize component_count = (transform ? 1u : 0u) + (node ? 1u : 0u) +
                                  (camera ? 1u : 0u) + (light ? 1u : 0u) +
                                  (renderable ? 1u : 0u) + (has_material ? 1u : 0u);

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
        w.map_header(8);
        w.str("albedo_rgba8");
        w.u32_(material.albedo_rgba8);
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

void open_web_console(console::Output& out) {
    ui::console::set_open(false);
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

void publish_selection_if_needed(scene::Scene* scene) {
    auto& server = ipc::Server::Get();
    if (!server.has_subscribers(ipc::proto::channels::kselection))
        return;

    static u64 last_signature = 0;
    static u32 unchanged_frames = 0;
    static bool last_was_cleared = false;

    const u32 raw = selected_entity_raw().load(std::memory_order_acquire);
    const u32 generation = selected_entity_generation().load(std::memory_order_acquire);
    if (!scene) {
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

void clear_web_scene_authoring_state() {
    entity_labels().clear();
    selected_entity_raw().store(0u, std::memory_order_release);
    selection::clear_selection();
    selected_entity_generation().fetch_add(1u, std::memory_order_acq_rel);
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
    if (label.empty()) {
        entity_labels().erase(entity.raw);
    } else {
        entity_labels()[entity.raw] = std::string{label};
    }
    hierarchy_generation().fetch_add(1u, std::memory_order_acq_rel);
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
    ipc::Server::Get().pump();
}

}  // namespace psynder::editor
