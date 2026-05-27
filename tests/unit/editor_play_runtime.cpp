// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE physics runtime tests (editor/play).
//
// Builds an in-memory scene::Scene, attaches RigidBodyComponent /
// CharacterControllerComponent to entities, and drives PlayRuntime through
// begin -> tick*N -> end. Verifies that:
//   * dynamic boxes fall under gravity and settle on a static ground body,
//   * the resolved poses land in each entity's TransformComponent column,
//   * end() restores the authored transforms and clears the runtime handles,
//   * a kinematic character walks horizontally when driven.

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
