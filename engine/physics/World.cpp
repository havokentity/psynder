// SPDX-License-Identifier: MIT
// Psynder physics — `World` facade + the fixed-timestep tick orchestrator
// (DESIGN.md §10.1).
//
// The public `step(dt)` accumulates real time and advances the sim by as
// many 1/120 s sub-ticks as fit, leaving the residual as `alpha` for render
// interpolation. Each sub-tick is:
//     1. Snapshot prev_position / prev_rotation
//     2. Integrate forces → velocities, apply gravity
//     3. Build world-AABB scratch list
//     4. Broadphase SAP (3 axes in parallel)
//     5. Narrowphase collide_pair on each candidate
//     6. Detect islands (union-find)
//     7. Per-island solve_island (parallel)
//     8. Integrate velocities → positions
//     9. Clear accumulators

#include "Physics.h"
#include "WorldImpl.h"
#include "Shape.h"
#include "Vehicle.h"
#include "FpControl.h"

#include "jobs/JobSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace psynder::physics {

namespace detail {

WorldState& world_state() {
    static WorldState s;
    return s;
}

// Internal integration helpers (kept in this TU; no point in their own .cpp
// because they share the body buffer with the orchestrator).
static void integrate_forces(WorldState& w, f32 dt) noexcept {
    for (Body& b : w.bodies) {
        if (b.inv_mass == 0.0f || (b.flags & kFlagSleeping))
            continue;
        b.linear_velocity = math::add(b.linear_velocity, math::mul(w.gravity, dt));
        // External forces accumulated via the public API in Wave B (apply_force).
        b.linear_velocity = math::add(b.linear_velocity, math::mul(b.force, b.inv_mass * dt));
        // Angular: I^-1 * torque * dt — diagonal inertia in local space, so
        // rotate torque into local, scale by inv_inertia, rotate back. For
        // Wave A we approximate with world-space diagonal — sufficient for
        // sphere/box at rest and good enough until a vehicle stresses it.
        math::Vec3 ang_accel{
            b.torque.x * b.inertia.inv_local.x,
            b.torque.y * b.inertia.inv_local.y,
            b.torque.z * b.inertia.inv_local.z,
        };
        b.angular_velocity = math::add(b.angular_velocity, math::mul(ang_accel, dt));
        b.force = {0, 0, 0};
        b.torque = {0, 0, 0};
    }
}

static void integrate_positions(WorldState& w, f32 dt) noexcept {
    for (Body& b : w.bodies) {
        if (b.inv_mass == 0.0f || (b.flags & kFlagSleeping))
            continue;
        b.position = math::add(b.position, math::mul(b.linear_velocity, dt));

        // Quaternion integration: q' = q + 0.5 * dt * w * q, then renormalise.
        math::Quat w_q{b.angular_velocity.x, b.angular_velocity.y, b.angular_velocity.z, 0.0f};
        math::Quat dq = math::quat_mul(w_q, b.rotation);
        b.rotation = math::quat_normalize({
            b.rotation.x + 0.5f * dt * dq.x,
            b.rotation.y + 0.5f * dt * dq.y,
            b.rotation.z + 0.5f * dt * dq.z,
            b.rotation.w + 0.5f * dt * dq.w,
        });
    }
}

// Run the vehicle solver for every live vehicle, accumulating tire +
// suspension + aero forces onto each chassis body's force/torque before the
// integrator consumes (and clears) them this sub-tick. Maps a vehicle's
// stored chassis handle back to its Body slot via the low 24 bits, skipping
// stale handles. Single-threaded with the rest of the tick — no allocation.
static void step_vehicles(WorldState& w, f32 dt) noexcept {
    VehicleWorld& vw = vehicle_world();
    for (Vehicle& v : vw.vehicles) {
        if (v.gen == 0)
            continue;  // freed slot
        const u32 bidx = v.chassis_body & 0x00FFFFFFu;
        if (bidx >= w.bodies.size())
            continue;
        Body& chassis = w.bodies[bidx];
        if (chassis.gen == 0 || chassis.inv_mass == 0.0f)
            continue;  // stale or static chassis
        vehicle_step(v, chassis, dt);
    }
}

static void rebuild_aabbs(WorldState& w) {
    w.aabb_scratch.clear();
    w.aabb_scratch.reserve(w.bodies.size());
    for (u32 i = 0; i < w.bodies.size(); ++i) {
        const Body& b = w.bodies[i];
        if (b.gen == 0)
            continue;  // hole
        math::Aabb box = aabb_world(b.shape, b.half_extent, b.position, b.rotation);
        w.aabb_scratch.push_back({box.min, box.max, i});
    }
}

static void run_narrowphase(WorldState& w) {
    w.contact_scratch.clear();
    w.contact_scratch.reserve(w.pair_scratch.size());
    for (const CandidatePair& p : w.pair_scratch) {
        Contact c;
        if (collide_pair(w.bodies[p.a], w.bodies[p.b], c)) {
            c.body_a = p.a;
            c.body_b = p.b;
            w.contact_scratch.push_back(c);
        }
    }
}

static void run_island_solve(WorldState& w, f32 dt) {
    detect_islands(w.contact_scratch, {w.bodies.data(), w.bodies.size()}, w.island_body_indices, w.islands);

    // Each island is independent — dispatch one job per island. Lane 04's
    // Phase-0 stub runs jobs synchronously, but the data layout is correct
    // for the real Chase-Lev pool: contacts and body-index slices are
    // contiguous and disjoint per island.
    struct IslandJobCtx {
        const Island* island;
        Contact* contacts_base;
        const u32* body_index_base;
        Body* bodies_base;
        usize bodies_count;
        const SolverParams* params;
        f32 dt;
    };

    static auto solve_one = [](void* user) noexcept {
        auto* ctx = static_cast<IslandJobCtx*>(user);
        solve_island(*ctx->island,
                     {ctx->contacts_base + ctx->island->first_contact, ctx->island->contact_count},
                     {ctx->body_index_base + ctx->island->first_body, ctx->island->body_count},
                     {ctx->bodies_base, ctx->bodies_count},
                     *ctx->params,
                     ctx->dt);
    };

    std::vector<IslandJobCtx> ctxs(w.islands.size());
    for (usize i = 0; i < w.islands.size(); ++i) {
        ctxs[i] = IslandJobCtx{
            &w.islands[i],
            w.contact_scratch.data(),
            w.island_body_indices.data(),
            w.bodies.data(),
            w.bodies.size(),
            &w.solver,
            dt,
        };
    }

    auto& js = jobs::JobSystem::Get();
    std::vector<jobs::JobHandle> handles;
    handles.reserve(ctxs.size());
    for (auto& c : ctxs) {
        handles.push_back(js.submit(jobs::JobDesc{solve_one, &c, "phys-island", 0}));
    }
    for (auto h : handles)
        js.wait(h);
}

}  // namespace detail

// ─── Public API ─────────────────────────────────────────────────────────

World& World::Get() {
    static World w;
    return w;
}

BodyId World::create_body(const BodyDesc& desc) {
    auto& w = detail::world_state();
    std::lock_guard<std::mutex> lock(w.mutate);

    u32 idx;
    if (!w.free_slots.empty()) {
        idx = w.free_slots.back();
        w.free_slots.pop_back();
    } else {
        idx = static_cast<u32>(w.bodies.size());
        w.bodies.emplace_back();
    }
    detail::Body& b = w.bodies[idx];

    b.position = desc.position;
    b.rotation = desc.rotation;
    b.prev_position = desc.position;
    b.prev_rotation = desc.rotation;
    b.linear_velocity = {0, 0, 0};
    b.angular_velocity = {0, 0, 0};
    b.force = {0, 0, 0};
    b.torque = {0, 0, 0};
    b.mass = desc.mass;
    b.inv_mass = (desc.mass > 0.0f) ? (1.0f / desc.mass) : 0.0f;
    b.friction = desc.friction;
    b.restitution = desc.restitution;
    b.shape = static_cast<u8>(desc.shape);
    b.half_extent = desc.half_extent;
    b.flags = (desc.mass <= 0.0f) ? detail::kFlagStatic : 0;

    // Inertia
    math::Vec3 In;
    switch (desc.shape) {
        case Shape::Sphere:
            In = detail::inertia_sphere(desc.mass, desc.half_extent.x);
            break;
        case Shape::Capsule:
            In = detail::inertia_capsule(desc.mass, desc.half_extent.x, desc.half_extent.y);
            break;
        case Shape::Box:
        default:
            In = detail::inertia_box(desc.mass, desc.half_extent);
            break;
    }
    b.inertia.local = In;
    if (b.inv_mass == 0.0f) {
        b.inertia.inv_local = {0, 0, 0};
    } else {
        b.inertia.inv_local = {
            In.x > 0.0f ? 1.0f / In.x : 0.0f,
            In.y > 0.0f ? 1.0f / In.y : 0.0f,
            In.z > 0.0f ? 1.0f / In.z : 0.0f,
        };
    }
    if (b.gen == 0)
        b.gen = 1;  // re-used slot
    return BodyId{(b.gen << 24) | (idx & 0x00FFFFFFu)};
}

void World::destroy_body(BodyId id) {
    auto& w = detail::world_state();
    std::lock_guard<std::mutex> lock(w.mutate);
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.bodies.size())
        return;
    w.bodies[idx].gen = 0;
    w.free_slots.push_back(idx);
}

void World::step(f32 dt_seconds) {
    auto& w = detail::world_state();
    detail::FpGuard fp_guard;  // round-to-nearest + denormals on, scope-bound

    w.accumulator += dt_seconds;

    // Cap the accumulator so a long stall doesn't spin us forever; "spiral
    // of death" guard (DESIGN.md §10.1 fixed-tick contract).
    constexpr f32 kMaxAccum = 0.25f;  // 250 ms cap
    if (w.accumulator > kMaxAccum)
        w.accumulator = kMaxAccum;

    while (w.accumulator >= detail::WorldState::kFixedDt) {
        const f32 dt = detail::WorldState::kFixedDt;

        // Snapshot previous transform for render interpolation.
        for (detail::Body& b : w.bodies) {
            if (b.gen == 0)
                continue;
            b.prev_position = b.position;
            b.prev_rotation = b.rotation;
        }

        // Vehicle solver runs first so its tire/suspension/aero forces are
        // in the chassis accumulators when integrate_forces consumes them.
        detail::step_vehicles(w, dt);
        detail::integrate_forces(w, dt);
        detail::rebuild_aabbs(w);
        detail::broadphase_sap(w.aabb_scratch, w.pair_scratch);
        detail::run_narrowphase(w);
        detail::run_island_solve(w, dt);
        detail::integrate_positions(w, dt);

        w.accumulator -= dt;
    }
    w.alpha = w.accumulator / detail::WorldState::kFixedDt;
}

void World::set_gravity(math::Vec3 g) {
    detail::world_state().gravity = g;
}

void World::set_body_position(BodyId id, math::Vec3 p) {
    auto& w = detail::world_state();
    std::lock_guard<std::mutex> lock(w.mutate);
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.bodies.size() || w.bodies[idx].gen == 0)
        return;
    detail::Body& b = w.bodies[idx];
    if (b.inv_mass == 0.0f)
        return;  // static — leave it pinned
    b.position = p;
    b.prev_position = p;  // no interpolation across the teleport
    b.linear_velocity = {0, 0, 0};
    b.angular_velocity = {0, 0, 0};
}

void World::set_body_velocity(BodyId id, math::Vec3 v) {
    auto& w = detail::world_state();
    std::lock_guard<std::mutex> lock(w.mutate);
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.bodies.size() || w.bodies[idx].gen == 0)
        return;
    detail::Body& b = w.bodies[idx];
    if (b.inv_mass == 0.0f)
        return;
    b.linear_velocity = v;
}

void World::apply_impulse(BodyId id, math::Vec3 impulse) {
    auto& w = detail::world_state();
    std::lock_guard<std::mutex> lock(w.mutate);
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.bodies.size() || w.bodies[idx].gen == 0)
        return;
    detail::Body& b = w.bodies[idx];
    if (b.inv_mass == 0.0f)
        return;
    b.linear_velocity = math::add(b.linear_velocity, math::mul(impulse, b.inv_mass));
}

math::Vec3 World::get_position(BodyId id) const {
    auto& w = detail::world_state();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.bodies.size() || w.bodies[idx].gen == 0)
        return {0, 0, 0};
    // Render lerp between prev and current using accumulator alpha.
    const detail::Body& b = w.bodies[idx];
    f32 a = w.alpha;
    return {
        b.prev_position.x + (b.position.x - b.prev_position.x) * a,
        b.prev_position.y + (b.position.y - b.prev_position.y) * a,
        b.prev_position.z + (b.position.z - b.prev_position.z) * a,
    };
}

math::Quat World::get_rotation(BodyId id) const {
    auto& w = detail::world_state();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.bodies.size() || w.bodies[idx].gen == 0)
        return {0, 0, 0, 1};
    // Quaternion lerp + normalise (cheap nlerp; for sub-step alpha the error
    // is sub-degree). Render side renormalises again before matrix conversion.
    const detail::Body& b = w.bodies[idx];
    f32 a = w.alpha;
    math::Quat q{
        b.prev_rotation.x + (b.rotation.x - b.prev_rotation.x) * a,
        b.prev_rotation.y + (b.rotation.y - b.prev_rotation.y) * a,
        b.prev_rotation.z + (b.rotation.z - b.prev_rotation.z) * a,
        b.prev_rotation.w + (b.rotation.w - b.prev_rotation.w) * a,
    };
    return math::quat_normalize(q);
}

}  // namespace psynder::physics
