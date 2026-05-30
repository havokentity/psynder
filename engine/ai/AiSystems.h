// SPDX-License-Identifier: MIT
// Psynder — M-AI enemy-AI systems (DOTS). Free functions + an AiSystems facade
// that operate over the AiComponents.h components via
// `registry.query<reads<...>, writes<...>>`. Three systems compose a tick:
//
//     perceive(...)  // sense  — fill PerceptionComponent (nearest visible hostile)
//     think(...)     // decide — step the AiAgentComponent FSM
//     act(...)       // do     — move (Transform writeback) + fire (host hook)
//
// PARALLEL-SAFE / ALLOC-FREE contract (matches gameplay::CombatContext + the
// scene::gather_* patterns):
//   * query bodies fire once per chunk across worker threads. Each system writes
//     ONLY into its own per-row output component (perceive -> PerceptionComponent,
//     think -> AiAgentComponent, act -> TransformComponent / PatrolComponent) so
//     there is NO cross-row shared mutation to guard. The only shared side
//     effects — firing a weapon, applying movement the host owns — are funnelled
//     through host hooks that the host makes thread-safe (combat already
//     serializes its damage queue under a mutex). AI itself adds no shared
//     accumulator, so the per-chunk bodies need no AI-side lock.
//   * No per-frame heap allocation: AiContext owns the injected hooks + a clock
//     and a small fixed scratch; nothing in perceive/think/act allocates. The
//     `perceive` nearest-hostile search re-scans the (read-only) hostile set per
//     agent rather than building a list, keeping it allocation-free at the cost
//     of an O(agents * targets) sweep (a spatial accel is a follow-up wave).
//
// HOST-AGNOSTIC BOUNDARY: side effects (LOS, fire, movement-apply) go through
// hooks set ONCE on the AiContext, never per-frame. This module therefore
// depends only on scene/gameplay/math — never render/host/net. The host wires
// AiContext::los to a physics / combat raycast, AiContext::fire to combat's
// fire_weapon, and (optionally) AiContext::apply_move to its own character
// controller; if apply_move is null, `act` writes the kinematic step straight
// into the agent's TransformComponent.

#pragma once

#include "ai/AiComponents.h"
#include "ai/NavGrid.h"
#include "ai/SquadCoord.h"
#include "core/Types.h"
#include "math/Math.h"
#include "scene/EcsRegistry.h"
#include "scene/SceneEcs.h"

#include <atomic>

namespace psynder::ai {

using ::psynder::Entity;
using ::psynder::f32;
using ::psynder::u32;

// ─── Host hooks ────────────────────────────────────────────────────────────
// Plain function pointers + a void* user payload so AI stays decoupled from the
// host without pulling in <functional> heap allocation. Set ONCE at load.

// Line-of-sight probe: returns true when nothing blocks the segment
// origin -> target. The host typically wires this to combat's raycast_hitboxes
// or a physics raycast. `user` is AiContext::los_user.
using LosFn = bool (*)(void* user, math::Vec3 origin, math::Vec3 target);

// Fire hook: the agent attacks `target`. The host wraps combat's fire_weapon
// (which is itself parallel-safe via its mutex-guarded damage queue). Returns
// true if a shot actually went out (e.g. not on cooldown / out of ammo) so the
// facade can count shots in tests. `user` is AiContext::fire_user.
using FireFn = bool (*)(void* user, Entity agent, Entity target);

// Movement-apply hook (optional). When set, `act` hands the host the desired
// post-step world position instead of writing the TransformComponent directly,
// so a host with a character controller / collision can own the final move.
// Return the position actually reached (the host may clamp it against geometry).
// `user` is AiContext::move_user.
using ApplyMoveFn = math::Vec3 (*)(void* user, Entity agent, math::Vec3 desired);

// ─── AiContext ───────────────────────────────────────────────────────────
// Reusable per-AI-world state. Construct ONE and reuse it every frame. Holds
// the injected host hooks + their user payloads, a monotonically-accumulated
// clock (so PerceptionComponent::last_seen_time is comparable across ticks),
// and a couple of counters tests assert on. Alloc-free: no owned containers.
struct AiContext {
    // Hooks — set once at load, never per-frame.
    LosFn los = nullptr;
    void* los_user = nullptr;
    FireFn fire = nullptr;
    void* fire_user = nullptr;
    ApplyMoveFn apply_move = nullptr;  // null => act writes Transform directly
    void* move_user = nullptr;

    // Accumulated seconds since the context was created. perceive() advances it
    // by dt at the top of its sweep so all agents stamp the same tick time.
    f32 time = 0.0f;

    // Lightweight telemetry. `act` runs across worker threads, so this is an
    // atomic relaxed counter (an Attack-state agent bumps it once per shot the
    // host hook actually lets through). Reset via begin_tick(). It is the only
    // shared mutable field AI itself touches from a parallel body; making it
    // atomic keeps the act() body lock-free + race-free without an allocation.
    std::atomic<u32> shots_fired{0u};

    // ── Navigation (optional) ───────────────────────────────────────────────
    // When `nav_grid` is set, the `navigate` pass routes Chase/Patrol movement
    // around blocked cells with A* (writing each agent's NavAgentComponent
    // waypoint buffer); `act` then FOLLOWS those waypoints instead of
    // straight-lining toward the goal. When `nav_grid` is null (the default —
    // every existing host + test), navigation is a no-op and `act` keeps its
    // original straight-line steer-toward behaviour. The host owns the grid and
    // feeds it via the NavGrid builder API (NavGrid.h), exactly like it owns the
    // LOS / fire hooks.
    const NavGrid* nav_grid = nullptr;

    // A* scratch is alloc-free but mutated in place, so it cannot be shared
    // across the worker threads `navigate` runs its query on. We keep a small
    // fixed POOL — one NavQuery per worker slot (+1 for the main thread, mapped
    // to slot 0). `navigate` picks its slot by jobs::current_worker(); chunks on
    // one worker run sequentially (safe reuse), distinct workers use distinct
    // slots (no race). kNavQueryPool comfortably exceeds any realistic core
    // count; an out-of-range worker index wraps into the pool deterministically.
    static constexpr u32 kNavQueryPool = 64u;
    NavQuery nav_query[kNavQueryPool];

    // Cell count the nav_query pool was last sized for. navigate() primes every
    // pool slot to the bound grid up front (reset()) whenever this differs from
    // the grid's current cell count, so the FIRST A* on a freshly-bound grid
    // grows no scratch inside the hot query (find_path()'s lazy self-size never
    // fires in steady state). usize(~0) means "not yet sized for any grid".
    usize nav_sized_cells = static_cast<usize>(~usize{0});

    // Repaths run this tick (telemetry / tests). Multiple workers may bump it
    // from the navigate pass, so it is atomic-relaxed like shots_fired.
    std::atomic<u32> repaths{0u};

    // ── Squad / flanking (optional, OPT-IN) ──────────────────────────────────
    // When squad.enabled is set, agents engaging a COMMON target are handed
    // DISTINCT approach slots around it (see SquadCoord.h): each Chase goal is
    // offset to its flank-slot position so the squad SURROUNDS the target instead
    // of stacking single-file. The slot is a deterministic function of the
    // engaging agents' stable ids (no RNG / time), recomputed each tick as the
    // engaging set changes. When squad.enabled is false (the default — every
    // existing host + test), the goal resolver returns the raw target position and
    // behaviour is bit-for-bit the old single-agent path. Set once, like the hooks.
    SquadConfig squad{};

    // Agents handed a distinct (non-head-on) flank slot this tick (telemetry /
    // tests). Bumped from the navigate pass across workers => atomic-relaxed.
    std::atomic<u32> flankers{0u};

    void begin_tick() noexcept {
        shots_fired.store(0u, std::memory_order_relaxed);
        repaths.store(0u, std::memory_order_relaxed);
        flankers.store(0u, std::memory_order_relaxed);
    }
};

// ─── perceive ────────────────────────────────────────────────────────────
// Sense pass. For each live, non-Dead agent, find the NEAREST hostile-faction
// entity (different faction, alive, has a Transform) within sight_range and
// inside the agent's FOV cone, then confirm visibility with the injected LOS
// probe. Updates the agent's PerceptionComponent (can_see / last_seen_pos /
// last_seen_time) and acquires/clears AiAgentComponent::target_entity.
//
// Query: reads<SceneNodeComponent, TransformComponent>,
//        writes<AiAgentComponent, PerceptionComponent>.
// Per-row writes only (each agent touches its own components) => no AI-side
// lock. Advances ctx.time by dt once, before the sweep.
void perceive(scene::EcsRegistry& registry, AiContext& ctx, f32 dt);

// ─── think ───────────────────────────────────────────────────────────────
// Decision pass. Steps each agent's FSM from the PerceptionComponent snapshot +
// HealthComponent, honouring per-agent think_cooldown:
//   any state, health<=0           -> Dead (terminal)
//   Idle                           -> Patrol (if it has a route) else stays Idle
//   target seen                    -> Chase, then Attack when within attack_range
//   target lost                    -> Chase toward last-seen, then back to Patrol
// Pure per-row writes to AiAgentComponent => no lock, alloc-free.
//
// Query: reads<SceneNodeComponent, PerceptionComponent>, writes<AiAgentComponent>.
void think(scene::EcsRegistry& registry, AiContext& ctx, f32 dt);

// ─── navigate ──────────────────────────────────────────────────────────────
// Planning pass (between think and act). For each non-Dead agent that owns a
// NavAgentComponent, when ctx.nav_grid is set it (re)plans an A* route to the
// agent's current goal (the Chase last-seen position or the active Patrol
// waypoint), THROTTLED: a fresh A* only runs when the per-agent repath cooldown
// elapses OR the goal drifted more than repath_dist from the planned goal. The
// routed + string-pull-smoothed waypoints are written into the agent's own
// NavAgentComponent; no cross-row state is touched.
//
// Parallel-safe + alloc-free: A* needs the in-place NavQuery scratch, which
// cannot be shared across the worker threads the query body runs on, so each
// chunk body borrows its own slot from ctx.nav_query[] keyed by the current
// worker index (chunks on one worker run sequentially => safe reuse; distinct
// workers => distinct slots => no race). Each agent writes only its own
// NavAgentComponent, and A* is deterministic per (grid, start, goal), so the
// routed waypoint list is identical regardless of how chunks are scheduled.
// When ctx.nav_grid is null this pass is a no-op (the default for every existing
// host / test).
//
// Query: reads<SceneNodeComponent, TransformComponent, PerceptionComponent>,
//        writes<AiAgentComponent, NavAgentComponent>.
void navigate(scene::EcsRegistry& registry, AiContext& ctx, f32 dt);

// ─── act ─────────────────────────────────────────────────────────────────
// Action pass. Drives the world from the FSM state:
//   Patrol  -> follow the routed nav path (if any) toward the current waypoint;
//              dwell + advance the patrol ring on arrival.
//   Chase   -> follow the routed nav path toward PerceptionComponent::last_seen_pos.
//   Attack  -> call ctx.fire(agent, target) (no movement).
//   Idle/Dead -> nothing.
// Movement: when the agent owns a NavAgentComponent with a path (planned by the
// `navigate` pass), `act` steers toward the NEXT smoothed waypoint and advances
// the cursor as each is reached — so the agent routes AROUND walls/cover instead
// of into them. Without a nav path it falls back to the original navigation v1
// kinematic steer-toward (move_speed * dt clamped to the remaining distance plus
// a cheap LOS-based perpendicular obstacle nudge). A light, deterministic
// separation nudge keeps co-pathing agents from stacking. Movement is applied
// via ctx.apply_move when set, else written straight into TransformComponent.
//
// Because fire is a shared side effect, `act` runs the Attack branch through the
// host's (thread-safe) fire hook; everything else is per-row Transform writes.
// Alloc-free.
//
// Query: reads<SceneNodeComponent, PerceptionComponent>,
//        writes<AiAgentComponent, TransformComponent>.
// PatrolComponent is deliberately NOT in the signature (it would exclude every
// route-less agent from the query); the Patrol branch fetches the agent's own
// route on demand via registry.get<PatrolComponent>().
void act(scene::EcsRegistry& registry, AiContext& ctx, f32 dt);

// ─── AiSystems facade ──────────────────────────────────────────────────────
// Thin wrapper so a host can hold one object and drive a full AI tick
// (perceive -> think -> act). Owns its AiContext. Stateless beyond that.
struct AiSystems {
    AiContext ctx{};

    // One full AI step. dt in seconds. The serial `navigate` pass sits between
    // `think` (decides the goal) and `act` (follows the route); it is a no-op
    // unless a host has wired ctx.nav_grid, so the default tick is unchanged.
    void update(scene::EcsRegistry& registry, f32 dt) {
        ctx.begin_tick();
        perceive(registry, ctx, dt);
        think(registry, ctx, dt);
        navigate(registry, ctx, dt);
        act(registry, ctx, dt);
    }
};

}  // namespace psynder::ai
