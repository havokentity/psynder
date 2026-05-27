// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE physics runtime tests (editor/play).
//
// Builds an in-memory scene::Scene, attaches RigidBodyComponent /
// CharacterControllerComponent / VehicleComponent to entities, and drives
// PlayRuntime through begin -> tick*N -> end. Verifies that:
//   * dynamic boxes fall under gravity and settle on a static ground body,
//   * the resolved poses land in each entity's TransformComponent column,
//   * end() restores the authored transforms and clears the runtime handles,
//   * a kinematic character walks horizontally when driven,
//   * a player vehicle drives forward under throttle, stays near the ground,
//     and restores its authored transform + clears its handles on stop.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "editor/play/PlayRuntime.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"

#include <cmath>
#include <vector>

using namespace psynder;
using Catch::Approx;

namespace {

scene::LocalTransform at(math::Vec3 t) {
    scene::LocalTransform out{};
    out.translation = t;
    return out;
}

struct RegistryReset {
    RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

f32 entity_y(scene::Scene& scene, Entity e) {
    return scene.transform(e).translation.y;
}

}  // namespace

TEST_CASE("PlayRuntime drops dynamic boxes onto a static ground and restores on stop",
          "[play][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // Static ground: a wide, thin box at y = 0. mass 0 => static.
    const Entity ground = scene.create_entity(at({0.0f, 0.0f, 0.0f}));
    {
        editor::play::RigidBodyComponent rb{};
        rb.shape = physics::Shape::Box;
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{20.0f, 0.5f, 20.0f};
        registry.add<editor::play::RigidBodyComponent>(ground, rb);
    }

    // A few dynamic boxes stacked above the ground (mass > 0).
    constexpr usize kBoxes = 4u;
    std::vector<Entity> boxes;
    std::vector<f32> authored_y;
    boxes.reserve(kBoxes);
    authored_y.reserve(kBoxes);
    for (usize i = 0; i < kBoxes; ++i) {
        const f32 y = 5.0f + static_cast<f32>(i) * 2.0f;
        const Entity box = scene.create_entity(at({0.0f, y, 0.0f}));
        editor::play::RigidBodyComponent rb{};
        rb.shape = physics::Shape::Box;
        rb.mass = 1.0f;
        rb.half_extent = math::Vec3{0.5f, 0.5f, 0.5f};
        rb.friction = 0.6f;
        registry.add<editor::play::RigidBodyComponent>(box, rb);
        boxes.push_back(box);
        authored_y.push_back(y);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);

    REQUIRE(runtime.playing());
    REQUIRE(runtime.body_count() == kBoxes + 1u);  // boxes + ground

    // Each box now has a live body handle written into its component.
    for (const Entity box : boxes)
        REQUIRE(registry.get<editor::play::RigidBodyComponent>(box)->body.valid());

    // Simulate a couple seconds of fixed steps so everything settles.
    for (int step = 0; step < 600; ++step)
        runtime.tick(scene, 1.0f / 120.0f);

    // The static ground never moved.
    REQUIRE(entity_y(scene, ground) == Approx(0.0f).margin(1e-3f));

    // Every box fell well below its authored height and settled at rest ABOVE
    // the ground surface (ground top = 0.5; box half-extent = 0.5 => centres
    // rest at y >= ~1.0 when stacked, never penetrating below the ground top).
    for (usize i = 0; i < kBoxes; ++i) {
        const f32 y = entity_y(scene, boxes[i]);
        INFO("box " << i << " settled y = " << y << " (authored " << authored_y[i] << ")");
        REQUIRE(y < authored_y[i] - 0.5f);  // it fell
        REQUIRE(y > 0.5f);                   // it rests above the ground top
        REQUIRE(std::isfinite(y));
    }

    // Capture pre-stop poses to confirm end() actually changes them back.
    const f32 box0_play_y = entity_y(scene, boxes[0]);
    REQUIRE(box0_play_y < authored_y[0] - 0.5f);

    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());

    // Authored transforms are restored and runtime handles cleared.
    REQUIRE(entity_y(scene, ground) == Approx(0.0f).margin(1e-4f));
    for (usize i = 0; i < kBoxes; ++i) {
        REQUIRE(entity_y(scene, boxes[i]) == Approx(authored_y[i]).margin(1e-4f));
        REQUIRE_FALSE(registry.get<editor::play::RigidBodyComponent>(boxes[i])->body.valid());
    }
    REQUIRE_FALSE(registry.get<editor::play::RigidBodyComponent>(ground)->body.valid());
}

TEST_CASE("PlayRuntime drives a kinematic character horizontally", "[play][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // A static floor so the capsule has something to stand on.
    const Entity floor = scene.create_entity(at({0.0f, -0.5f, 0.0f}));
    {
        editor::play::RigidBodyComponent rb{};
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{20.0f, 0.5f, 20.0f};
        registry.add<editor::play::RigidBodyComponent>(floor, rb);
    }

    const Entity actor = scene.create_entity(at({0.0f, 1.0f, 0.0f}));
    {
        editor::play::CharacterControllerComponent cc{};
        cc.height = 1.8f;
        cc.radius = 0.35f;
        cc.move_speed = 4.0f;
        registry.add<editor::play::CharacterControllerComponent>(actor, cc);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(registry.get<editor::play::CharacterControllerComponent>(actor)->character.valid());

    const f32 start_x = scene.transform(actor).translation.x;

    // Walk along +X for a while.
    runtime.set_character_input(scene, actor, math::Vec3{1.0f, 0.0f, 0.0f});
    for (int step = 0; step < 120; ++step)
        runtime.tick(scene, 1.0f / 120.0f);

    const f32 end_x = scene.transform(actor).translation.x;
    INFO("character moved from x=" << start_x << " to x=" << end_x);
    REQUIRE(end_x > start_x + 0.5f);  // it walked forward
    REQUIRE(std::isfinite(end_x));

    // The TransformComponent tracks the character store position.
    REQUIRE(end_x == Approx(runtime.character_position(scene, actor).x).margin(1e-4f));

    runtime.end(scene);
    REQUIRE_FALSE(registry.get<editor::play::CharacterControllerComponent>(actor)->character.valid());
    // Authored transform restored.
    REQUIRE(scene.transform(actor).translation.x == Approx(start_x).margin(1e-4f));
}

TEST_CASE("PlayRuntime drives a player vehicle forward and restores on stop",
          "[play][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // Player car sitting just above the flat ground plane (y=0). The chassis
    // half-extent is 0.4 in Y, so its centre rests around y ~= 0.4 + wheel/susp.
    const math::Vec3 authored_pos{0.0f, 1.0f, 0.0f};
    const Entity car = scene.create_entity(at(authored_pos));
    {
        editor::play::VehicleComponent vc{};
        vc.is_player = true;
        registry.add<editor::play::VehicleComponent>(car, vc);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());

    auto* comp = registry.get<editor::play::VehicleComponent>(car);
    REQUIRE(comp->vehicle.valid());
    REQUIRE(comp->chassis.valid());

    const math::Vec3 start = scene.transform(car).translation;
    // Forward is -Z in the authored (identity) frame.
    const f32 start_z = start.z;

    // Full throttle, no steer, for a couple seconds of fixed steps.
    runtime.set_vehicle_input(/*throttle*/ 1.0f, /*brake*/ 0.0f, /*steer*/ 0.0f);
    for (int step = 0; step < 480; ++step)
        runtime.tick(scene, 1.0f / 120.0f);

    const math::Vec3 end = scene.transform(car).translation;
    INFO("car moved z " << start_z << " -> " << end.z << ", y=" << end.y);
    // Drove forward along -Z (z decreased).
    REQUIRE(end.z < start_z - 0.5f);
    // Stayed near the ground (did not fly off or sink through the plane).
    REQUIRE(std::isfinite(end.y));
    REQUIRE(end.y > -0.5f);
    REQUIRE(end.y < 3.0f);

    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());
    // Handles cleared and authored transform restored.
    REQUIRE_FALSE(comp->vehicle.valid());
    REQUIRE_FALSE(comp->chassis.valid());
    REQUIRE(scene.transform(car).translation.x == Approx(authored_pos.x).margin(1e-4f));
    REQUIRE(scene.transform(car).translation.y == Approx(authored_pos.y).margin(1e-4f));
    REQUIRE(scene.transform(car).translation.z == Approx(authored_pos.z).margin(1e-4f));
}

TEST_CASE("PlayRuntime flies a player helicopter up and yaws it, restores on stop",
          "[play][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    const math::Vec3 authored_pos{0.0f, 5.0f, 0.0f};
    const Entity heli = scene.create_entity(at(authored_pos));
    {
        editor::play::HelicopterComponent hc{};
        hc.is_player = true;
        registry.add<editor::play::HelicopterComponent>(heli, hc);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());

    auto* comp = registry.get<editor::play::HelicopterComponent>(heli);
    REQUIRE(comp->body.valid());

    const f32 start_y = scene.transform(heli).translation.y;
    const math::Quat start_rot = scene.transform(heli).rotation;

    // Full ascend collective for a couple seconds: it should climb (max_thrust
    // exceeds m*g, so net upward force lifts it).
    runtime.set_helicopter_input(/*collective*/ 1.0f, 0.0f, 0.0f, 0.0f);
    for (int step = 0; step < 240; ++step)
        runtime.tick(scene, 1.0f / 120.0f);

    const f32 climbed_y = scene.transform(heli).translation.y;
    INFO("heli climbed from y=" << start_y << " to y=" << climbed_y);
    REQUIRE(climbed_y > start_y + 0.5f);
    REQUIRE(std::isfinite(climbed_y));

    // Now feed yaw with neutral collective (hover_assist holds altitude): the
    // orientation must change away from the authored identity rotation.
    runtime.set_helicopter_input(/*collective*/ 0.0f, 0.0f, 0.0f, /*yaw*/ 1.0f);
    for (int step = 0; step < 120; ++step)
        runtime.tick(scene, 1.0f / 120.0f);

    const math::Quat yawed = scene.transform(heli).rotation;
    // Quaternion dot near +/-1 means "same orientation"; require it to diverge.
    const f32 align = std::fabs(yawed.x * start_rot.x + yawed.y * start_rot.y +
                                yawed.z * start_rot.z + yawed.w * start_rot.w);
    INFO("orientation alignment with authored = " << align);
    REQUIRE(align < 0.999f);  // it rotated

    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());
    REQUIRE_FALSE(comp->body.valid());
    // Authored transform restored.
    REQUIRE(scene.transform(heli).translation.x == Approx(authored_pos.x).margin(1e-4f));
    REQUIRE(scene.transform(heli).translation.y == Approx(authored_pos.y).margin(1e-4f));
    REQUIRE(scene.transform(heli).translation.z == Approx(authored_pos.z).margin(1e-4f));
}
