// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE physics runtime implementation.
//
// See PlayRuntime.h for the lifecycle contract and the DOTS rationale. All
// per-entity sim state lives in pooled ECS components; this TU only owns two
// pooled scratch vectors (entity scan lists) + the authored-transform
// snapshot, all reserved once and reused.
//
// Writeback parallelism: World::get_position / get_rotation are pure const
// reads (engine/physics/World.cpp:336/351) -- they read the function-local
// static world_state() and interpolate prev/current; they NEVER mutate a body
// or lazily allocate. step() (the only writer) completes BEFORE the writeback
// query runs, and the query writes only its own TransformComponent column
// (per-row safe). So the writeback runs in parallel safely. The character
// writeback is a short serial pass (few characters; physics::character::move
// mutates the shared character store and must not run concurrently).

#include "editor/play/PlayRuntime.h"

#include "editor/play/CharacterPeek.h"  // peek_character_position (isolated TU)
#include "math/MathExt.h"               // inverse_affine (parenting writeback)

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace psynder::editor::play {

namespace {

// Extract a (translation, rotation) world pose from a rigid 4x4 world matrix.
// Reused for the parenting-aware writeback: a simulated body's WORLD pose must
// be re-expressed in its parent's local space before it can be stored into
// TransformComponent.local. Decomposition mirrors the scene save path
// (SceneFile.cpp decompose_world_transform / quat_from_basis): normalize the
// three basis columns, fix handedness, and build a quaternion from the basis.
// Authored (non-uniform) scale on the body is never produced by physics, so
// only translation + rotation are written; the caller preserves lt.scale.
struct WorldPose {
    math::Vec3 translation{};
    math::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
};

[[nodiscard]] math::Vec3 matrix_column(const math::Mat4& m, u32 column) noexcept {
    const u32 base = column * 4u;
    return {m.m[base + 0u], m.m[base + 1u], m.m[base + 2u]};
}

[[nodiscard]] math::Quat quat_from_basis(math::Vec3 x, math::Vec3 y, math::Vec3 z) noexcept {
    const f32 m00 = x.x, m01 = y.x, m02 = z.x;
    const f32 m10 = x.y, m11 = y.y, m12 = z.y;
    const f32 m20 = x.z, m21 = y.z, m22 = z.z;
    const f32 trace = m00 + m11 + m22;

    math::Quat q{};
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return math::quat_normalize(q);
}

// Decompose a node's WORLD matrix into a rigid (translation, rotation) pose,
// dropping any scale. Used to SPAWN the physics body at the entity's true world
// pose (scene.transform() returns the parent-LOCAL transform, which is the
// world pose only for top-level entities). Mirrors the basis normalization in
// world_pose_in_parent below.
[[nodiscard]] WorldPose world_pose_from_matrix(const math::Mat4& world) noexcept {
    WorldPose out{};
    out.translation = {world.m[12], world.m[13], world.m[14]};

    constexpr f32 kMinScale = 1.0e-6f;
    math::Vec3 cx = matrix_column(world, 0u);
    math::Vec3 cy = matrix_column(world, 1u);
    math::Vec3 cz = matrix_column(world, 2u);
    const f32 sx = math::length(cx);
    const f32 sy = math::length(cy);
    const f32 sz = math::length(cz);
    if (sx <= kMinScale || sy <= kMinScale || sz <= kMinScale)
        return out;  // degenerate basis: identity rotation
    cx = math::mul(cx, 1.0f / sx);
    cy = math::mul(cy, 1.0f / sy);
    cz = math::mul(cz, 1.0f / sz);
    if (math::dot(math::cross(cx, cy), cz) < 0.0f)
        cz = math::mul(cz, -1.0f);
    out.rotation = quat_from_basis(cx, cy, cz);
    return out;
}

// Convert a simulated body's WORLD pose (world_pos, world_rot) into the local
// pose under `parent_world`: local = inverse(parent_world) * body_world. Pure,
// alloc-free. The parent world matrix may carry scale; we strip it from the
// rotation basis (normalized columns) so the result is a clean rigid pose.
[[nodiscard]] WorldPose world_pose_in_parent(const math::Mat4& parent_world,
                                             math::Vec3 world_pos,
                                             math::Quat world_rot) noexcept {
    const math::Mat4 inv_parent = math::inverse_affine(parent_world);
    const math::Mat4 body_world = math::mul_affine(math::translate(world_pos),
                                                   math::rotate_quat(world_rot));
    const math::Mat4 local = math::mul_affine(inv_parent, body_world);

    WorldPose out{};
    out.translation = {local.m[12], local.m[13], local.m[14]};

    constexpr f32 kMinScale = 1.0e-6f;
    math::Vec3 cx = matrix_column(local, 0u);
    math::Vec3 cy = matrix_column(local, 1u);
    math::Vec3 cz = matrix_column(local, 2u);
    const f32 sx = math::length(cx);
    const f32 sy = math::length(cy);
    const f32 sz = math::length(cz);
    if (sx <= kMinScale || sy <= kMinScale || sz <= kMinScale) {
        // Degenerate parent basis: keep the body's own world rotation rather
        // than emitting NaNs.
        out.rotation = math::quat_normalize(world_rot);
        return out;
    }
    cx = math::mul(cx, 1.0f / sx);
    cy = math::mul(cy, 1.0f / sy);
    cz = math::mul(cz, 1.0f / sz);
    if (math::dot(math::cross(cx, cy), cz) < 0.0f)
        cz = math::mul(cz, -1.0f);
    out.rotation = quat_from_basis(cx, cy, cz);
    return out;
}

}  // namespace

PlayRuntime::~PlayRuntime() {
    // The handles needed to free bodies live in the ECS components, which we
    // cannot reach without a Scene&. The host contract is to always call end()
    // on Stop / teardown, which frees the bodies and restores transforms; the
    // destructor only marks state. (The shared World is a process singleton and
    // is reclaimed at process exit regardless.)
    playing_ = false;
}

void PlayRuntime::clear_pools() noexcept {
    rigid_entities_.clear();
    character_entities_.clear();
    vehicle_entities_.clear();
    helicopter_entities_.clear();
    authored_.clear();
    body_count_ = 0u;
}

void PlayRuntime::begin(scene::Scene& scene) {
    // Idempotent: a stray double-begin tears down the previous session first.
    if (playing_)
        end(scene);

    clear_pools();

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = physics::World::Get();
    world.set_gravity(config_.gravity);

    // --- Phase 1: scan (parallel, mutex-merged) ---------------------------
    // Gather entities that HAVE a RigidBodyComponent. We read the entity column
    // (via SceneNodeComponent) row-aligned with the rigid body column. The
    // query body fires once per chunk across worker threads, so the shared
    // append is serialized by a mutex and each chunk builds a local buffer
    // first (the gather_scene_render_items pattern).
    // Each chunk's `nodes` span is exactly its live-row count, so we drive the
    // append straight off nodes.size() -- there is NO fixed per-chunk cap that
    // could silently drop rows beyond N (the previous std::array<,256> + break
    // would have dropped any row past 256 in a chunk). Each chunk's entities are
    // appended under the mutex; reserve-then-push keeps it amortized alloc-free.
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, RigidBodyComponent>, scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const RigidBodyComponent> bodies) {
                const usize n = std::min(nodes.size(), bodies.size());
                if (n == 0u)
                    return;
                std::scoped_lock lock{append_mutex};
                rigid_entities_.reserve(rigid_entities_.size() + n);
                for (usize i = 0; i < n; ++i)
                    rigid_entities_.push_back(nodes[i].entity);
            });
    }

    // Same scan for character entities.
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, CharacterControllerComponent>,
                  scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const CharacterControllerComponent> chars) {
                const usize n = std::min(nodes.size(), chars.size());
                if (n == 0u)
                    return;
                std::scoped_lock lock{append_mutex};
                character_entities_.reserve(character_entities_.size() + n);
                for (usize i = 0; i < n; ++i)
                    character_entities_.push_back(nodes[i].entity);
            });
    }

    // Same scan for vehicle entities.
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, VehicleComponent>, scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const VehicleComponent> vehicles) {
                const usize n = std::min(nodes.size(), vehicles.size());
                if (n == 0u)
                    return;
                std::scoped_lock lock{append_mutex};
                vehicle_entities_.reserve(vehicle_entities_.size() + n);
                for (usize i = 0; i < n; ++i)
                    vehicle_entities_.push_back(nodes[i].entity);
            });
    }

    // Same scan for helicopter entities.
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, HelicopterComponent>, scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const HelicopterComponent> helis) {
                const usize n = std::min(nodes.size(), helis.size());
                if (n == 0u)
                    return;
                std::scoped_lock lock{append_mutex};
                helicopter_entities_.reserve(helicopter_entities_.size() + n);
                for (usize i = 0; i < n; ++i)
                    helicopter_entities_.push_back(nodes[i].entity);
            });
    }

    authored_.reserve(rigid_entities_.size() + character_entities_.size() +
                      vehicle_entities_.size() + helicopter_entities_.size());

    // --- Phase 2: build bodies (serial, main thread) ----------------------
    // create_body mutates the shared World, so this MUST be serial. We snapshot
    // the entity's authored (parent-LOCAL) transform for restore, create the
    // body at the entity's WORLD pose, and write the handle back. Physics
    // operates entirely in world space, so a parented body must SPAWN at its
    // world pose (scene.transform() returns the parent-LOCAL transform, which is
    // the world pose only for top-level entities). Refresh world matrices once
    // so a parented entity's world pose is current.
    scene::SceneGraph& graph = scene.graph();
    graph.update_world_transforms();

    // World spawn pose for an entity: its decomposed world matrix when parented,
    // else its authored local (local == world for a top-level entity). Alloc-free.
    const auto world_spawn = [&](const scene::LocalTransform& local,
                                 Entity e) noexcept -> WorldPose {
        const scene::SceneNode node = scene.node(e);
        if (node.valid() && graph.parent(node).valid())
            return world_pose_from_matrix(graph.world_matrix(node));
        return WorldPose{local.translation, math::quat_normalize(local.rotation)};
    };

    for (const Entity e : rigid_entities_) {
        RigidBodyComponent* rb = reg.get<RigidBodyComponent>(e);
        if (rb == nullptr)
            continue;
        const scene::LocalTransform local = scene.transform(e);
        authored_.push_back(AuthoredTransform{e, local});
        const WorldPose spawn = world_spawn(local, e);

        physics::BodyDesc d{};
        d.shape = rb->shape;
        d.mass = rb->mass;  // 0 => static
        d.position = spawn.translation;
        d.rotation = spawn.rotation;
        d.half_extent = rb->half_extent;
        d.friction = rb->friction;
        d.restitution = rb->restitution;

        rb->body = world.create_body(d);
        if (rb->body.valid())
            ++body_count_;
    }

    // Characters.
    for (const Entity e : character_entities_) {
        CharacterControllerComponent* cc = reg.get<CharacterControllerComponent>(e);
        if (cc == nullptr)
            continue;
        const scene::LocalTransform local = scene.transform(e);
        authored_.push_back(AuthoredTransform{e, local});

        physics::character::CharacterDesc d{};
        d.position = world_spawn(local, e).translation;
        d.height = cc->height;
        d.radius = cc->radius;
        cc->character = physics::character::create(d);
        cc->walk_dir = math::Vec3{0.0f, 0.0f, 0.0f};
    }

    // Vehicles (serial; create_body + vehicle::create mutate the shared World).
    // The chassis is a dynamic box body at the authored pose; the four wheels
    // are auto-placed at the corners of half_extent. Front pair (-Z, the car's
    // forward is -Z to match the camera convention) = steer/non-drive, rear pair
    // (+Z) = drive (RWD: the solver treats wheels[2],[3] as the drive axle).
    for (const Entity e : vehicle_entities_) {
        VehicleComponent* vc = reg.get<VehicleComponent>(e);
        if (vc == nullptr)
            continue;
        const scene::LocalTransform local = scene.transform(e);
        authored_.push_back(AuthoredTransform{e, local});
        const WorldPose spawn = world_spawn(local, e);

        physics::BodyDesc cd{};
        cd.shape = physics::Shape::Box;
        cd.mass = vc->mass;
        cd.position = spawn.translation;
        cd.rotation = spawn.rotation;
        cd.half_extent = vc->half_extent;
        cd.friction = 0.5f;
        cd.restitution = 0.0f;
        vc->chassis = world.create_body(cd);
        if (!vc->chassis.valid())
            continue;
        ++body_count_;

        // Four wheels at the half-extent corners, dropped to the chassis bottom.
        const f32 hx = vc->half_extent.x;
        const f32 hy = vc->half_extent.y;
        const f32 hz = vc->half_extent.z;
        std::array<physics::vehicle::WheelDesc, 4> wheels{};
        // Order: [0]=front-left, [1]=front-right (steer); [2]=rear-left,
        // [3]=rear-right (drive). Forward is -Z.
        const std::array<math::Vec3, 4> corners{
            math::Vec3{-hx, -hy, -hz}, math::Vec3{hx, -hy, -hz},
            math::Vec3{-hx, -hy, hz},  math::Vec3{hx, -hy, hz}};
        for (usize w = 0; w < 4; ++w) {
            wheels[w].local_position = corners[w];
            wheels[w].radius = vc->wheel_radius;
            wheels[w].suspension = vc->suspension;
            wheels[w].stiffness = vc->stiffness;
            wheels[w].damping = vc->damping;
        }

        physics::vehicle::VehicleDesc vd{};
        vd.chassis = vc->chassis;
        vd.wheels = std::span<const physics::vehicle::WheelDesc>{wheels.data(), wheels.size()};
        vd.engine_max_torque = vc->engine_max_torque;
        vd.drag_coefficient = vc->drag;
        vd.downforce_coefficient = 0.0f;
        vc->vehicle = physics::vehicle::create(vd);
        // Flat ground plane at y=0 (no terrain yet; KNOWN follow-up).
        physics::vehicle::set_ground_plane(vc->vehicle, 0.0f);
    }

    // Helicopters (serial; create_body mutates the shared World). The chassis is
    // a dynamic box body at the authored pose. The angular-velocity estimate is
    // zeroed so the craft starts at rest.
    for (const Entity e : helicopter_entities_) {
        HelicopterComponent* hc = reg.get<HelicopterComponent>(e);
        if (hc == nullptr)
            continue;
        const scene::LocalTransform local = scene.transform(e);
        authored_.push_back(AuthoredTransform{e, local});
        const WorldPose spawn = world_spawn(local, e);

        physics::BodyDesc hd{};
        hd.shape = physics::Shape::Box;
        hd.mass = hc->mass;
        hd.position = spawn.translation;
        hd.rotation = spawn.rotation;
        hd.half_extent = hc->half_extent;
        hd.friction = 0.5f;
        hd.restitution = 0.0f;
        hc->body = world.create_body(hd);
        hc->ang_vel_est = math::Vec3{0.0f, 0.0f, 0.0f};
        if (hc->body.valid())
            ++body_count_;
    }

    vehicle_throttle_ = 0.0f;
    vehicle_brake_ = 0.0f;
    vehicle_steer_ = 0.0f;
    heli_collective_ = 0.0f;
    heli_pitch_ = 0.0f;
    heli_roll_ = 0.0f;
    heli_yaw_ = 0.0f;

    playing_ = true;
}

void PlayRuntime::tick(scene::Scene& scene, f32 dt) {
    if (!playing_ || dt <= 0.0f)
        return;

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = physics::World::Get();

    // --- Drive characters first (serial) ----------------------------------
    // physics::character::move mutates the shared character store, so this is a
    // serial pass. Few characters, so the cost is negligible.
    for (const Entity e : character_entities_) {
        CharacterControllerComponent* cc = reg.get<CharacterControllerComponent>(e);
        if (cc == nullptr || !cc->character.valid())
            continue;
        const math::Vec3 walk = cc->walk_dir;
        const f32 wlen2 = math::dot(walk, walk);
        if (wlen2 > 1e-8f) {
            const f32 wlen = std::sqrt(wlen2);
            const math::Vec3 dir = math::mul(walk, 1.0f / wlen);
            physics::character::move(cc->character, math::mul(dir, cc->move_speed * dt), dt);
        }
    }

    // --- Apply vehicle driving intent (serial) ----------------------------
    // The shared player input drives every is_player vehicle. set_* only stash
    // controller values on the engine vehicle; the solver consumes them inside
    // world.step() below, so no per-vehicle stepping happens here.
    for (const Entity e : vehicle_entities_) {
        VehicleComponent* vc = reg.get<VehicleComponent>(e);
        if (vc == nullptr || !vc->vehicle.valid() || !vc->is_player)
            continue;
        physics::vehicle::set_throttle(vc->vehicle, vehicle_throttle_);
        physics::vehicle::set_brake(vc->vehicle, vehicle_brake_);
        physics::vehicle::set_steer(vc->vehicle, vehicle_steer_);
    }

    // --- Apply helicopter flight intent (serial) --------------------------
    // Body-relative arcade flight model, applied BEFORE the single step:
    //   * Collective -> thrust magnitude in [0, max_thrust] along the body-up
    //     axis (hover_assist centres neutral collective at m*g so it hovers).
    //     Continuous thrust is modelled as a per-tick linear impulse J=force*dt
    //     (there is no public linear apply_force).
    //   * Cyclic pitch/roll + pedal yaw -> body-axis torques, integrated into a
    //     component-carried angular-velocity estimate (the engine has no public
    //     angular reader). The estimate is exponentially damped each tick and
    //     written authoritatively via set_angular_velocity, so the craft stays
    //     controllable and never tumbles out of control. We are the only torque
    //     source while airborne (gravity adds no torque at the COM), so the
    //     estimate tracks the body. An inertia proxy I = m * (hx^2+hz^2)/3 maps
    //     torque -> angular acceleration (solid-box about a horizontal axis).
    for (const Entity e : helicopter_entities_) {
        HelicopterComponent* hc = reg.get<HelicopterComponent>(e);
        if (hc == nullptr || !hc->body.valid() || !hc->is_player)
            continue;

        const math::Quat rot = math::quat_normalize(world.get_rotation(hc->body));
        const math::Vec3 up = math::quat_rotate(rot, math::Vec3{0.0f, 1.0f, 0.0f});
        const math::Vec3 right = math::quat_rotate(rot, math::Vec3{1.0f, 0.0f, 0.0f});
        const math::Vec3 fwd = math::quat_rotate(rot, math::Vec3{0.0f, 0.0f, -1.0f});

        // Collective -> thrust along body-up.
        const f32 hover = hc->hover_assist ? hc->mass * (-config_.gravity.y) : 0.0f;
        f32 thrust = hover + heli_collective_ * hc->max_thrust_n;
        if (thrust < 0.0f)
            thrust = 0.0f;
        if (thrust > hc->max_thrust_n)
            thrust = hc->max_thrust_n;
        world.apply_impulse(hc->body, math::mul(up, thrust * dt));

        // Cyclic + pedals -> torque about body axes. Pitch nose-down (positive
        // input) tilts the nose forward (negative torque about body-right);
        // roll-right (positive) rolls right; yaw-left/right about body-up.
        const math::Vec3 torque =
            math::add(math::add(math::mul(right, -hc->pitch_torque * heli_pitch_),
                                math::mul(fwd, hc->roll_torque * heli_roll_)),
                      math::mul(up, hc->yaw_torque * heli_yaw_));

        // Inertia proxy (solid box about a horizontal axis) maps torque to an
        // angular acceleration. A single scalar keeps the estimate stable.
        const f32 hx = hc->half_extent.x;
        const f32 hz = hc->half_extent.z;
        const f32 inertia = std::max(1e-3f, hc->mass * (hx * hx + hz * hz) / 3.0f);
        const f32 inv_inertia = 1.0f / inertia;

        math::Vec3 w = hc->ang_vel_est;
        w = math::add(w, math::mul(torque, inv_inertia * dt));  // integrate torque
        // Exponential angular damping so it doesn't spin forever.
        const f32 decay = std::max(0.0f, 1.0f - hc->angular_damping * dt);
        w = math::mul(w, decay);
        hc->ang_vel_est = w;
        world.set_angular_velocity(hc->body, w);
    }

    // --- Step the world ONCE (serial, single writer) ----------------------
    // The single step advances rigid bodies, characters' resolution, AND the
    // vehicle solver (which writes each chassis body's pose). Both the rigid
    // body and vehicle writebacks below read from this one step -- the world is
    // never stepped twice per tick.
    world.step(dt);

    // --- Writeback rigid bodies (parallel) --------------------------------
    // get_position / get_rotation are pure const reads (no mutation, no lazy
    // alloc) and step() has completed, so reading the World concurrently is
    // safe. Each row writes only its own TransformComponent column.
    // SceneNodeComponent is read row-aligned (read-only, parallel-safe) so each
    // row can find its parent: a parented body's WORLD pose must be folded into
    // the parent's local space (local = inverse(parent_world) * body_world)
    // before storing into TransformComponent.local. graph().parent() and
    // graph().world_matrix() are pure const reads. The parent's world matrix is
    // from the previous tick's update_world_transforms() (this tick's runs
    // after every writeback) -- exact for a static parent, one-frame-lagged for
    // a moving one, which is the best achievable in a single graph pass.
    const scene::SceneGraph& graph = scene.graph();
    reg.query<scene::reads<scene::SceneNodeComponent, RigidBodyComponent>,
              scene::writes<scene::TransformComponent>>(
        [&](std::span<const scene::SceneNodeComponent> nodes,
            std::span<const RigidBodyComponent> bodies,
            std::span<scene::TransformComponent> transforms) {
            const usize n =
                std::min(std::min(nodes.size(), bodies.size()), transforms.size());
            for (usize i = 0; i < n; ++i) {
                const physics::BodyId id = bodies[i].body;
                if (!id.valid())
                    continue;
                const math::Vec3 wp = world.get_position(id);
                const math::Quat wr = math::quat_normalize(world.get_rotation(id));
                scene::LocalTransform& lt = transforms[i].local;
                const scene::SceneNode parent = graph.parent(nodes[i].node);
                if (parent.valid()) {
                    const WorldPose lp =
                        world_pose_in_parent(graph.world_matrix(parent), wp, wr);
                    lt.translation = lp.translation;
                    lt.rotation = lp.rotation;
                } else {
                    lt.translation = wp;
                    lt.rotation = wr;
                }
                // physics never changes scale; lt.scale is left as authored.
            }
        });

    // --- Writeback vehicle chassis poses (parallel) -----------------------
    // Same safety as the rigid body writeback: get_position/get_rotation are
    // pure const reads and step() has completed. Each row writes only its own
    // TransformComponent column.
    reg.query<scene::reads<scene::SceneNodeComponent, VehicleComponent>,
              scene::writes<scene::TransformComponent>>(
        [&](std::span<const scene::SceneNodeComponent> nodes,
            std::span<const VehicleComponent> vehicles,
            std::span<scene::TransformComponent> transforms) {
            const usize n =
                std::min(std::min(nodes.size(), vehicles.size()), transforms.size());
            for (usize i = 0; i < n; ++i) {
                const physics::BodyId id = vehicles[i].chassis;
                if (!id.valid())
                    continue;
                const math::Vec3 wp = world.get_position(id);
                const math::Quat wr = math::quat_normalize(world.get_rotation(id));
                scene::LocalTransform& lt = transforms[i].local;
                const scene::SceneNode parent = graph.parent(nodes[i].node);
                if (parent.valid()) {
                    const WorldPose lp =
                        world_pose_in_parent(graph.world_matrix(parent), wp, wr);
                    lt.translation = lp.translation;
                    lt.rotation = lp.rotation;
                } else {
                    lt.translation = wp;
                    lt.rotation = wr;
                }
            }
        });

    // --- Writeback helicopter chassis poses (parallel) --------------------
    // Same safety as the rigid body / vehicle writebacks: get_position /
    // get_rotation are pure const reads and step() has completed. Each row
    // writes only its own TransformComponent column.
    reg.query<scene::reads<scene::SceneNodeComponent, HelicopterComponent>,
              scene::writes<scene::TransformComponent>>(
        [&](std::span<const scene::SceneNodeComponent> nodes,
            std::span<const HelicopterComponent> helis,
            std::span<scene::TransformComponent> transforms) {
            const usize n =
                std::min(std::min(nodes.size(), helis.size()), transforms.size());
            for (usize i = 0; i < n; ++i) {
                const physics::BodyId id = helis[i].body;
                if (!id.valid())
                    continue;
                const math::Vec3 wp = world.get_position(id);
                const math::Quat wr = math::quat_normalize(world.get_rotation(id));
                scene::LocalTransform& lt = transforms[i].local;
                const scene::SceneNode parent = graph.parent(nodes[i].node);
                if (parent.valid()) {
                    const WorldPose lp =
                        world_pose_in_parent(graph.world_matrix(parent), wp, wr);
                    lt.translation = lp.translation;
                    lt.rotation = lp.rotation;
                } else {
                    lt.translation = wp;
                    lt.rotation = wr;
                }
            }
        });

    // --- Writeback characters (serial) ------------------------------------
    // The character has no body rotation; only its capsule-centre WORLD
    // position is resolved. Parented characters fold that world point into the
    // parent's local space (rotation kept as authored).
    for (const Entity e : character_entities_) {
        CharacterControllerComponent* cc = reg.get<CharacterControllerComponent>(e);
        if (cc == nullptr || !cc->character.valid())
            continue;
        auto* tc = reg.get<scene::TransformComponent>(e);
        if (tc == nullptr)
            continue;
        const math::Vec3 wp = peek_character_position(cc->character);
        const scene::SceneNode parent = graph.parent(scene.node(e));
        if (parent.valid()) {
            const WorldPose lp =
                world_pose_in_parent(graph.world_matrix(parent), wp, tc->local.rotation);
            tc->local.translation = lp.translation;
        } else {
            tc->local.translation = wp;
        }
    }

    // --- Chase camera (serial) --------------------------------------------
    // Position the active scene camera behind/above the player chassis. Done
    // before update_world_transforms so the camera's world matrix refreshes in
    // the single pass below.
    update_chase_camera(scene);

    // --- Recompute world matrices ONCE ------------------------------------
    scene.graph().update_world_transforms();
}

void PlayRuntime::end(scene::Scene& scene) {
    if (!playing_)
        return;

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = physics::World::Get();

    // Destroy bodies and clear the runtime handle column.
    for (const Entity e : rigid_entities_) {
        RigidBodyComponent* rb = reg.get<RigidBodyComponent>(e);
        if (rb == nullptr)
            continue;
        if (rb->body.valid())
            world.destroy_body(rb->body);
        rb->body = physics::BodyId{};
    }

    // Destroy characters and clear their handle column.
    for (const Entity e : character_entities_) {
        CharacterControllerComponent* cc = reg.get<CharacterControllerComponent>(e);
        if (cc == nullptr)
            continue;
        if (cc->character.valid())
            physics::character::destroy(cc->character);
        cc->character = physics::character::CharacterId{};
        cc->walk_dir = math::Vec3{0.0f, 0.0f, 0.0f};
    }

    // Destroy vehicles + their chassis bodies and clear the handle columns.
    for (const Entity e : vehicle_entities_) {
        VehicleComponent* vc = reg.get<VehicleComponent>(e);
        if (vc == nullptr)
            continue;
        if (vc->vehicle.valid())
            physics::vehicle::destroy(vc->vehicle);
        if (vc->chassis.valid())
            world.destroy_body(vc->chassis);
        vc->vehicle = physics::vehicle::VehicleId{};
        vc->chassis = physics::BodyId{};
    }

    // Destroy helicopter chassis bodies and clear the handle column.
    for (const Entity e : helicopter_entities_) {
        HelicopterComponent* hc = reg.get<HelicopterComponent>(e);
        if (hc == nullptr)
            continue;
        if (hc->body.valid())
            world.destroy_body(hc->body);
        hc->body = physics::BodyId{};
        hc->ang_vel_est = math::Vec3{0.0f, 0.0f, 0.0f};
    }

    vehicle_throttle_ = 0.0f;
    vehicle_brake_ = 0.0f;
    vehicle_steer_ = 0.0f;
    heli_collective_ = 0.0f;
    heli_pitch_ = 0.0f;
    heli_roll_ = 0.0f;
    heli_yaw_ = 0.0f;

    // Restore authored transforms (serial).
    for (const AuthoredTransform& a : authored_)
        scene.set_transform(a.entity, a.local);

    scene.graph().update_world_transforms();

    playing_ = false;
    clear_pools();  // keeps vector capacity
}

void PlayRuntime::update_chase_camera(scene::Scene& scene) noexcept {
    if (!playing_)
        return;

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = physics::World::Get();

    // Find the first live player vehicle's chassis (serial; few vehicles). If
    // none, fall back to the first live player helicopter's chassis so the
    // chase camera follows the craft the flight input drives.
    physics::BodyId chassis{};
    for (const Entity e : vehicle_entities_) {
        const VehicleComponent* vc = reg.get<VehicleComponent>(e);
        if (vc != nullptr && vc->is_player && vc->chassis.valid()) {
            chassis = vc->chassis;
            break;
        }
    }
    if (!chassis.valid()) {
        for (const Entity e : helicopter_entities_) {
            const HelicopterComponent* hc = reg.get<HelicopterComponent>(e);
            if (hc != nullptr && hc->is_player && hc->body.valid()) {
                chassis = hc->body;
                break;
            }
        }
    }
    if (!chassis.valid())
        return;

    const Entity camera = scene.active_camera_entity();
    if (!camera.valid() || !reg.alive(camera) || reg.get<scene::CameraComponent>(camera) == nullptr)
        return;
    // Chase only a TOP-LEVEL camera: the writeback puts a WORLD pose into
    // TransformComponent.local, which is only exact for an unparented entity.
    const scene::SceneNode cam_node = scene.node(camera);
    if (cam_node.valid() && scene.graph().parent(cam_node).valid())
        return;

    const math::Vec3 target = world.get_position(chassis);
    const math::Quat chassis_rot = math::quat_normalize(world.get_rotation(chassis));
    // Chassis forward is -Z (matches the editor/FPS camera convention). The
    // camera sits BEHIND (-forward) and ABOVE the chassis, looking at it.
    const math::Vec3 forward =
        math::normalize(math::quat_rotate(chassis_rot, math::Vec3{0.0f, 0.0f, -1.0f}));
    constexpr f32 kBack = 6.0f;
    constexpr f32 kUp = 2.5f;
    const math::Vec3 eye{target.x - forward.x * kBack,
                         target.y - forward.y * kBack + kUp,
                         target.z - forward.z * kBack};

    // Build a look-at rotation in the editor camera convention:
    // quat_from_euler(pitch, yaw, 0) with view direction = rotate * (0,0,-1).
    const math::Vec3 dir = math::normalize(math::Vec3{target.x - eye.x,
                                                      target.y - eye.y,
                                                      target.z - eye.z});
    const f32 yaw = std::atan2(dir.x, -dir.z);
    const f32 horiz = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    const f32 pitch = std::atan2(dir.y, horiz);
    const math::Quat rot = math::quat_normalize(math::quat_from_euler(pitch, yaw, 0.0f));

    scene::LocalTransform cam_local = scene.transform(camera);
    cam_local.translation = eye;
    cam_local.rotation = rot;
    (void)scene.set_transform(camera, cam_local);
}

void PlayRuntime::set_character_input(scene::Scene& scene,
                                      Entity entity,
                                      math::Vec3 walk_dir) noexcept {
    auto* cc = scene.registry().get<CharacterControllerComponent>(entity);
    if (cc != nullptr)
        cc->walk_dir = walk_dir;
}

math::Vec3 PlayRuntime::character_position(scene::Scene& scene, Entity entity) const noexcept {
    auto* cc = scene.registry().get<CharacterControllerComponent>(entity);
    if (cc == nullptr)
        return math::Vec3{0.0f, 0.0f, 0.0f};
    return peek_character_position(cc->character);
}

}  // namespace psynder::editor::play
