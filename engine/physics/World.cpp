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

// Resolve a BodyId to its live slot index, validating the FULL generation
// (not just gen != 0). Returns a pointer to the Body on success, nullptr for
// a stale / out-of-range / freed handle. This is the single decode gate the
// public mutators + queries share so a stale handle can never alias a freshly
// recreated body (UAF-class fix).
static Body* resolve_body(WorldState& w, BodyId id) noexcept {
    const u32 idx = handle_index(id.raw);
    if (idx >= w.bodies.size())
        return nullptr;
    Body& b = w.bodies[idx];
    // A destroyed slot keeps its generation (so reuse can bump it), so the
    // `alive` flag — not the generation — distinguishes a live body from a
    // freed hole. Validate BOTH: alive AND exact generation match.
    if (b.alive == 0 || b.gen != handle_gen(id.raw))
        return nullptr;  // freed slot or stale generation
    return &b;
}

// Internal integration helpers (kept in this TU; no point in their own .cpp
// because they share the body buffer with the orchestrator).
static void integrate_forces(WorldState& w, f32 dt) noexcept {
    for (Body& b : w.bodies) {
        if (b.alive == 0 || b.inv_mass == 0.0f || (b.flags & BodyFlags::Sleeping) != 0u)
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
        if (b.alive == 0 || b.inv_mass == 0.0f || (b.flags & BodyFlags::Sleeping) != 0u)
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
static void step_vehicles(WorldState& w, VehicleWorld& vw, f32 dt) noexcept {
    for (Vehicle& v : vw.vehicles) {
        if (v.alive == 0)
            continue;  // freed slot
        // Resolve the stored chassis BodyId with a FULL generation check so a
        // vehicle whose chassis was destroyed (and whose slot may have been
        // reused) is skipped rather than driving a different body.
        const u32 bidx = handle_index(v.chassis_body);
        if (bidx >= w.bodies.size())
            continue;
        Body& chassis = w.bodies[bidx];
        if (chassis.alive == 0 || chassis.gen != handle_gen(v.chassis_body) ||
            chassis.inv_mass == 0.0f)
            continue;  // stale, recycled, or static chassis
        vehicle_step(v, chassis, dt);
    }
}

static void rebuild_aabbs(WorldState& w) {
    w.aabb_scratch.clear();
    w.aabb_scratch.reserve(w.bodies.size());
    for (u32 i = 0; i < w.bodies.size(); ++i) {
        const Body& b = w.bodies[i];
        if (b.alive == 0)
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
    // contiguous and disjoint per island. IslandJobCtx is a WorldState member
    // type so its scratch vector can be reused across steps (Fix 2).
    using IslandJobCtx = WorldState::IslandJobCtx;

    static auto solve_one = [](void* user) noexcept {
        auto* ctx = static_cast<IslandJobCtx*>(user);
        solve_island(*ctx->island,
                     {ctx->contacts_base + ctx->island->first_contact, ctx->island->contact_count},
                     {ctx->body_index_base + ctx->island->first_body, ctx->island->body_count},
                     {ctx->bodies_base, ctx->bodies_count},
                     *ctx->params,
                     ctx->dt);
    };

    // Reuse the per-step scratch: clear (retain capacity) instead of freeing
    // and re-allocating every 120 Hz sub-tick.
    w.island_ctx_scratch.clear();
    w.island_ctx_scratch.reserve(w.islands.size());
    for (usize i = 0; i < w.islands.size(); ++i) {
        w.island_ctx_scratch.push_back(IslandJobCtx{
            &w.islands[i],
            w.contact_scratch.data(),
            w.island_body_indices.data(),
            w.bodies.data(),
            w.bodies.size(),
            &w.solver,
            dt,
        });
    }

    auto& js = jobs::JobSystem::Get();
    w.island_handle_scratch.clear();
    w.island_handle_scratch.reserve(w.island_ctx_scratch.size());
    for (auto& c : w.island_ctx_scratch) {
        w.island_handle_scratch.push_back(
            js.submit(jobs::JobDesc{solve_one, &c, "phys-island", 0}));
    }
    for (auto h : w.island_handle_scratch)
        js.wait(h);
}

}  // namespace detail

// ─── Lifetime ─────────────────────────────────────────────────────────────
//
// Each World owns one WorldImpl (rigid-body state + vehicle + character
// sub-worlds). The ctor/dtor/move are defined HERE — not inline in the
// header — because WorldImpl is incomplete at the header (PIMPL): the
// unique_ptr's deleter needs the full type, which only this TU sees.

World::World() : impl_(std::make_unique<detail::WorldImpl>()) {}
World::~World() = default;
World::World(World&&) noexcept = default;
World& World::operator=(World&&) noexcept = default;

namespace detail {

// Legacy default-world sub-state accessors. They forward into the ONE default
// World instance (World::Get()), so callers that historically reached for the
// "global" (the bench's world_state(), sample 09's character_world()) keep
// observing the same world the public default-world API mutates. New code owns
// its own World and never routes through here.
WorldState& world_state() {
    return World::Get().internal().state;
}
VehicleWorld& vehicle_world() {
    return World::Get().internal().vehicles;
}
CharacterWorld& character_world() {
    return World::Get().internal().characters;
}

}  // namespace detail

// ─── Public API ─────────────────────────────────────────────────────────

World& World::Get() {
    static World w;
    return w;
}

BodyId World::create_body(const BodyDesc& desc) {
    auto& w = impl_->state;
    std::lock_guard<std::mutex> lock(w.mutate);

    u32 idx;
    u32 reuse_gen = 1;  // generation to stamp on the (re)used slot
    if (!w.free_slots.empty()) {
        idx = w.free_slots.back();
        w.free_slots.pop_back();
        // The freed slot's prior generation was preserved in `gen` (we only
        // zero a transient marker on destroy); bump it so a stale handle that
        // still references this slot fails the decode equality check.
        reuse_gen = detail::handle_next_gen(w.bodies[idx].gen);
    } else {
        idx = static_cast<u32>(w.bodies.size());
        w.bodies.emplace_back();
        reuse_gen = w.bodies[idx].gen;  // fresh slot starts at gen == 1
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
    b.flags = (desc.mass <= 0.0f) ? detail::BodyFlags::Static : detail::BodyFlags::None;

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
    b.gen = reuse_gen;  // bumped on reuse, fresh slots start at 1
    b.alive = 1;
    return BodyId{detail::handle_encode(b.gen, idx)};
}

void World::destroy_body(BodyId id) {
    auto& w = impl_->state;
    std::lock_guard<std::mutex> lock(w.mutate);
    const u32 idx = detail::handle_index(id.raw);
    if (idx >= w.bodies.size())
        return;
    detail::Body& b = w.bodies[idx];
    // Reject a stale or already-freed handle: validate the FULL generation and
    // the alive flag. This guards against double-destroy (which would push the
    // same slot onto the free-list twice and hand it out for two live bodies).
    if (b.alive == 0 || b.gen != detail::handle_gen(id.raw))
        return;
    b.alive = 0;            // mark hole; KEEP gen so reuse can bump it
    w.free_slots.push_back(idx);
}

void World::step(f32 dt_seconds) {
    auto& w = impl_->state;
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
            if (b.alive == 0)
                continue;
            b.prev_position = b.position;
            b.prev_rotation = b.rotation;
        }

        // Vehicle solver runs first so its tire/suspension/aero forces are
        // in the chassis accumulators when integrate_forces consumes them.
        detail::step_vehicles(w, impl_->vehicles, dt);
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
    impl_->state.gravity = g;
}

void World::set_body_position(BodyId id, math::Vec3 p) {
    auto& w = impl_->state;
    std::lock_guard<std::mutex> lock(w.mutate);
    detail::Body* bp = detail::resolve_body(w, id);
    if (bp == nullptr)
        return;
    detail::Body& b = *bp;
    if (b.inv_mass == 0.0f)
        return;  // static — leave it pinned
    b.position = p;
    b.prev_position = p;  // no interpolation across the teleport
    b.linear_velocity = {0, 0, 0};
    b.angular_velocity = {0, 0, 0};
}

void World::set_body_velocity(BodyId id, math::Vec3 v) {
    auto& w = impl_->state;
    std::lock_guard<std::mutex> lock(w.mutate);
    detail::Body* bp = detail::resolve_body(w, id);
    if (bp == nullptr)
        return;
    detail::Body& b = *bp;
    if (b.inv_mass == 0.0f)
        return;
    b.linear_velocity = v;
}

void World::apply_impulse(BodyId id, math::Vec3 impulse) {
    auto& w = impl_->state;
    std::lock_guard<std::mutex> lock(w.mutate);
    detail::Body* bp = detail::resolve_body(w, id);
    if (bp == nullptr)
        return;
    detail::Body& b = *bp;
    if (b.inv_mass == 0.0f)
        return;
    b.linear_velocity = math::add(b.linear_velocity, math::mul(impulse, b.inv_mass));
}

void World::apply_torque(BodyId id, math::Vec3 torque) {
    auto& w = impl_->state;
    std::lock_guard<std::mutex> lock(w.mutate);
    detail::Body* bp = detail::resolve_body(w, id);
    if (bp == nullptr)
        return;
    detail::Body& b = *bp;
    if (b.inv_mass == 0.0f)
        return;
    // Accumulate into the torque the integrator consumes on the next step
    // (same lifecycle as Body::force). Inertia is applied there.
    b.torque = math::add(b.torque, torque);
}

void World::apply_angular_impulse(BodyId id, math::Vec3 impulse) {
    auto& w = impl_->state;
    std::lock_guard<std::mutex> lock(w.mutate);
    detail::Body* bp = detail::resolve_body(w, id);
    if (bp == nullptr)
        return;
    detail::Body& b = *bp;
    if (b.inv_mass == 0.0f)
        return;
    // w += I^-1 * J. Mirror the integrator's world-space diagonal convention
    // (integrate_forces) so impulse response matches accumulated-torque response.
    b.angular_velocity = math::add(b.angular_velocity,
                                   math::Vec3{impulse.x * b.inertia.inv_local.x,
                                              impulse.y * b.inertia.inv_local.y,
                                              impulse.z * b.inertia.inv_local.z});
}

void World::set_angular_velocity(BodyId id, math::Vec3 angular) {
    auto& w = impl_->state;
    std::lock_guard<std::mutex> lock(w.mutate);
    detail::Body* bp = detail::resolve_body(w, id);
    if (bp == nullptr)
        return;
    detail::Body& b = *bp;
    if (b.inv_mass == 0.0f)
        return;
    b.angular_velocity = angular;
}

math::Vec3 World::get_position(BodyId id) const {
    auto& w = impl_->state;
    const detail::Body* bp = detail::resolve_body(w, id);
    if (bp == nullptr)
        return {0, 0, 0};
    // Render lerp between prev and current using accumulator alpha.
    const detail::Body& b = *bp;
    f32 a = w.alpha;
    return {
        b.prev_position.x + (b.position.x - b.prev_position.x) * a,
        b.prev_position.y + (b.position.y - b.prev_position.y) * a,
        b.prev_position.z + (b.position.z - b.prev_position.z) * a,
    };
}

math::Quat World::get_rotation(BodyId id) const {
    auto& w = impl_->state;
    const detail::Body* bp = detail::resolve_body(w, id);
    if (bp == nullptr)
        return {0, 0, 0, 1};
    // Quaternion lerp + normalise (cheap nlerp; for sub-step alpha the error
    // is sub-degree). Render side renormalises again before matrix conversion.
    const detail::Body& b = *bp;
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
