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
#include "internal/Kernels.h"
#include "internal/Raycast.h"
#include "internal/IntegrateKernels.h"

#include "jobs/JobSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
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
        // SIMD per-body linear update (Lane 4). Bit-identical to the two scalar
        // component-wise adds it replaces: gravity*dt then force*(inv_mass*dt),
        // each as an f32x4 vertical add/mul. See IntegrateKernels.h.
        // External forces accumulated via the public API in Wave B (apply_force).
        b.linear_velocity =
            integrate::forces_linear_simd(b.linear_velocity, w.gravity, b.force, b.inv_mass, dt);
        // Angular: alpha = R * I_local^-1 * R^T * torque. Diagonal inertia is
        // stored in the body's LOCAL principal frame, so rotate world torque
        // into local, scale by the inverse diagonal, then rotate the result
        // back to world (apply_inv_inertia_world). This is the properly
        // rotated tensor — a tumbling asymmetric body now precesses correctly
        // instead of the old world-space-diagonal approximation.
        math::Vec3 ang_accel =
            detail::apply_inv_inertia_world(b.rotation, b.inertia.inv_local, b.torque);
        b.angular_velocity = math::add(b.angular_velocity, math::mul(ang_accel, dt));
        b.force = {0, 0, 0};
        b.torque = {0, 0, 0};
    }
}

static void integrate_positions(WorldState& w, f32 dt) noexcept {
    for (Body& b : w.bodies) {
        if (b.alive == 0 || b.inv_mass == 0.0f || (b.flags & BodyFlags::Sleeping) != 0u)
            continue;
        // SIMD per-body linear update (Lane 4): position += linear_velocity*dt
        // as an f32x4 vertical mul/add. Bit-identical to the scalar add/mul.
        b.position = integrate::position_linear_simd(b.position, b.linear_velocity, dt);

        // Quaternion integration: q' = q + 0.5 * dt * w * q, then renormalise.
        // KEPT SCALAR (Lane 4): the per-component `rotation.c + 0.5f*dt*dq.c`
        // is FMA-contracted by the compiler (default -ffp-contract=on, which
        // -fno-fast-math does NOT disable) into a single fused multiply-add.
        // A separate-rounding SIMD mul+add would drop that fusion and drift by
        // 1 ULP - so this stays byte-for-byte on the original scalar path. The
        // linear updates above DON'T contract (proven 0-ULP at -O0 and -O2), so
        // they are vectorised; this one is not. quat_mul / quat_normalize also
        // stay scalar (cross terms; exact sqrt + reciprocal).
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

static void rebuild_aabbs(WorldState& w, f32 dt) {
    w.aabb_scratch.clear();
    w.aabb_scratch.reserve(w.bodies.size());
    for (u32 i = 0; i < w.bodies.size(); ++i) {
        const Body& b = w.bodies[i];
        if (b.alive == 0)
            continue;  // hole
        math::Aabb box = aabb_world(b.shape, b.half_extent, b.position, b.rotation);

        // Speculative anti-tunnelling: expand a FAST dynamic body's AABB by its
        // swept displacement this tick (velocity * dt) so an about-to-cross
        // pair is found THIS step rather than after it has already tunnelled.
        // Gated on the body actually moving more than the speculative margin in
        // one tick, so slow / resting bodies keep their EXACT prior AABB — the
        // candidate-pair set (and therefore every existing resting contact) is
        // byte-for-byte unchanged. The expansion is one-sided per axis toward
        // the motion direction. Static bodies (inv_mass == 0) never sweep.
        if (b.inv_mass > 0.0f) {
            // SIMD per-body swept displacement (Lane 4): linear_velocity*dt as
            // an f32x4 vertical mul. Bit-identical to math::mul. The per-axis
            // sign-select expansion below stays scalar (branchy, already exact).
            const math::Vec3 disp = integrate::swept_disp_simd(b.linear_velocity, dt);
            const f32 disp_len2 = math::dot(disp, disp);
            const f32 margin = detail::kernels::kSpeculativeMargin;
            if (disp_len2 > margin * margin) {
                if (disp.x < 0.0f)
                    box.min.x += disp.x;
                else
                    box.max.x += disp.x;
                if (disp.y < 0.0f)
                    box.min.y += disp.y;
                else
                    box.max.y += disp.y;
                if (disp.z < 0.0f)
                    box.min.z += disp.z;
                else
                    box.max.z += disp.z;
            }
        }
        w.aabb_scratch.push_back({box.min, box.max, i});
    }
}

static void run_narrowphase(WorldState& w, f32 dt) {
    w.contact_scratch.clear();
    w.contact_scratch.reserve(w.pair_scratch.size());
    const f32 margin = detail::kernels::kSpeculativeMargin;
    for (const CandidatePair& p : w.pair_scratch) {
        Contact c;
        // Speculative-aware collide: penetrating pairs behave exactly as
        // before; a fast pair about to cross emits a speculative contact the
        // solver clamps. Pairs with no closed-form gap (box-box / capsule /
        // GJK) fall through to the unchanged overlap path inside.
        if (collide_pair_spec(w.bodies[p.a], w.bodies[p.b], dt, margin, c)) {
            c.body_a = p.a;
            c.body_b = p.b;
            w.contact_scratch.push_back(c);
        }
    }
}

static void run_island_solve(WorldState& w, f32 dt) {
    detect_islands(w.contact_scratch, {w.bodies.data(), w.bodies.size()}, w.island_body_indices, w.islands);

    // Each island is independent — dispatch one job per island (island-level
    // parallelism). WITHIN a large island we additionally run the ADR-013
    // graph-colored solve so a colour's disjoint-body contacts spread across
    // cores via parallel_for (intra-island parallelism). Contacts and
    // body-index slices are contiguous and disjoint per island. IslandJobCtx is
    // a WorldState member type so its scratch vectors reuse across steps.
    using IslandJobCtx = WorldState::IslandJobCtx;

    static auto solve_one = [](void* user) noexcept {
        auto* ctx = static_cast<IslandJobCtx*>(user);
        const u32 contact_count = ctx->island->contact_count;

        // Per-colour batch dispatcher. Above the threshold we route a colour's
        // disjoint-body contacts through the job system's parallel_for; below it
        // we run serial. EITHER way the result is bit-identical (a colour's
        // bodies are disjoint, so the partition cannot change the arithmetic),
        // so determinism holds whether or not the pool is running and regardless
        // of core count.
        //
        // The per-contact solve is LIGHT (tens of ns), so a parallel_for
        // fork/join (~3-4us measured) only pays off when a colour is large.
        // Two guards keep us on the right side of break-even: (1) the whole
        // island must clear kColoredParallelThreshold, and (2) inside the
        // dispatcher a colour smaller than kColoredColorMinParallel runs inline
        // (no task submit), so the many tiny box-box colours never fork. Grain
        // is large (kColoredParallelGrain) because chunk-submission cost — not
        // load imbalance — is the bottleneck at this work density (empirically
        // ~512 is the sweet spot; bigger underutilises, smaller over-submits).
        kernels::ColorBatchDispatch dispatch;
        if (contact_count >= kernels::kColoredParallelThreshold) {
            dispatch = [](usize count, const std::function<void(usize, usize)>& fn) {
                if (count < kernels::kColoredColorMinParallel) {
                    fn(0, count);  // tiny colour: inline, skip the fork/join
                    return;
                }
                // parallel_for falls back to a synchronous body() call when the
                // pool is stopped or the range is one chunk.
                jobs::JobSystem::Get().parallel_for(0, count, kernels::kColoredParallelGrain, fn);
            };
        } else {
            dispatch = kernels::solver_serial_dispatch;
        }

        solve_island_colored(
            *ctx->island,
            {ctx->contacts_base + ctx->island->first_contact, ctx->island->contact_count},
            {ctx->body_index_base + ctx->island->first_body, ctx->island->body_count},
            {ctx->bodies_base, ctx->bodies_count},
            *ctx->params,
            ctx->dt,
            *ctx->solver_scratch,
            dispatch);
    };

    // Reuse the per-step scratch: clear (retain capacity) instead of freeing
    // and re-allocating every 120 Hz sub-tick. One colored-solve scratch per
    // island, pooled across frames.
    if (w.island_solver_scratch.size() < w.islands.size())
        w.island_solver_scratch.resize(w.islands.size());
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
            &w.island_solver_scratch[i],
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
        case Shape::Plane:
            // An infinite half-space has no finite rotational inertia; treat it
            // as non-rotating (zero local inertia -> zero inverse inertia
            // below). Planes are normally static (mass 0) anyway.
            In = {0, 0, 0};
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
        // Swept AABB + speculative narrowphase run AFTER integrate_forces so
        // the velocity they sweep with is the post-gravity velocity this tick.
        detail::rebuild_aabbs(w, dt);
        detail::broadphase_sap(w.aabb_scratch, w.pair_scratch);
        detail::run_narrowphase(w, dt);
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
    // w += I^-1 * J with the properly rotated tensor: dw = R*(I_local^-1 (.)
    // (R^T*J)). Mirrors integrate_forces so impulse response matches the
    // accumulated-torque response on a rotated asymmetric body.
    b.angular_velocity = math::add(
        b.angular_velocity,
        detail::apply_inv_inertia_world(b.rotation, b.inertia.inv_local, impulse));
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

World::RaycastHit World::raycast(math::Vec3 origin,
                                 math::Vec3 dir,
                                 f32 max_t,
                                 BodyId ignore) const noexcept {
    RaycastHit result{};
    const auto& w = impl_->state;

    // Normalise the direction so `t` is a true distance in metres; a degenerate
    // (zero-length) dir can never hit anything.
    const f32 dir_len = math::length(dir);
    if (!(dir_len > 0.0f) || !(max_t > 0.0f))
        return result;
    const math::Vec3 d = math::mul(dir, 1.0f / dir_len);

    // Decode `ignore` once: skip the shooter's own slot only when its slot AND
    // generation are live (a stale ignore handle must not blanket-skip a reused
    // slot). kSkip is an index that never matches a real slot when ignore is
    // empty/stale.
    constexpr u32 kSkip = 0xFFFFFFFFu;
    u32 ignore_idx = kSkip;
    if (ignore.raw != 0u) {
        const u32 ii = detail::handle_index(ignore.raw);
        if (ii < w.bodies.size() && w.bodies[ii].alive != 0 &&
            w.bodies[ii].gen == detail::handle_gen(ignore.raw))
            ignore_idx = ii;
    }

    f32 best_t = max_t;  // shrinks as nearer hits are found
    for (u32 i = 0; i < w.bodies.size(); ++i) {
        const detail::Body& b = w.bodies[i];
        if (b.alive == 0 || i == ignore_idx)
            continue;  // hole or the shooter's own body

        // Broad prefilter: skip bodies whose (conservative, possibly fattened)
        // world AABB the ray does not enter within the current best distance.
        // This is the same AABB the broadphase builds, so a body that can't be
        // hit is rejected before the exact per-shape solve.
        const math::Aabb box =
            detail::aabb_world(b.shape, b.half_extent, b.position, b.rotation);
        f32 broad_t;
        if (!detail::ray_aabb(origin, d, box, best_t, broad_t))
            continue;

        detail::RayShapeHit hit{};
        bool got = false;
        switch (static_cast<Shape>(b.shape)) {
            case Shape::Sphere:
                got = detail::ray_sphere(origin, d, b.position, b.half_extent.x,
                                         best_t, hit);
                break;
            case Shape::Capsule:
                got = detail::ray_capsule(origin, d, b.position, b.rotation,
                                          b.half_extent.x, b.half_extent.y, best_t, hit);
                break;
            case Shape::Box:
            // ConvexHull / Compound / Heightfield / TriangleMesh fall back to the
            // OBB of their half_extent (Wave-A; lane-13 Wave B refines these).
            default:
                got = detail::ray_obb(origin, d, b.position, b.rotation, b.half_extent,
                                      best_t, hit);
                break;
        }

        if (got && hit.hit && hit.t <= best_t) {
            best_t = hit.t;
            result.body = BodyId{detail::handle_encode(b.gen, i)};
            result.t = hit.t;
            result.point = hit.point;
            result.normal = hit.normal;
            result.hit = true;
        }
    }
    return result;
}

}  // namespace psynder::physics
