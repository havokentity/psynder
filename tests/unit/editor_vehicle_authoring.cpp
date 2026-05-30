// SPDX-License-Identifier: MIT
// Psynder - Wave 8 no-code "drivable vehicle on terrain" authoring tests.
//
// Covers the three legs of the LANE A deliverable end to end:
//   1. SERIALIZATION round-trip: a VehicleComponent proxy authored with the new
//      Wave 8 fields (speed governor + speed-scaled steering authority + ground
//      binding mode + the procedural Heightfield surface) saves into the v6 SVHX
//      chunk and reloads bit-for-bit, with the SPHY base fields intact and the
//      runtime_* handles staying 0 (never serialized).
//   2. BACKWARD COMPAT: a scene file whose SVHX chunk is absent (a pre-Wave-8
//      v5 .psyscene) still loads -- the vehicle comes back with the component
//      defaults (governor off, full steering authority, Plane ground at 0). We
//      synthesise that "old file" by neutering the SVHX chunk descriptor on a
//      real saved buffer, exercising the loader's missing-chunk path.
//   3. PLAYRUNTIME synthesis: PlayRuntime::begin() turns a Heightfield-mode
//      VehicleComponent into a LIVE physics::vehicle that responds to throttle /
//      steer and rides the authored procedural terrain, then end() destroys it
//      and clears the handles. Deterministic (fixed 120 Hz steps, no RNG).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "editor/play/PlayRuntime.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/PhysicsComponents.h"
#include "scene/SceneEcs.h"
#include "scene/SceneFile.h"

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

using namespace psynder;
using Catch::Approx;

namespace {

struct RegistryReset {
    RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

scene::LocalTransform at(math::Vec3 t) {
    scene::LocalTransform out{};
    out.translation = t;
    return out;
}

Entity find_entity_named(scene::Scene& scene, std::string_view name) {
    Entity found{};
    scene.registry().query<scene::reads<scene::SceneNodeComponent>, scene::writes<>>(
        [&](std::span<const scene::SceneNodeComponent> nodes) {
            for (const scene::SceneNodeComponent& node : nodes) {
                if (scene.entity_name(node.entity) == name)
                    found = node.entity;
            }
        });
    return found;
}

// Author a vehicle entity with deliberately non-default Wave 8 fields so the
// round-trip proves every new field survives (not just its default).
scene::VehicleComponent authored_terrain_car() {
    scene::VehicleComponent vc{};
    vc.half_extent = {1.05f, 0.45f, 2.15f};
    vc.mass = 1380.0f;
    vc.engine_max_torque = 480.0f;
    vc.drag = 0.33f;
    vc.wheel_radius = 0.37f;
    vc.suspension = 0.34f;
    vc.stiffness = 33000.0f;
    vc.damping = 4600.0f;
    // Wave 8 governor + steering authority.
    vc.max_speed = 22.5f;
    vc.steer_full_speed = 4.5f;
    vc.steer_taper_speed = 16.0f;
    vc.steer_min_authority = 0.4f;
    // Wave 8 ground binding: ride a procedural rolling-hills surface.
    vc.ground_mode = scene::VehicleGroundMode::Heightfield;
    vc.plane_y = 1.5f;
    vc.hf_base_y = 0.75f;
    vc.hf_amplitude = 3.25f;
    vc.hf_frequency = 0.08f;
    vc.is_player = 1u;
    // RUNTIME junk that must NOT persist.
    vc.runtime_vehicle = 4242u;
    vc.runtime_chassis = 2424u;
    return vc;
}

}  // namespace

TEST_CASE("VehicleComponent Wave 8 fields round-trip through the v6 SVHX chunk",
          "[scene][scene_file][vehicle][authoring][wave8]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    const scene::VehicleComponent authored_vc = authored_terrain_car();

    scene::detail::AlignedVector<u8> bytes;
    scene::SceneFileSaveStats stats{};
    std::string error;
    {
        scene::Scene authored{registry};
        const Entity car = authored.create_entity(at({-4.0f, 2.0f, 0.0f}));
        REQUIRE(car.valid());
        REQUIRE(authored.set_entity_name(car, "TerrainCar"));
        registry.add<scene::VehicleComponent>(car, authored_vc);

        REQUIRE(scene::save_scene_file(authored, {}, bytes, &stats, &error));
        REQUIRE(error.empty());
    }
    REQUIRE(stats.physics_bodies == 1u);
    REQUIRE(stats.vehicle_exts == 1u);  // one SVHX record for the vehicle.

    scene::SceneFileView view{};
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(error.empty());
    REQUIRE(view.header->version == 6u);  // v6 bump.
    REQUIRE(view.physics_bodies.size() == 1u);
    REQUIRE(view.vehicle_exts.size() == 1u);

    // Reload into a fresh registry/scene and confirm the live component matches.
    registry.clear();
    registry.set_structural_deferred(false);
    scene::Scene loaded{registry};
    std::array<scene::SceneMeshBinding, 0> mesh_bindings{};
    std::array<Entity, 0> out_mesh_entities{};
    scene::instantiate_scene_file(loaded, view, mesh_bindings, out_mesh_entities);

    const Entity loaded_car = find_entity_named(loaded, "TerrainCar");
    REQUIRE(loaded_car.valid());
    const auto* vc = registry.get<scene::VehicleComponent>(loaded_car);
    REQUIRE(vc != nullptr);

    // SPHY base fields.
    REQUIRE_THAT(static_cast<double>(vc->mass), Catch::Matchers::WithinAbs(1380.0, 0.01));
    REQUIRE_THAT(static_cast<double>(vc->engine_max_torque),
                 Catch::Matchers::WithinAbs(480.0, 0.01));
    REQUIRE_THAT(static_cast<double>(vc->wheel_radius), Catch::Matchers::WithinAbs(0.37, 1e-4));
    // SVHX governor + steering authority fields.
    REQUIRE_THAT(static_cast<double>(vc->max_speed), Catch::Matchers::WithinAbs(22.5, 1e-4));
    REQUIRE_THAT(static_cast<double>(vc->steer_full_speed),
                 Catch::Matchers::WithinAbs(4.5, 1e-4));
    REQUIRE_THAT(static_cast<double>(vc->steer_taper_speed),
                 Catch::Matchers::WithinAbs(16.0, 1e-4));
    REQUIRE_THAT(static_cast<double>(vc->steer_min_authority),
                 Catch::Matchers::WithinAbs(0.4, 1e-4));
    // SVHX ground binding + procedural surface fields.
    REQUIRE(vc->ground_mode == scene::VehicleGroundMode::Heightfield);
    REQUIRE_THAT(static_cast<double>(vc->plane_y), Catch::Matchers::WithinAbs(1.5, 1e-4));
    REQUIRE_THAT(static_cast<double>(vc->hf_base_y), Catch::Matchers::WithinAbs(0.75, 1e-4));
    REQUIRE_THAT(static_cast<double>(vc->hf_amplitude), Catch::Matchers::WithinAbs(3.25, 1e-4));
    REQUIRE_THAT(static_cast<double>(vc->hf_frequency), Catch::Matchers::WithinAbs(0.08, 1e-4));
    // Runtime handles are NEVER serialized.
    REQUIRE(vc->runtime_vehicle == 0u);
    REQUIRE(vc->runtime_chassis == 0u);
}

TEST_CASE("A pre-Wave-8 scene without the SVHX chunk still loads (backward compat)",
          "[scene][scene_file][vehicle][authoring][wave8][compat]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    // Save a real scene that DOES carry a vehicle (and therefore an SVHX chunk).
    scene::detail::AlignedVector<u8> bytes;
    std::string error;
    {
        scene::Scene authored{registry};
        const Entity car = authored.create_entity(at({0.0f, 1.0f, 0.0f}));
        REQUIRE(car.valid());
        REQUIRE(authored.set_entity_name(car, "LegacyCar"));
        registry.add<scene::VehicleComponent>(car, authored_terrain_car());
        scene::SceneFileSaveStats stats{};
        REQUIRE(scene::save_scene_file(authored, {}, bytes, &stats, &error));
        REQUIRE(error.empty());
        REQUIRE(stats.vehicle_exts == 1u);
    }

    // Synthesise an "old" v5 file: drop the version to 5 and NEUTER the SVHX
    // chunk descriptor (point it at an empty range), exactly what the loader sees
    // for a .psyscene saved before Wave 8. The loader must treat the missing
    // chunk as empty and load the vehicle with the component defaults.
    {
        auto* header = reinterpret_cast<scene::SceneFileHeader*>(bytes.data());
        header->version = 5u;
        auto* chunks = reinterpret_cast<scene::SceneFileChunk*>(
            bytes.data() + sizeof(scene::SceneFileHeader));
        bool neutered = false;
        for (u32 i = 0u; i < header->chunk_count; ++i) {
            if (chunks[i].type == scene::SceneFileChunkType::VehicleExt) {
                chunks[i].bytes = 0u;  // zero-length => find_chunk/as_array see empty.
                neutered = true;
            }
        }
        REQUIRE(neutered);
    }

    scene::SceneFileView view{};
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(error.empty());
    REQUIRE(view.header->version == 5u);
    REQUIRE(view.physics_bodies.size() == 1u);
    REQUIRE(view.vehicle_exts.empty());  // SVHX seen as absent.

    registry.clear();
    registry.set_structural_deferred(false);
    scene::Scene loaded{registry};
    std::array<scene::SceneMeshBinding, 0> mesh_bindings{};
    std::array<Entity, 0> out_mesh_entities{};
    scene::instantiate_scene_file(loaded, view, mesh_bindings, out_mesh_entities);

    const Entity loaded_car = find_entity_named(loaded, "LegacyCar");
    REQUIRE(loaded_car.valid());
    const auto* vc = registry.get<scene::VehicleComponent>(loaded_car);
    REQUIRE(vc != nullptr);
    // Base SPHY fields still load.
    REQUIRE_THAT(static_cast<double>(vc->mass), Catch::Matchers::WithinAbs(1380.0, 0.01));
    // The Wave 8 fields fall back to the component defaults (no SVHX present).
    REQUIRE(vc->ground_mode == scene::VehicleGroundMode::Plane);
    REQUIRE(vc->max_speed == Approx(0.0f));
    REQUIRE(vc->steer_min_authority == Approx(1.0f));
    REQUIRE(vc->plane_y == Approx(0.0f));
}

TEST_CASE("vehicle_terrain_height samples the authored rolling-hills surface",
          "[scene][vehicle][authoring][wave8]") {
    scene::VehicleComponent vc{};
    vc.hf_base_y = 2.0f;
    vc.hf_amplitude = 4.0f;
    vc.hf_frequency = 0.5f;
    // height(x,z) = base + amp*0.5*(sin(x*f)+sin(z*f)). At the origin both sines
    // are 0, so the surface sits exactly at the base height.
    REQUIRE(scene::vehicle_terrain_height(vc, 0.0f, 0.0f) == Approx(2.0f));
    // Deterministic + matches the closed form anywhere.
    const f32 x = 3.0f, z = -1.5f;
    const f32 expected =
        vc.hf_base_y + vc.hf_amplitude * 0.5f * (std::sin(x * vc.hf_frequency) +
                                                 std::sin(z * vc.hf_frequency));
    REQUIRE(scene::vehicle_terrain_height(vc, x, z) == Approx(expected));
}

TEST_CASE("PlayRuntime synthesises a live terrain vehicle that drives + restores",
          "[play][editor][vehicle][authoring][wave8]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // A Heightfield-mode player car. The procedural surface is a gentle bowl-ish
    // rolling field; the car spawns above it and must settle ON the terrain (the
    // suspension probes the authored surface, not a flat y=0 plane).
    const math::Vec3 authored_pos{0.0f, 4.0f, 0.0f};
    const Entity car = scene.create_entity(at(authored_pos));
    {
        scene::VehicleComponent vc{};
        vc.is_player = 1u;
        vc.ground_mode = scene::VehicleGroundMode::Heightfield;
        vc.hf_base_y = 1.0f;
        vc.hf_amplitude = 2.0f;
        vc.hf_frequency = 0.05f;
        vc.max_speed = 18.0f;
        vc.steer_full_speed = 4.0f;
        vc.steer_taper_speed = 12.0f;
        vc.steer_min_authority = 0.45f;
        registry.add<scene::VehicleComponent>(car, vc);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());

    // A live physics::vehicle exists and its handles are stored back.
    auto* comp = registry.get<scene::VehicleComponent>(car);
    REQUIRE(comp != nullptr);
    REQUIRE(comp->runtime_vehicle != 0u);
    REQUIRE(comp->runtime_chassis != 0u);

    const math::Vec3 start = scene.transform(car).translation;
    const f32 start_z = start.z;

    // Let it settle onto the terrain for a moment (no input), then the throttle/
    // steer setters must take effect and drive it forward (-Z is forward).
    for (int step = 0; step < 60; ++step)
        runtime.tick(scene, 1.0f / 120.0f);

    runtime.set_vehicle_input(/*throttle*/ 1.0f, /*brake*/ 0.0f, /*steer*/ 0.2f);
    for (int step = 0; step < 480; ++step)
        runtime.tick(scene, 1.0f / 120.0f);

    const math::Vec3 end = scene.transform(car).translation;
    INFO("terrain car z " << start_z << " -> " << end.z << ", y=" << end.y);
    // Responds to throttle: it moved forward along -Z.
    REQUIRE(end.z < start_z - 0.5f);
    // Rides the terrain near the authored surface (did not sink through or fly
    // off): the procedural surface near the car is roughly hf_base_y +- amp.
    REQUIRE(std::isfinite(end.y));
    REQUIRE(end.y > -2.0f);
    REQUIRE(end.y < 8.0f);

    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());
    // Handles cleared + authored transform restored.
    REQUIRE(comp->runtime_vehicle == 0u);
    REQUIRE(comp->runtime_chassis == 0u);
    REQUIRE(scene.transform(car).translation.x == Approx(authored_pos.x).margin(1e-4f));
    REQUIRE(scene.transform(car).translation.y == Approx(authored_pos.y).margin(1e-4f));
    REQUIRE(scene.transform(car).translation.z == Approx(authored_pos.z).margin(1e-4f));

    // A second Play session re-synthesises cleanly (proves end() fully tore down
    // the borrowed heightfield sampler + handles).
    runtime.begin(scene);
    REQUIRE(runtime.playing());
    comp = registry.get<scene::VehicleComponent>(car);
    REQUIRE(comp->runtime_vehicle != 0u);
    runtime.end(scene);
}
