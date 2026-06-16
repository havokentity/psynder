// SPDX-License-Identifier: MIT
// Psynder - LANE W11-1 Definition-of-Done checkpoint: an editor-authored PLAYABLE
// RACER level round-trips through a .psyscene and PLAYS under PlayRuntime, with
// NO per-feature C++. This completes the DoD trifecta: the terrain (Wave 9,
// tests/unit/editor_authored_level.cpp) and indoor-shooter (Wave 10,
// editor_authored_indoor_level.cpp) genres already have an editor-authored-and-
// playing level test; this is the RACER sibling. It shares those tests'
// author -> save -> reload -> Play -> assert -> strip spine, but swaps the
// rolling-hills surface / box room for a closed-loop RACE TRACK + a no-code
// drivable car, so the discriminating assertion is LAPPING: the car drives the
// authored Bezier loop -- it corners (its heading sweeps past 180 deg), it visits
// both extents of the oval, it progresses a lap's worth of distance and completes
// a gate lap, and it stays governed under the speed cap.
//
// Everything below is authored from the SCENE/EDITOR component model only:
//   * a scene::TrackComponent (a fixed set of cubic Bezier segments + track
//     half-width + the start/finish lap gate) on a "Track" entity, and
//   * a scene::VehicleComponent (the Wave-8 no-code drivable car, governed) on a
//     "Car" entity that ALSO carries the same TrackComponent so PlayRuntime runs
//     its track-follow auto-driver over it, plus
//   * a static ground plane (scene::RigidBodyComponent, mass 0) the wheels rest on.
// No engine internals, no bespoke racer C++: PlayRuntime ports the proven
// racer_demo auto_drive logic into a deterministic, alloc-free per-tick driver.
//
// The flow proves the DoD bar end to end:
//   author -> save_scene_file (in-memory .psyscene) -> parse_scene_file ->
//   instantiate_scene_file (FRESH registry/scene) -> PlayRuntime.begin -> tick*N
//   -> ASSERT the level PLAYS (the car laps the authored track) -> end (clean
//   strip) -> a 2nd Play re-synthesises.
//
// DoD assertions (all on the RELOADED scene, never the authored one):
//   (a) SYNTHESIS: the authored VehicleComponent synthesised into a LIVE physics
//       vehicle (runtime_vehicle + runtime_chassis filled, body_count includes the
//       chassis), and the TrackComponent round-tripped with its segments + gate.
//   (b) PROGRESS: the track-follow driver drove the car a lap's worth of distance
//       around the loop (cumulative planar distance >= a lap's arc fraction).
//   (c) CORNERS: the chassis heading swept through more than 180 deg AND the car
//       visited both extents of the oval (a wide span on the curve axis), proving
//       it drove the U-turns rather than running straight.
//   (d) LAPS: the authored lap gate registered at least one completed lap.
//   (e) GOVERNED: the peak speed stayed under the authored governor cap (+margin),
//       so the car cruised the loop instead of running away.
//   (f) ROUND-TRIPS + STRIPS: end() tears down the runtime vehicle + clears the
//       track runtime cursor/lap state, leaving the editor scene authoring-only;
//       a 2nd Play re-synthesises cleanly.
//
// Deterministic: fixed-step ticks, no RNG, no wall clock. Headless unit ticks are
// fast, so a full multi-lap run of a few thousand ticks is cheap.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "editor/play/PlayRuntime.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/PhysicsComponents.h"
#include "scene/SceneEcs.h"
#include "scene/SceneFile.h"
#include "scene/TrackComponent.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    auto& registry = scene.registry();
    const u32 count = registry.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> entities(count);
    const u32 copied = registry.snapshot_live_entities(entities);
    entities.resize(copied);
    for (const Entity e : entities) {
        if (scene.entity_name(e) == name)
            return e;
    }
    return {};
}

math::Vec3 v3(f32 x, f32 y, f32 z) noexcept { return {x, y, z}; }

// Cubic Bezier evaluation (host-side, only to derive the spawn pose + asserts).
math::Vec3 bezier_eval(const scene::TrackSegment& s, f32 t) noexcept {
    const f32 u = 1.0f - t;
    const f32 b0 = u * u * u;
    const f32 b1 = 3.0f * u * u * t;
    const f32 b2 = 3.0f * u * t * t;
    const f32 b3 = t * t * t;
    return v3(b0 * s.p0.x + b1 * s.p1.x + b2 * s.p2.x + b3 * s.p3.x,
              b0 * s.p0.y + b1 * s.p1.y + b2 * s.p2.y + b3 * s.p3.y,
              b0 * s.p0.z + b1 * s.p1.z + b2 * s.p2.z + b3 * s.p3.z);
}

math::Vec3 bezier_tangent(const scene::TrackSegment& s, f32 t) noexcept {
    const f32 u = 1.0f - t;
    const f32 a = 3.0f * u * u;
    const f32 b = 6.0f * u * t;
    const f32 c = 3.0f * t * t;
    return v3(a * (s.p1.x - s.p0.x) + b * (s.p2.x - s.p1.x) + c * (s.p3.x - s.p2.x),
              a * (s.p1.y - s.p0.y) + b * (s.p2.y - s.p1.y) + c * (s.p3.y - s.p2.y),
              a * (s.p1.z - s.p0.z) + b * (s.p2.z - s.p1.z) + c * (s.p3.z - s.p2.z));
}

constexpr f32 kTrackHalfWidth = 6.0f;
constexpr f32 kStraightHalfX = 50.0f;  // straight half-length on X
constexpr f32 kCurveHalfZ = 25.0f;     // curve half-depth on Z
constexpr f32 kCarMaxSpeed = 12.0f;    // governed forward-speed cap (m/s)

// Build a closed oval track centred at the origin (XZ plane, flat at y=0): two
// straights along +/-X joined by two half-loop U-turns. Mirrors racer_demo's
// build_oval_track, but emits scene::TrackSegment quads onto a TrackComponent.
// C1-continuous joins (co-linear endpoint tangents) so the loop has no kinks.
scene::TrackComponent build_oval_track() {
    const f32 sx = kStraightHalfX;
    const f32 sz = kCurveHalfZ;
    const f32 cz = sz * 1.8f;

    scene::TrackComponent t{};
    t.segment_count = 4u;
    auto set_seg = [&](u32 i, math::Vec3 p0, math::Vec3 p1, math::Vec3 p2, math::Vec3 p3) {
        t.segments[i].p0 = p0;
        t.segments[i].p1 = p1;
        t.segments[i].p2 = p2;
        t.segments[i].p3 = p3;
        t.segments[i].half_width = kTrackHalfWidth;
    };
    // Segment 0: straight, +X along z = -sz (the start straight).
    set_seg(0u, v3(-sx, 0.0f, -sz), v3(-sx * 0.33f, 0.0f, -sz), v3(sx * 0.33f, 0.0f, -sz),
            v3(sx, 0.0f, -sz));
    // Segment 1: U-turn at +X.
    set_seg(1u, v3(sx, 0.0f, -sz), v3(sx + cz, 0.0f, -sz), v3(sx + cz, 0.0f, sz),
            v3(sx, 0.0f, sz));
    // Segment 2: straight, -X along z = +sz.
    set_seg(2u, v3(sx, 0.0f, sz), v3(sx * 0.33f, 0.0f, sz), v3(-sx * 0.33f, 0.0f, sz),
            v3(-sx, 0.0f, sz));
    // Segment 3: U-turn at -X.
    set_seg(3u, v3(-sx, 0.0f, sz), v3(-sx - cz, 0.0f, sz), v3(-sx - cz, 0.0f, -sz),
            v3(-sx, 0.0f, -sz));

    // Auto-driver tuning (mirror racer_demo's Driver; just under the governor cap).
    t.target_speed = 11.0f;
    t.look_ahead = 12.0f;
    t.steer_gain = 0.7f;
    t.steer_clamp = 0.22f;
    t.throttle_kp = 0.5f;
    return t;
}

// Author the whole racer level into `authored`, returning the cooked .psyscene
// bytes. Uses ONLY the public scene authoring APIs.
void author_level(scene::Scene& authored,
                  scene::detail::AlignedVector<u8>& out_bytes,
                  scene::SceneFileSaveStats& stats) {
    scene::TrackComponent track = build_oval_track();

    // The lap gate is the plane through a start point on segment 0, normal = the
    // track forward tangent there. Start a little into the straight so the car
    // crosses the gate cleanly on its first lap.
    const f32 start_t = 0.05f;
    const math::Vec3 start_p = bezier_eval(track.segments[0], start_t);
    const math::Vec3 start_tan = math::normalize(bezier_tangent(track.segments[0], start_t));
    track.lap_gate_point = start_p;
    track.lap_gate_normal = start_tan;
    track = scene::sanitize_track_component(track);

    // --- STATIC GROUND PLANE: a mass-0 RigidBody the wheels rest on. ---------
    {
        const Entity ground = authored.create_entity(at({0.0f, 0.0f, 0.0f}));
        REQUIRE(authored.set_entity_name(ground, "Ground"));
        scene::RigidBodyComponent rb{};
        rb.shape = scene::ColliderShape::Plane;
        rb.mass = 0.0f;  // static
        rb.half_extent = v3(1.0f, 1.0f, 1.0f);
        authored.registry().add<scene::RigidBodyComponent>(ground, rb);
    }

    // --- TRACK entity: carries the authored TrackComponent (editor-authorable). --
    {
        const Entity track_entity = authored.create_entity(at({0.0f, 0.0f, 0.0f}));
        REQUIRE(authored.set_entity_name(track_entity, "Track"));
        authored.registry().add<scene::TrackComponent>(track_entity, track);
    }

    // --- CAR: a no-code governed VehicleComponent + the SAME TrackComponent so
    // PlayRuntime runs its track-follow driver over the car. Spawn ON the start
    // point, lifted to the chassis ride height, ALIGNED so the car's local -Z
    // forward (PlayRuntime's vehicle forward convention) points down the start
    // tangent -- so the driver starts the loop without a big initial U-turn. -----
    {
        // Yaw that maps local -Z to start_tan: rotating (0,0,-1) about +Y by yaw
        // gives (-sin yaw, 0, -cos yaw); solve yaw = atan2(-tan.x, -tan.z).
        const f32 yaw = std::atan2(-start_tan.x, -start_tan.z);
        scene::LocalTransform car_xf{};
        car_xf.translation = v3(start_p.x, start_p.y + 0.6f, start_p.z);
        car_xf.rotation = math::quat_from_axis_angle(v3(0.0f, 1.0f, 0.0f), yaw);
        const Entity car = authored.create_entity(car_xf);
        REQUIRE(authored.set_entity_name(car, "Car"));

        scene::VehicleComponent vc{};
        vc.half_extent = v3(2.1f, 0.55f, 0.9f);
        vc.mass = 1500.0f;
        vc.engine_max_torque = 240.0f;
        vc.drag = 0.55f;
        vc.wheel_radius = 0.35f;
        vc.suspension = 0.30f;
        vc.stiffness = 40000.0f;
        vc.damping = 4800.0f;
        // Speed governor + steering authority (mirror racer_demo's build_vehicle).
        vc.max_speed = kCarMaxSpeed;
        vc.steer_full_speed = 5.0f;
        vc.steer_taper_speed = 14.0f;
        vc.steer_min_authority = 0.55f;
        vc.ground_mode = scene::VehicleGroundMode::Plane;
        vc.plane_y = 0.0f;
        vc.is_player = 1u;
        authored.registry().add<scene::VehicleComponent>(
            car, scene::sanitize_vehicle_component(vc));
        authored.registry().add<scene::TrackComponent>(car, track);
    }

    std::string error;
    REQUIRE(scene::save_scene_file(authored, {}, out_bytes, &stats, &error));
    REQUIRE(error.empty());
}

}  // namespace

TEST_CASE(
    "DoD: an editor-authored RACER level round-trips through a .psyscene and PLAYS "
    "under PlayRuntime, with a no-code car lapping the authored track",
    "[play][editor][authoring][level][racer][dod]") {
    // --- 1. AUTHOR the level + SAVE it to an in-memory .psyscene -------------
    scene::detail::AlignedVector<u8> bytes;
    scene::SceneFileSaveStats stats{};
    {
        RegistryReset author_reset;
        auto& author_registry = scene::EcsRegistry::Get();
        author_registry.set_structural_deferred(false);
        scene::Scene authored{author_registry};
        author_level(authored, bytes, stats);
    }

    // The saved file carries TWO SPHY physics-body records (the static ground
    // RigidBody + the car's VehicleComponent both ride the SPHY chunk), ONE SVHX
    // vehicle-extension record (the car's governor/steer authority), and TWO
    // TrackComponent records (the Track entity + the Car entity both carry one).
    REQUIRE(stats.physics_bodies == 2u);  // static ground RigidBody + the car vehicle
    REQUIRE(stats.vehicle_exts == 1u);    // the car's governor/steer ext (SVHX)
    REQUIRE(stats.tracks == 2u);          // the Track entity + the Car entity

    // Optional: dump the authored level to a checked-in sample .psyscene asset,
    // gated behind an env var so a normal run never touches the filesystem.
    //   PSYNDER_DUMP_RACER_ASSET=<path> ./psynder_unit "[racer][dod]"
    if (const char* dump_path = std::getenv("PSYNDER_DUMP_RACER_ASSET");
        dump_path != nullptr && dump_path[0] != '\0') {
        std::ofstream out(dump_path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out.good());
    }

    // --- 2. PARSE + RELOAD into a FRESH registry / scene --------------------
    scene::SceneFileView view{};
    std::string error;
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()},
                                    view, &error));
    REQUIRE(error.empty());
    REQUIRE(view.header->version == scene::kPsySceneVersion);
    REQUIRE(view.physics_bodies.size() == 2u);  // static ground + the car vehicle
    REQUIRE(view.vehicle_exts.size() == 1u);
    REQUIRE(view.tracks.size() == 2u);

    RegistryReset reload_reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);
    scene::Scene scene{registry};
    std::array<scene::SceneMeshBinding, 0> mesh_bindings{};
    std::array<Entity, 0> out_mesh_entities{};
    (void)scene::instantiate_scene_file(scene, view, mesh_bindings, out_mesh_entities);

    const Entity ground = find_entity_named(scene, "Ground");
    const Entity track_entity = find_entity_named(scene, "Track");
    const Entity car = find_entity_named(scene, "Car");
    REQUIRE(ground.valid());
    REQUIRE(track_entity.valid());
    REQUIRE(car.valid());

    // The reloaded Track entity carries the authored TrackComponent with its
    // segments + gate (it came straight off the .psyscene; no C++ re-authored it).
    {
        const auto* tc = registry.get<scene::TrackComponent>(track_entity);
        REQUIRE(tc != nullptr);
        REQUIRE(tc->segment_count == 4u);
        REQUIRE(tc->segments[0].half_width == Approx(kTrackHalfWidth));
        // Runtime cursor/lap state is NOT serialized: it reloads at the defaults.
        REQUIRE(tc->lap_count == 0u);
        REQUIRE(tc->gate_armed == 0u);
    }
    // The reloaded Car carries BOTH the vehicle proxy and the track proxy, but no
    // live physics handles yet (Play has not begun).
    {
        const auto* vc = registry.get<scene::VehicleComponent>(car);
        REQUIRE(vc != nullptr);
        REQUIRE(vc->max_speed == Approx(kCarMaxSpeed));
        REQUIRE(vc->runtime_vehicle == 0u);
        REQUIRE(vc->runtime_chassis == 0u);
        REQUIRE(registry.get<scene::TrackComponent>(car) != nullptr);
    }

    // --- 3. PLAY: begin synthesises the live sim, tick runs it --------------
    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());

    // DoD assert (a): the authored VehicleComponent synthesised into a LIVE physics
    // vehicle -- runtime_vehicle + runtime_chassis filled. The static ground + the
    // car chassis are live bodies, so body_count == 2.
    {
        const auto* vc = registry.get<scene::VehicleComponent>(car);
        REQUIRE(vc != nullptr);
        REQUIRE(vc->runtime_vehicle != 0u);
        REQUIRE(vc->runtime_chassis != 0u);
    }
    REQUIRE(runtime.body_count() == 2u);  // ground plane + car chassis

    const f32 dt = 1.0f / 120.0f;

    // Drive the track-follow loop for enough ticks to lap the oval. Latch the
    // evidence across ALL ticks (cumulative distance, heading sweep, extents,
    // peak speed, lap count) so a single end-of-loop snapshot can never miss it.
    // The chassis heading is the car's local -Z forward (PlayRuntime's vehicle
    // forward convention), read from the live chassis body each tick.
    //
    // There is no public accessor to PlayRuntime's runtime world, so we sample the
    // car's TransformComponent, which PlayRuntime writes back from the simulated
    // chassis pose every tick (the renderer source) -- it tracks the world pose.
    auto car_world_pos = [&]() -> math::Vec3 {
        const auto* tc = registry.get<scene::TransformComponent>(car);
        return tc != nullptr ? tc->local.translation : math::Vec3{};
    };
    auto car_world_rot = [&]() -> math::Quat {
        const auto* tc = registry.get<scene::TransformComponent>(car);
        return tc != nullptr ? tc->local.rotation : math::Quat{0.0f, 0.0f, 0.0f, 1.0f};
    };

    math::Vec3 prev_pos = car_world_pos();
    math::Vec3 prev_heading =
        math::quat_rotate(math::quat_normalize(car_world_rot()), v3(0.0f, 0.0f, -1.0f));
    prev_heading.y = 0.0f;
    prev_heading = math::normalize(prev_heading);

    f32 cum_distance = 0.0f;       // planar distance the car covered
    f32 heading_sweep = 0.0f;      // total |turn| of the heading (rad)
    f32 peak_speed = 0.0f;         // peak per-tick speed (m/s)
    f32 x_min = prev_pos.x, x_max = prev_pos.x;
    f32 z_min = prev_pos.z, z_max = prev_pos.z;

    // The oval perimeter is ~2 straights (200 m) + 2 wide U-turns (~140 m each)
    // ~= 480 m; at the ~11 m/s cruise a lap is ~45 s, so 9000 ticks (75 s at
    // 120 Hz) comfortably completes a full lap (a gate crossing) plus the second
    // U-turn for the > 180 deg heading sweep. Headless ticks are fast.
    constexpr int kTicks = 9000;
    for (int step = 0; step < kTicks; ++step) {
        runtime.tick(scene, dt);

        const math::Vec3 pos = car_world_pos();
        math::Vec3 dp = math::sub(pos, prev_pos);
        dp.y = 0.0f;
        const f32 step_dist = math::length(dp);
        cum_distance += step_dist;
        peak_speed = std::max(peak_speed, step_dist / dt);
        x_min = std::min(x_min, pos.x);
        x_max = std::max(x_max, pos.x);
        z_min = std::min(z_min, pos.z);
        z_max = std::max(z_max, pos.z);
        prev_pos = pos;

        math::Vec3 heading =
            math::quat_rotate(math::quat_normalize(car_world_rot()), v3(0.0f, 0.0f, -1.0f));
        heading.y = 0.0f;
        if (math::dot(heading, heading) > 1e-6f) {
            heading = math::normalize(heading);
            const f32 dotv = std::clamp(math::dot(prev_heading, heading), -1.0f, 1.0f);
            heading_sweep += std::acos(dotv);
            prev_heading = heading;
        }
    }

    // The reloaded car's live TrackComponent (PlayRuntime drives the Car entity's
    // proxy; the standalone Track entity has no vehicle so it is never driven).
    const auto* car_track = registry.get<scene::TrackComponent>(car);
    REQUIRE(car_track != nullptr);

    INFO("racer DoD: distance=" << cum_distance << "m heading_sweep=" << heading_sweep
                                << "rad x_span=" << (x_max - x_min)
                                << " z_span=" << (z_max - z_min)
                                << " peak_speed=" << peak_speed
                                << " laps=" << car_track->lap_count);

    // DoD assert (b) -- PROGRESS: the driver covered a lap's worth of distance.
    // The oval's perimeter is ~2 straights (4*sx) + 2 U-turns (each a wide arc);
    // a conservative lower bound on a single lap is the two straights alone.
    constexpr f32 kMinProgress = 4.0f * kStraightHalfX;  // >= the two straights
    REQUIRE(cum_distance >= kMinProgress);

    // DoD assert (c) -- CORNERS: the heading swept past 180 deg (the car turned
    // through both U-turns) AND it visited both extents of the oval. A car that
    // could only go straight would pin to the start straight: no heading sweep,
    // and a tiny Z span.
    REQUIRE(heading_sweep > math::kPi);                 // > 180 deg of cornering
    REQUIRE((x_max - x_min) >= 1.5f * kStraightHalfX);  // visits both straights' ends
    REQUIRE((z_max - z_min) >= 1.5f * kCurveHalfZ);     // visits both straights (z = +/-sz)

    // DoD assert (d) -- LAPS: the authored gate registered at least one full lap.
    REQUIRE(car_track->lap_count >= 1u);

    // DoD assert (e) -- GOVERNED: the peak speed stayed under the authored cap
    // (+ a small margin). A runaway car blows well past the governed cruise.
    constexpr f32 kCapMargin = 2.5f;
    REQUIRE(peak_speed <= kCarMaxSpeed + kCapMargin);

    // --- 4. STOP: end() cleanly strips ALL runtime state -------------------
    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());

    // DoD assert (f): the live vehicle handles were cleared and the track runtime
    // cursor/lap state reset, leaving the editor scene authoring-only.
    {
        const auto* vc = registry.get<scene::VehicleComponent>(car);
        REQUIRE(vc != nullptr);
        REQUIRE(vc->runtime_vehicle == 0u);
        REQUIRE(vc->runtime_chassis == 0u);
        const auto* tc = registry.get<scene::TrackComponent>(car);
        REQUIRE(tc != nullptr);
        REQUIRE(tc->lap_count == 0u);
        REQUIRE(tc->gate_armed == 0u);
        REQUIRE(tc->cursor_seg == 0u);
        // The authored geometry survives the round trip untouched.
        REQUIRE(tc->segment_count == 4u);
        REQUIRE(tc->segments[0].half_width == Approx(kTrackHalfWidth));
    }

    // --- 5. RE-PLAY: a second Play session re-synthesises cleanly -----------
    runtime.begin(scene);
    REQUIRE(runtime.playing());
    REQUIRE(runtime.body_count() == 2u);
    {
        const auto* vc = registry.get<scene::VehicleComponent>(car);
        REQUIRE(vc != nullptr);
        REQUIRE(vc->runtime_vehicle != 0u);
        REQUIRE(vc->runtime_chassis != 0u);
    }
    // A few ticks of the second session move the car again (the driver re-seeded).
    const math::Vec3 replay_start = car_world_pos();
    for (int step = 0; step < 240; ++step)
        runtime.tick(scene, dt);
    const math::Vec3 replay_end = car_world_pos();
    {
        math::Vec3 d = math::sub(replay_end, replay_start);
        d.y = 0.0f;
        INFO("re-play moved " << math::length(d) << "m");
        REQUIRE(math::length(d) > 1.0f);  // the car drove again
    }
    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());
}
