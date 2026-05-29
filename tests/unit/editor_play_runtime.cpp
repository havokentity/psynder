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

#include "ai/AiComponents.h"
#include "editor/play/PlayRuntime.h"
#include "gameplay/GameplayComponents.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"
#include "script/psygraph/Bytecode.h"
#include "script/psygraph/Graph.h"
#include "script/psygraph/NodeTypes.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace psynder;
using Catch::Approx;

namespace {

scene::LocalTransform at(math::Vec3 t) {
    scene::LocalTransform out{};
    out.translation = t;
    return out;
}

scene::LocalTransform scaled_at(math::Vec3 t, math::Vec3 s) {
    scene::LocalTransform out{};
    out.translation = t;
    out.scale = s;
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
        scene::RigidBodyComponent rb{};
        rb.shape = scene::ColliderShape::Box;
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{20.0f, 0.5f, 20.0f};
        registry.add<scene::RigidBodyComponent>(ground, rb);
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
        scene::RigidBodyComponent rb{};
        rb.shape = scene::ColliderShape::Box;
        rb.mass = 1.0f;
        rb.half_extent = math::Vec3{0.5f, 0.5f, 0.5f};
        rb.friction = 0.6f;
        registry.add<scene::RigidBodyComponent>(box, rb);
        boxes.push_back(box);
        authored_y.push_back(y);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);

    REQUIRE(runtime.playing());
    REQUIRE(runtime.body_count() == kBoxes + 1u);  // boxes + ground

    // Each box now has a live body handle written into its component.
    for (const Entity box : boxes)
        REQUIRE(registry.get<scene::RigidBodyComponent>(box)->runtime_body != 0u);

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
        REQUIRE_FALSE(registry.get<scene::RigidBodyComponent>(boxes[i])->runtime_body != 0u);
    }
    REQUIRE_FALSE(registry.get<scene::RigidBodyComponent>(ground)->runtime_body != 0u);
}

TEST_CASE("PlayRuntime syncs simulated poses into the SceneGraph (renderer source)",
          "[play][editor]") {
    // Regression: the writeback updated only TransformComponent, but the
    // renderer reads SceneGraph world matrices (recomputed from the graph's own
    // local store, NOT TransformComponent), so simulated bodies rendered at
    // their authored pose. tick() must push the simulated local into the graph.
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    const Entity ground = scene.create_entity(at({0.0f, 0.0f, 0.0f}));
    {
        scene::RigidBodyComponent rb{};
        rb.shape = scene::ColliderShape::Box;
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{20.0f, 0.5f, 20.0f};
        registry.add<scene::RigidBodyComponent>(ground, rb);
    }
    const f32 authored_y = 6.0f;
    const Entity box = scene.create_entity(at({0.0f, authored_y, 0.0f}));
    {
        scene::RigidBodyComponent rb{};
        rb.shape = scene::ColliderShape::Box;
        rb.mass = 1.0f;
        rb.half_extent = math::Vec3{0.5f, 0.5f, 0.5f};
        registry.add<scene::RigidBodyComponent>(box, rb);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    for (int s = 0; s < 300; ++s)
        runtime.tick(scene, 1.0f / 120.0f);

    // The graph world matrix (what the renderer reads) must reflect the fall.
    const math::Mat4 w = scene.graph().world_matrix(scene.node(box));
    const f32 graph_y = w.m[13];  // column-major translation.y
    INFO("graph world y = " << graph_y << " authored " << authored_y);
    REQUIRE(graph_y < authored_y - 0.5f);  // fell, per the graph
    REQUIRE(graph_y > 0.5f);               // rests above the ground top
    // ...and the graph agrees with the TransformComponent the body wrote.
    REQUIRE(graph_y == Approx(entity_y(scene, box)).margin(1e-3f));
    runtime.end(scene);
}

TEST_CASE("PlayRuntime: collider honors entity scale + re-simulates on replay",
          "[play][editor]") {
    // Regression for the interactive-test fall-through: the collider was sized
    // from the mesh's unit bounds and ignored the entity scale, so a floor
    // authored via scale got a 1 m cube collider and dynamic bodies missed it.
    // Also covers "subsequent plays need re-add" -> a clean begin/end/begin
    // must re-simulate without re-adding the component.
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // Wide, thick STATIC floor authored via SCALE on a unit-cube mesh:
    // scale (3, 0.5, 3) -> world collider half-extent (1.5, 0.25, 1.5).
    const Entity floor = scene.create_entity(scaled_at({0.0f, 0.0f, 0.0f}, {3.0f, 0.5f, 3.0f}));
    {
        scene::RigidBodyComponent rb{};
        rb.shape = scene::ColliderShape::Box;
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{0.5f, 0.5f, 0.5f};  // mesh unit-cube bounds
        registry.add<scene::RigidBodyComponent>(floor, rb);
    }
    // Dynamic box dropped OFF-CENTER (x = 1.2): inside the scaled 1.5 half-width
    // floor, but OUTSIDE the buggy unscaled 0.5 cube. With the scale fix it
    // lands; without it, it misses the 1 m cube and falls to -inf.
    const f32 drop_y = 3.0f;
    const Entity box = scene.create_entity(at({1.2f, drop_y, 0.0f}));
    {
        scene::RigidBodyComponent rb{};
        rb.shape = scene::ColliderShape::Box;
        rb.mass = 1.0f;
        rb.half_extent = math::Vec3{0.5f, 0.5f, 0.5f};
        registry.add<scene::RigidBodyComponent>(box, rb);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    for (int s = 0; s < 480; ++s)
        runtime.tick(scene, 1.0f / 120.0f);
    const f32 y1 = entity_y(scene, box);
    INFO("first play settled y = " << y1);
    REQUIRE(std::isfinite(y1));
    REQUIRE(y1 > 0.0f);            // rested ON the scaled floor (no fall-through)
    REQUIRE(y1 < drop_y - 0.5f);   // it fell

    runtime.end(scene);
    REQUIRE(entity_y(scene, box) == Approx(drop_y).margin(1e-3f));  // restored

    // Replay WITHOUT re-adding the component: it must simulate again.
    runtime.begin(scene);
    REQUIRE(registry.get<scene::RigidBodyComponent>(box)->runtime_body != 0u);
    for (int s = 0; s < 480; ++s)
        runtime.tick(scene, 1.0f / 120.0f);
    const f32 y2 = entity_y(scene, box);
    INFO("second play settled y = " << y2);
    REQUIRE(y2 > 0.0f);
    REQUIRE(y2 < drop_y - 0.5f);
    REQUIRE(y2 == Approx(y1).margin(0.3f));  // re-sim matches first play
    runtime.end(scene);
}

TEST_CASE("PlayRuntime drives a kinematic character horizontally", "[play][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // A static floor so the capsule has something to stand on.
    const Entity floor = scene.create_entity(at({0.0f, -0.5f, 0.0f}));
    {
        scene::RigidBodyComponent rb{};
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{20.0f, 0.5f, 20.0f};
        registry.add<scene::RigidBodyComponent>(floor, rb);
    }

    const Entity actor = scene.create_entity(at({0.0f, 1.0f, 0.0f}));
    {
        scene::CharacterControllerComponent cc{};
        cc.height = 1.8f;
        cc.radius = 0.35f;
        cc.move_speed = 4.0f;
        registry.add<scene::CharacterControllerComponent>(actor, cc);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(registry.get<scene::CharacterControllerComponent>(actor)->runtime_character != 0u);

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
    REQUIRE_FALSE(registry.get<scene::CharacterControllerComponent>(actor)->runtime_character != 0u);
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
        scene::VehicleComponent vc{};
        vc.is_player = true;
        registry.add<scene::VehicleComponent>(car, vc);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());

    auto* comp = registry.get<scene::VehicleComponent>(car);
    REQUIRE(comp->runtime_vehicle != 0u);
    REQUIRE(comp->runtime_chassis != 0u);

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
    REQUIRE_FALSE(comp->runtime_vehicle != 0u);
    REQUIRE_FALSE(comp->runtime_chassis != 0u);
    REQUIRE(scene.transform(car).translation.x == Approx(authored_pos.x).margin(1e-4f));
    REQUIRE(scene.transform(car).translation.y == Approx(authored_pos.y).margin(1e-4f));
    REQUIRE(scene.transform(car).translation.z == Approx(authored_pos.z).margin(1e-4f));
}

TEST_CASE("PlayRuntime writes a parented dynamic body back in parent-local space",
          "[play][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // Static ground at y = 0 (top surface y = 0.5), top-level.
    const Entity ground = scene.create_entity(at({0.0f, 0.0f, 0.0f}));
    {
        scene::RigidBodyComponent rb{};
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{20.0f, 0.5f, 20.0f};
        registry.add<scene::RigidBodyComponent>(ground, rb);
    }

    // A parent node MOVED far from the origin and YAWED 90 degrees, so its world
    // matrix is a non-trivial rigid transform. The parent itself is inert.
    const math::Vec3 parent_pos{10.0f, 0.0f, -7.0f};
    scene::LocalTransform parent_local{};
    parent_local.translation = parent_pos;
    // Yaw 90 degrees about the world up (Y) axis: a non-trivial rotation that
    // leaves the Y axis fixed, so a local +Y offset stays +Y in world space.
    parent_local.rotation = math::quat_from_axis_angle(math::Vec3{0.0f, 1.0f, 0.0f}, math::kHalfPi);
    const Entity parent = scene.create_entity(parent_local);
    scene.graph().update_world_transforms();  // parent world matrix ready

    // A dynamic box parented under the moved/rotated parent. Its authored LOCAL
    // places it 4 m above the parent; its authored WORLD point is therefore
    // parent_world * (0,4,0).
    const math::Mat4 parent_world = scene.graph().world_matrix(scene.node(parent));
    const math::Vec4 box_world4 =
        math::mul(parent_world, math::Vec4{0.0f, 4.0f, 0.0f, 1.0f});
    const math::Vec3 box_world0{box_world4.x, box_world4.y, box_world4.z};

    const Entity box = scene.create_entity(at({0.0f, 4.0f, 0.0f}), scene.node(parent));
    {
        scene::RigidBodyComponent rb{};
        rb.mass = 1.0f;
        rb.half_extent = math::Vec3{0.5f, 0.5f, 0.5f};
        rb.friction = 0.6f;
        registry.add<scene::RigidBodyComponent>(box, rb);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());
    REQUIRE(registry.get<scene::RigidBodyComponent>(box)->runtime_body != 0u);

    // The body was created from the WORLD pose (scene.transform composes through
    // the parent), so it starts at box_world0 -- confirm the authored world.
    {
        const math::Mat4 w0 = scene.graph().world_matrix(scene.node(box));
        INFO("authored world start = (" << w0.m[12] << "," << w0.m[13] << "," << w0.m[14]
                                        << ") expected (" << box_world0.x << "," << box_world0.y
                                        << "," << box_world0.z << ")");
        REQUIRE(w0.m[12] == Approx(box_world0.x).margin(1e-3f));
        REQUIRE(w0.m[13] == Approx(box_world0.y).margin(1e-3f));
        REQUIRE(w0.m[14] == Approx(box_world0.z).margin(1e-3f));
    }

    // Settle.
    for (int step = 0; step < 600; ++step)
        runtime.tick(scene, 1.0f / 120.0f);

    // The writeback stores the body's WORLD pose folded into the parent's local
    // space directly into TransformComponent.local. Recompose the WORLD pose the
    // way the renderer does (parent_world * local) and confirm it matches a free
    // body: same world X/Z, resting just above the ground top. (If the writeback
    // wrongly stored the world pose AS the local -- the old top-level-only path
    // -- recomposing through the moved/rotated parent would fling it far away;
    // this is the discriminating assertion.)
    const scene::LocalTransform lf = scene.transform(box);
    const math::Mat4 wf =
        math::mul(parent_world, scene::local_transform_matrix(lf));
    const math::Vec3 world_final{wf.m[12], wf.m[13], wf.m[14]};
    INFO("box LOCAL after settle = (" << lf.translation.x << "," << lf.translation.y << ","
                                      << lf.translation.z << ")");
    INFO("box WORLD (parent_world * local) = (" << world_final.x << "," << world_final.y << ","
                                                << world_final.z << ")");
    REQUIRE(std::isfinite(world_final.x));
    REQUIRE(std::isfinite(world_final.y));
    REQUIRE(std::isfinite(world_final.z));
    // Stayed near the authored world X/Z (minor physics drift while settling);
    // the buggy world-as-local path would land >10 units away through the
    // moved/rotated parent, so this margin is still strongly discriminating.
    REQUIRE(world_final.x == Approx(box_world0.x).margin(0.75f));
    REQUIRE(world_final.z == Approx(box_world0.z).margin(0.75f));
    REQUIRE(world_final.y < box_world0.y - 1.0f);  // it fell
    REQUIRE(world_final.y > 0.5f);                 // rests above the ground top
    REQUIRE(world_final.y < 1.5f);                 // ground_top(0.5)+half(0.5), settled

    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());
    // Authored local transform restored exactly (it never moved in parent space
    // because end() restores the snapshot, regardless of the parent).
    const scene::LocalTransform restored = scene.transform(box);
    REQUIRE(restored.translation.x == Approx(0.0f).margin(1e-4f));
    REQUIRE(restored.translation.y == Approx(4.0f).margin(1e-4f));
    REQUIRE(restored.translation.z == Approx(0.0f).margin(1e-4f));
    REQUIRE_FALSE(registry.get<scene::RigidBodyComponent>(box)->runtime_body != 0u);
}

TEST_CASE("PlayRuntime creates and simulates more than 256 bodies in one archetype",
          "[play][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // Static ground.
    const Entity ground = scene.create_entity(at({0.0f, 0.0f, 0.0f}));
    {
        scene::RigidBodyComponent rb{};
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{200.0f, 0.5f, 200.0f};
        registry.add<scene::RigidBodyComponent>(ground, rb);
    }

    // 300 dynamic boxes -- one ECS chunk holds at most 256 rows (16 KiB / cache
    // line ceiling), so this archetype spans multiple chunks. The old fixed
    // std::array<,256> + break would silently skip every row past 256 in a
    // chunk; with the fix every body must be created and must fall.
    constexpr usize kBoxes = 300u;
    std::vector<Entity> boxes;
    std::vector<f32> authored_y;
    boxes.reserve(kBoxes);
    authored_y.reserve(kBoxes);
    for (usize i = 0; i < kBoxes; ++i) {
        // Spread out on a grid so they don't all collide into one stack.
        const f32 x = static_cast<f32>(i % 30u) * 2.0f - 30.0f;
        const f32 z = static_cast<f32>(i / 30u) * 2.0f - 10.0f;
        const f32 y = 6.0f;
        const Entity box = scene.create_entity(at({x, y, z}));
        scene::RigidBodyComponent rb{};
        rb.mass = 1.0f;
        rb.half_extent = math::Vec3{0.4f, 0.4f, 0.4f};
        rb.friction = 0.6f;
        registry.add<scene::RigidBodyComponent>(box, rb);
        boxes.push_back(box);
        authored_y.push_back(y);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());
    // ground + every one of the 300 boxes got a live body.
    REQUIRE(runtime.body_count() == kBoxes + 1u);
    for (const Entity box : boxes)
        REQUIRE(registry.get<scene::RigidBodyComponent>(box)->runtime_body != 0u);

    for (int step = 0; step < 360; ++step)
        runtime.tick(scene, 1.0f / 120.0f);

    // EVERY box simulated: each fell below its authored height and rests above
    // the ground top. A dropped (uncreated) body would still sit at its authored
    // y = 6.0, so this catches the silent-truncation regression for all rows.
    usize fell = 0u;
    for (usize i = 0; i < kBoxes; ++i) {
        const f32 y = entity_y(scene, boxes[i]);
        REQUIRE(std::isfinite(y));
        REQUIRE(y > 0.5f);  // never below the ground top
        if (y < authored_y[i] - 1.0f)
            ++fell;
    }
    INFO("boxes that fell = " << fell << " / " << kBoxes);
    REQUIRE(fell == kBoxes);

    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());
    for (const Entity box : boxes)
        REQUIRE_FALSE(registry.get<scene::RigidBodyComponent>(box)->runtime_body != 0u);
}

TEST_CASE("PlayRuntime flies a player helicopter up and yaws it, restores on stop",
          "[play][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    const math::Vec3 authored_pos{0.0f, 5.0f, 0.0f};
    const Entity heli = scene.create_entity(at(authored_pos));
    {
        scene::HelicopterComponent hc{};
        hc.is_player = true;
        registry.add<scene::HelicopterComponent>(heli, hc);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());

    auto* comp = registry.get<scene::HelicopterComponent>(heli);
    REQUIRE(comp->runtime_body != 0u);

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
    REQUIRE_FALSE(comp->runtime_body != 0u);
    // Authored transform restored.
    REQUIRE(scene.transform(heli).translation.x == Approx(authored_pos.x).margin(1e-4f));
    REQUIRE(scene.transform(heli).translation.y == Approx(authored_pos.y).margin(1e-4f));
    REQUIRE(scene.transform(heli).translation.z == Approx(authored_pos.z).margin(1e-4f));
}

// ─── GAMEPLAY PHASE: AI combat during Play ─────────────────────────────────
namespace {

// Spawn a stationary AI enemy: AiAgent (omni-FOV, fires every tick once cooled),
// Health+Faction (faction 2), a sphere Hitbox, and a hitscan Weapon + runtime.
Entity spawn_ai_enemy(scene::Scene& scene, math::Vec3 pos, u32 faction, f32 damage) {
    auto& reg = scene.registry();
    const Entity e = scene.create_entity(at(pos));

    scene::HealthComponent health{};
    health.max_health = 100.0f;
    health.current_health = 100.0f;
    health.faction = faction;
    reg.add<scene::HealthComponent>(e, health);

    gameplay::FactionComponent fac{};
    fac.faction = faction;
    reg.add<gameplay::FactionComponent>(e, fac);

    gameplay::HitboxComponent hb{};
    hb.radius = 0.5f;
    reg.add<gameplay::HitboxComponent>(e, gameplay::sanitize_hitbox(hb));

    scene::WeaponComponent weapon{};
    weapon.damage = damage;
    weapon.range = 100.0f;
    weapon.fire_rate = 10.0f;  // 10 shots/sec -> cooldown 0.1s between shots
    weapon.ammo = 10000u;
    reg.add<scene::WeaponComponent>(e, weapon);

    gameplay::WeaponRuntimeComponent rt{};
    rt.kind = gameplay::WeaponKind::Hitscan;
    reg.add<gameplay::WeaponRuntimeComponent>(e, gameplay::sanitize_weapon_runtime(rt));

    ai::AiAgentComponent agent{};
    agent.state = ai::AiState::Idle;
    agent.sight_range = 50.0f;
    agent.fov_cos = -1.0f;       // omnidirectional => deterministic acquisition
    agent.attack_range = 50.0f;  // always in attack range within sight
    agent.think_cooldown = 0.0f;
    agent.think_interval = 0.0f;  // re-evaluate every tick
    agent.move_speed = 0.0f;      // stationary turret
    reg.add<ai::AiAgentComponent>(e, ai::sanitize_ai_agent(agent));
    reg.add<ai::PerceptionComponent>(e, ai::PerceptionComponent{});
    return e;
}

// Spawn the player target: Health+Faction (faction 1) + a sphere Hitbox so the
// AI hitscan can resolve a hit against it.
Entity spawn_player_target(scene::Scene& scene, math::Vec3 pos, u32 faction, f32 hp) {
    auto& reg = scene.registry();
    const Entity e = scene.create_entity(at(pos));
    scene::HealthComponent health{};
    health.max_health = hp;
    health.current_health = hp;
    health.faction = faction;
    reg.add<scene::HealthComponent>(e, health);
    gameplay::HitboxComponent hb{};
    hb.radius = 0.5f;
    reg.add<gameplay::HitboxComponent>(e, gameplay::sanitize_hitbox(hb));
    return e;
}

}  // namespace

TEST_CASE("PlayRuntime gameplay: an AI enemy fires on a player target and kills it",
          "[play][editor][gameplay]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // Enemy (faction 2) at origin, player target (faction 1) 6 m away. No
    // physics bodies between them => the AI LOS raycast against world_ is clear.
    const Entity enemy = spawn_ai_enemy(scene, {0.0f, 0.0f, 0.0f}, /*faction*/ 2u,
                                        /*damage*/ 5.0f);
    const f32 target_hp = 100.0f;
    const Entity player = spawn_player_target(scene, {6.0f, 0.0f, 0.0f}, /*faction*/ 1u, target_hp);

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());

    // No shots before any tick.
    REQUIRE(runtime.ai_shots_fired() == 0u);

    // Tick a while: the AI should acquire the hostile, reach Attack, and fire
    // (rate-limited by the weapon cooldown). Health drops; eventually the target
    // dies and is despawned, after which the AI loses its target and stops.
    const f32 dt = 1.0f / 60.0f;
    bool player_alive = true;
    for (int step = 0; step < 2000 && player_alive; ++step) {
        runtime.tick(scene, dt);
        player_alive = registry.alive(player);
    }

    // The AI fired at least once (the running tally counts resolved hits).
    INFO("ai shots fired = " << runtime.ai_shots_fired());
    REQUIRE(runtime.ai_shots_fired() > 0u);

    // The target took damage and ultimately died (resolve_deaths despawned it).
    REQUIRE_FALSE(registry.alive(player));

    // The enemy itself never died (the target had no weapon to fight back).
    const auto* enemy_hp = registry.get<scene::HealthComponent>(enemy);
    REQUIRE(enemy_hp != nullptr);
    REQUIRE(enemy_hp->current_health == Approx(100.0f));

    // Once the target is gone, the AI stops firing: record the tally, tick more,
    // and confirm it did not climb (no live hostile to engage).
    const u32 shots_after_kill = runtime.ai_shots_fired();
    for (int step = 0; step < 300; ++step)
        runtime.tick(scene, dt);
    REQUIRE(runtime.ai_shots_fired() == shots_after_kill);

    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());
}

TEST_CASE("PlayRuntime gameplay: a wall between enemy and target blocks AI fire",
          "[play][editor][gameplay]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // Enemy (faction 2) at origin, target (faction 1) 10 m along +X.
    const Entity enemy = spawn_ai_enemy(scene, {0.0f, 0.0f, 0.0f}, 2u, /*damage*/ 5.0f);
    (void)enemy;
    const f32 target_hp = 100.0f;
    const Entity player = spawn_player_target(scene, {10.0f, 0.0f, 0.0f}, 1u, target_hp);

    // A STATIC physics wall (rigid body, mass 0) sitting between them at x=5.
    // The AI LOS raycast against world_ hits this body before the target => the
    // line of sight is blocked, so the AI never reaches Attack / fires.
    const Entity wall = scene.create_entity(at({5.0f, 0.0f, 0.0f}));
    {
        scene::RigidBodyComponent rb{};
        rb.shape = scene::ColliderShape::Box;
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{0.5f, 3.0f, 3.0f};  // tall slab across the line
        registry.add<scene::RigidBodyComponent>(wall, rb);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());

    const f32 dt = 1.0f / 60.0f;
    for (int step = 0; step < 600; ++step)
        runtime.tick(scene, dt);

    // LOS was blocked every tick => no shots, target untouched and alive.
    REQUIRE(runtime.ai_shots_fired() == 0u);
    REQUIRE(registry.alive(player));
    const auto* hp = registry.get<scene::HealthComponent>(player);
    REQUIRE(hp != nullptr);
    REQUIRE(hp->current_health == Approx(target_hp));

    runtime.end(scene);
}

// ─── GAMEPLAY PHASE: PsyGraph OnTick during Play ───────────────────────────
namespace {

// Build a graph whose OnTick calls ApplyDamage(<self>, amount): the Target pin
// is left unconnected (defaults to entity 0) so the play runtime's host
// substitutes the running graph's own entity (the documented self fallback).
script::psygraph::Graph build_ontick_self_damage_graph(double amount) {
    using namespace script::psygraph;
    Graph g;
    g.variable_count = 0;

    auto add_node = [&](NodeTypeId type, std::vector<u64> params = {}) -> NodeIndex {
        Node n;
        n.type = type;
        n.params = std::move(params);
        g.nodes.push_back(std::move(n));
        return static_cast<NodeIndex>(g.nodes.size() - 1);
    };
    auto exec_edge = [&](NodeIndex from, u16 pin, NodeIndex to) {
        Edge e;
        e.kind = EdgeKind::Exec;
        e.from_node = from;
        e.from_pin = pin;
        e.to_node = to;
        g.edges.push_back(e);
    };
    auto data_edge = [&](NodeIndex from, u16 from_pin, NodeIndex to, u16 to_pin) {
        Edge e;
        e.kind = EdgeKind::Data;
        e.from_node = from;
        e.from_pin = from_pin;
        e.to_node = to;
        e.to_pin = to_pin;
        g.edges.push_back(e);
    };
    auto float_bits = [](double v) -> u64 {
        u64 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    };

    const NodeIndex on_tick = add_node(NodeTypeId::OnTick);
    const NodeIndex amt = add_node(NodeTypeId::LiteralFloat, {float_bits(amount)});
    const NodeIndex dmg = add_node(NodeTypeId::ApplyDamage);
    // ApplyDamage pins: 0 = Target (Entity, left unconnected => self), 1 = Amount.
    data_edge(amt, 0, dmg, 1);
    exec_edge(on_tick, 0, dmg);
    return g;
}

}  // namespace

TEST_CASE("PlayRuntime gameplay: a PsyGraph OnTick action runs during Play",
          "[play][editor][gameplay][psygraph]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // An entity that simply carries health; its graph drains it each OnTick.
    const f32 start_hp = 100.0f;
    const Entity scripted = scene.create_entity(at({0.0f, 0.0f, 0.0f}));
    {
        scene::HealthComponent health{};
        health.max_health = start_hp;
        health.current_health = start_hp;
        health.faction = 0u;
        registry.add<scene::HealthComponent>(scripted, health);
    }

    editor::play::PlayRuntime runtime;
    // Bind the OnTick->ApplyDamage(self, 2) graph BEFORE play begins.
    const script::psygraph::Graph graph = build_ontick_self_damage_graph(/*amount*/ 2.0);
    REQUIRE(runtime.bind_psygraph(scene, scripted, graph));
    // Binding twice on the same entity is rejected.
    REQUIRE_FALSE(runtime.bind_psygraph(scene, scripted, graph));

    runtime.begin(scene);
    REQUIRE(runtime.playing());

    // Tick a handful of times: each OnTick applies 2 damage to self.
    constexpr int kTicks = 10;
    const f32 dt = 1.0f / 60.0f;
    for (int step = 0; step < kTicks; ++step)
        runtime.tick(scene, dt);

    const auto* hp = registry.get<scene::HealthComponent>(scripted);
    REQUIRE(hp != nullptr);
    INFO("scripted health after " << kTicks << " ticks = " << hp->current_health);
    // The OnTick action ran every tick => health dropped by 2 * kTicks (clamped
    // at 0). With 100 hp and 20 total damage it lands at 80.
    REQUIRE(hp->current_health == Approx(start_hp - 2.0f * static_cast<f32>(kTicks)));

    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());
}
