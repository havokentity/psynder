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
        editor::play::RigidBodyComponent rb{};
        rb.shape = physics::Shape::Box;
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{20.0f, 0.5f, 20.0f};
        registry.add<editor::play::RigidBodyComponent>(ground, rb);
    }
    const f32 authored_y = 6.0f;
    const Entity box = scene.create_entity(at({0.0f, authored_y, 0.0f}));
    {
        editor::play::RigidBodyComponent rb{};
        rb.shape = physics::Shape::Box;
        rb.mass = 1.0f;
        rb.half_extent = math::Vec3{0.5f, 0.5f, 0.5f};
        registry.add<editor::play::RigidBodyComponent>(box, rb);
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
        editor::play::RigidBodyComponent rb{};
        rb.shape = physics::Shape::Box;
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{0.5f, 0.5f, 0.5f};  // mesh unit-cube bounds
        registry.add<editor::play::RigidBodyComponent>(floor, rb);
    }
    // Dynamic box dropped OFF-CENTER (x = 1.2): inside the scaled 1.5 half-width
    // floor, but OUTSIDE the buggy unscaled 0.5 cube. With the scale fix it
    // lands; without it, it misses the 1 m cube and falls to -inf.
    const f32 drop_y = 3.0f;
    const Entity box = scene.create_entity(at({1.2f, drop_y, 0.0f}));
    {
        editor::play::RigidBodyComponent rb{};
        rb.shape = physics::Shape::Box;
        rb.mass = 1.0f;
        rb.half_extent = math::Vec3{0.5f, 0.5f, 0.5f};
        registry.add<editor::play::RigidBodyComponent>(box, rb);
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
    REQUIRE(registry.get<editor::play::RigidBodyComponent>(box)->body.valid());
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

TEST_CASE("PlayRuntime writes a parented dynamic body back in parent-local space",
          "[play][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // Static ground at y = 0 (top surface y = 0.5), top-level.
    const Entity ground = scene.create_entity(at({0.0f, 0.0f, 0.0f}));
    {
        editor::play::RigidBodyComponent rb{};
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{20.0f, 0.5f, 20.0f};
        registry.add<editor::play::RigidBodyComponent>(ground, rb);
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
        editor::play::RigidBodyComponent rb{};
        rb.mass = 1.0f;
        rb.half_extent = math::Vec3{0.5f, 0.5f, 0.5f};
        rb.friction = 0.6f;
        registry.add<editor::play::RigidBodyComponent>(box, rb);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());
    REQUIRE(registry.get<editor::play::RigidBodyComponent>(box)->body.valid());

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
    REQUIRE_FALSE(registry.get<editor::play::RigidBodyComponent>(box)->body.valid());
}

TEST_CASE("PlayRuntime creates and simulates more than 256 bodies in one archetype",
          "[play][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};

    // Static ground.
    const Entity ground = scene.create_entity(at({0.0f, 0.0f, 0.0f}));
    {
        editor::play::RigidBodyComponent rb{};
        rb.mass = 0.0f;
        rb.half_extent = math::Vec3{200.0f, 0.5f, 200.0f};
        registry.add<editor::play::RigidBodyComponent>(ground, rb);
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
        editor::play::RigidBodyComponent rb{};
        rb.mass = 1.0f;
        rb.half_extent = math::Vec3{0.4f, 0.4f, 0.4f};
        rb.friction = 0.6f;
        registry.add<editor::play::RigidBodyComponent>(box, rb);
        boxes.push_back(box);
        authored_y.push_back(y);
    }

    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());
    // ground + every one of the 300 boxes got a live body.
    REQUIRE(runtime.body_count() == kBoxes + 1u);
    for (const Entity box : boxes)
        REQUIRE(registry.get<editor::play::RigidBodyComponent>(box)->body.valid());

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
        REQUIRE_FALSE(registry.get<editor::play::RigidBodyComponent>(box)->body.valid());
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
