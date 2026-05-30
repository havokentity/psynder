// SPDX-License-Identifier: MIT
// Psynder — M-AI enemy-AI systems implementation.

#include "ai/AiSystems.h"

#include "ai/SquadCoord.h"
#include "gameplay/GameplayComponents.h"
#include "jobs/JobSystem.h"
#include "scene/EcsRegistry.h"
#include "scene/SceneEcs.h"

#include <cmath>
#include <span>

namespace psynder::ai {

using namespace ::psynder::scene;

namespace {

// Faction lookup mirroring gameplay::entity_faction without coupling to its
// CombatContext: HealthComponent::faction first, FactionComponent fallback,
// 0 => neutral/world. AI only engages a DIFFERENT, non-zero faction.
[[nodiscard]] u32 read_faction(EcsRegistry& registry, Entity e) {
    if (const auto* health = registry.get<HealthComponent>(e))
        return health->faction;
    if (const auto* faction = registry.get<gameplay::FactionComponent>(e))
        return faction->faction;
    return 0u;
}

// Read an entity's translation. AI operates in Transform space directly (same
// space combat + the LOS hook use), so no scene-graph world flatten is needed.
[[nodiscard]] bool read_position(EcsRegistry& registry, Entity e, math::Vec3& out) {
    if (const auto* t = registry.get<TransformComponent>(e)) {
        out = t->local.translation;
        return true;
    }
    return false;
}

// The agent's facing forward (world +Z rotated by the Transform rotation). Used
// for the FOV cone test. Kept here so perceive stays the only place that knows
// the facing convention.
[[nodiscard]] math::Vec3 agent_forward(const TransformComponent& t) {
    // Rotate (0,0,1) by the unit quaternion. Standard q * v * q^-1 expanded for
    // a forward axis; cheap + branch-free, no normalize needed for a unit quat.
    const math::Quat q = t.local.rotation;
    const f32 x = q.x, y = q.y, z = q.z, w = q.w;
    math::Vec3 f{};
    f.x = 2.0f * (x * z + w * y);
    f.y = 2.0f * (y * z - w * x);
    f.z = 1.0f - 2.0f * (x * x + y * y);
    const f32 len = math::length(f);
    return len > 0.0f ? math::mul(f, 1.0f / len) : math::Vec3{0.0f, 0.0f, 1.0f};
}

}  // namespace

// ─── perceive ────────────────────────────────────────────────────────────
void perceive(EcsRegistry& registry, AiContext& ctx, f32 dt) {
    if (dt > 0.0f)
        ctx.time += dt;
    const f32 now = ctx.time;

    // Outer query: one body per chunk of agents. Each body writes ONLY into the
    // agent's own AiAgentComponent + PerceptionComponent rows, so there is no
    // cross-row mutation and no AI-side lock is needed even though bodies run on
    // many worker threads at once.
    registry.query<reads<SceneNodeComponent, TransformComponent>,
                   writes<AiAgentComponent, PerceptionComponent>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const TransformComponent> xforms,
            std::span<AiAgentComponent> agents,
            std::span<PerceptionComponent> senses) {
            const usize n =
                std::min({nodes.size(), xforms.size(), agents.size(), senses.size()});
            for (usize i = 0; i < n; ++i) {
                AiAgentComponent& agent = agents[i];
                PerceptionComponent& sense = senses[i];
                if (agent.state == AiState::Dead) {
                    sense.can_see = 0u;
                    agent.target_entity = Entity{};
                    continue;
                }

                const Entity self = nodes[i].entity;
                const u32 self_faction = read_faction(registry, self);
                const math::Vec3 eye = xforms[i].local.translation;
                const math::Vec3 fwd = agent_forward(xforms[i]);
                const f32 sight2 = agent.sight_range * agent.sight_range;

                // Nearest visible hostile, found via a NESTED read-only query
                // over candidate targets. Nested queries are re-entrancy- and
                // thread-safe (each stands up its own stack scratch + chunk
                // work-list) and allocate nothing in the steady state. A
                // per-agent best is reduced on this body's own stack — never
                // shared — so no lock is required.
                Entity best{};
                math::Vec3 best_pos{};
                f32 best_dist2 = sight2;

                registry.query<reads<SceneNodeComponent, TransformComponent>, writes<>>(
                    [&](std::span<const SceneNodeComponent> tnodes,
                        std::span<const TransformComponent> txforms) {
                        const usize tn = std::min(tnodes.size(), txforms.size());
                        for (usize j = 0; j < tn; ++j) {
                            const Entity cand = tnodes[j].entity;
                            if (cand == self)
                                continue;
                            const u32 cf = read_faction(registry, cand);
                            // Only engage a different, non-neutral faction.
                            if (cf == 0u || cf == self_faction)
                                continue;
                            // A target must be damageable (have health) and
                            // alive — corpses are not chased.
                            const auto* hp = registry.get<HealthComponent>(cand);
                            if (!hp || hp->current_health <= 0.0f)
                                continue;

                            const math::Vec3 tpos = txforms[j].local.translation;
                            const math::Vec3 to = math::sub(tpos, eye);
                            const f32 d2 = math::dot(to, to);
                            if (d2 > best_dist2)
                                continue;  // farther than the current best / out of sight
                            // FOV cone: cos(angle) = dot(fwd, dir). Skip the
                            // sqrt when fov_cos <= -1 (omnidirectional).
                            if (agent.fov_cos > -1.0f) {
                                const f32 d = std::sqrt(d2);
                                if (d > 0.0f) {
                                    const f32 cosang = math::dot(fwd, math::mul(to, 1.0f / d));
                                    if (cosang < agent.fov_cos)
                                        continue;
                                }
                            }
                            best = cand;
                            best_pos = tpos;
                            best_dist2 = d2;
                        }
                    });

                bool visible = false;
                if (best.valid()) {
                    // Confirm line of sight through the injected probe. With no
                    // LOS hook wired the agent is treated as having clear sight
                    // (pure-perception unit tests / hosts without geometry).
                    visible = (ctx.los == nullptr) || ctx.los(ctx.los_user, eye, best_pos);
                }

                if (visible) {
                    agent.target_entity = best;
                    sense.can_see = 1u;
                    sense.last_seen_pos = best_pos;
                    sense.last_seen_time = now;
                } else {
                    // Keep the remembered target+position (Chase-to-last-seen),
                    // but report it is not currently visible. Only forget the
                    // target entirely when nothing hostile is in the sight
                    // sphere at all (best invalid).
                    sense.can_see = 0u;
                    if (!best.valid())
                        agent.target_entity = Entity{};
                    else
                        agent.target_entity = best;  // remembered, just unseen
                }
            }
        });
}

// ─── think ───────────────────────────────────────────────────────────────
void think(EcsRegistry& registry, AiContext& ctx, f32 dt) {
    (void)ctx;
    registry.query<reads<SceneNodeComponent, TransformComponent, PerceptionComponent>,
                   writes<AiAgentComponent>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const TransformComponent> xforms,
            std::span<const PerceptionComponent> senses,
            std::span<AiAgentComponent> agents) {
            const usize n =
                std::min({nodes.size(), xforms.size(), senses.size(), agents.size()});
            for (usize i = 0; i < n; ++i) {
                AiAgentComponent& agent = agents[i];

                // Death overrides every state and is terminal — checked every
                // tick regardless of the think throttle so a dying agent stops
                // immediately.
                const Entity self = nodes[i].entity;
                if (const auto* hp = registry.get<HealthComponent>(self);
                    hp && hp->current_health <= 0.0f) {
                    agent.state = AiState::Dead;
                    agent.target_entity = Entity{};
                    continue;
                }
                if (agent.state == AiState::Dead)
                    continue;

                // Per-agent think throttle: only re-evaluate transitions when
                // the cooldown elapses, then re-arm it. Cheap crowds.
                if (agent.think_cooldown > 0.0f) {
                    agent.think_cooldown -= dt;
                    if (agent.think_cooldown > 0.0f)
                        continue;
                }
                agent.think_cooldown = agent.think_interval;

                const PerceptionComponent& sense = senses[i];
                const bool has_target = agent.target_entity.valid();
                const bool seen = sense.can_see != 0u && has_target;

                if (seen) {
                    // Distance to the visible target decides Chase vs Attack.
                    const math::Vec3 self_pos = xforms[i].local.translation;
                    const math::Vec3 to = math::sub(sense.last_seen_pos, self_pos);
                    const f32 d2 = math::dot(to, to);
                    const f32 r2 = agent.attack_range * agent.attack_range;
                    agent.state = (d2 <= r2) ? AiState::Attack : AiState::Chase;
                } else if (has_target) {
                    // Target known but not visible right now => pursue its last
                    // known position instead of attacking blind.
                    agent.state = AiState::Chase;
                } else {
                    // Nothing to fight: patrol if a route is configured, else
                    // idle. (act() no-ops Idle, so a route-less agent rests.)
                    const auto* patrol = registry.get<PatrolComponent>(self);
                    agent.state =
                        (patrol && patrol->count > 0u) ? AiState::Patrol : AiState::Idle;
                }
            }
        });
}

// ─── navigate ──────────────────────────────────────────────────────────────
namespace {

// Resolve the world goal an agent is heading for, given its FSM state. Returns
// false when the agent has no movement goal this tick (Idle / Attack / Dead, or
// a route-less Patrol). Chase heads for the last-seen position; Patrol heads for
// its current waypoint.
[[nodiscard]] bool resolve_goal(EcsRegistry& registry,
                                Entity self,
                                const AiAgentComponent& agent,
                                const PerceptionComponent& sense,
                                math::Vec3& out_goal) {
    switch (agent.state) {
        case AiState::Chase:
            out_goal = sense.last_seen_pos;
            return true;
        case AiState::Patrol: {
            const PatrolComponent* patrol = registry.get<PatrolComponent>(self);
            if (patrol == nullptr || patrol->count == 0u)
                return false;
            const u32 cur = patrol->current < patrol->count ? patrol->current : 0u;
            out_goal = patrol->waypoints[cur];
            return true;
        }
        case AiState::Idle:
        case AiState::Attack:
        case AiState::Dead:
        default:
            return false;
    }
}

// Map a job worker index (0..N-1, or ~0u for the main thread) to a NavQuery
// pool slot, wrapping deterministically into the fixed pool.
[[nodiscard]] u32 nav_slot_for_worker() noexcept {
    const u32 w = jobs::JobSystem::Get().current_worker();
    const u32 slot = (w == ~0u) ? 0u : (w + 1u);
    return slot % AiContext::kNavQueryPool;
}

// Apply the optional squad / flank layer to a resolved Chase goal. With the
// layer off (or a non-Chase goal) this returns the goal UNCHANGED, so the default
// single-agent path is bit-for-bit preserved. With it on, a Chase goal is offset
// to the agent's distinct flank-slot position around the target (its last-seen
// position is the ring anchor, so flanking continues even after the target hides).
// When `count` is set AND the offset actually moved the goal (a flanker, not a
// head-on suppressor / lone agent), the ctx.flankers telemetry is bumped — only
// `act` passes count=true so the tally is exactly one per flanking agent even
// when `navigate` also offsets the same agent's routed goal. Deterministic +
// alloc-free (SquadCoord scans the engaging set read-only, reducing onto the
// stack).
[[nodiscard]] math::Vec3 apply_squad_offset(EcsRegistry& registry,
                                            AiContext& ctx,
                                            Entity self,
                                            const AiAgentComponent& agent,
                                            math::Vec3 self_pos,
                                            math::Vec3 goal,
                                            bool count) {
    if (!ctx.squad.enabled || agent.state != AiState::Chase)
        return goal;
    const math::Vec3 flank =
        squad_flank_goal(registry, ctx.squad, self, agent, goal, self_pos);
    if (count) {
        // Count this agent as a flanker only when the slot actually displaced its
        // goal (a head-on suppressor / lone engager gets goal back unchanged).
        const math::Vec3 d = math::sub(flank, goal);
        if (math::dot(d, d) > 1e-6f)
            ctx.flankers.fetch_add(1u, std::memory_order_relaxed);
    }
    return flank;
}

}  // namespace

void navigate(EcsRegistry& registry, AiContext& ctx, f32 dt) {
    if (ctx.nav_grid == nullptr)
        return;  // navigation disabled — straight-line steer in act() (unchanged).
    const NavGrid& grid = *ctx.nav_grid;

    // Size every NavQuery pool slot to this grid UP FRONT, once, whenever the
    // bound grid's cell count changes (a freshly-bound or resized grid). This
    // moves the one-time scratch allocation OUT of the hot query: find_path()'s
    // lazy self-size guard then never fires in steady state, so the FIRST path on
    // a newly-bound grid performs zero heap growth inside the query. Sizing all
    // slots (not just the ones a given run touches) keeps it deterministic and
    // independent of how the job system schedules chunks across workers. reset()
    // on an already-correctly-sized query is O(1) (it only re-stamps), so a grid
    // that never changes size pays this cost exactly once.
    if (ctx.nav_sized_cells != grid.cell_count()) {
        for (NavQuery& q : ctx.nav_query)
            q.reset(grid);
        ctx.nav_sized_cells = grid.cell_count();
    }

    registry.query<reads<SceneNodeComponent, TransformComponent, PerceptionComponent>,
                   writes<AiAgentComponent, NavAgentComponent>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const TransformComponent> xforms,
            std::span<const PerceptionComponent> senses,
            std::span<AiAgentComponent> agents,
            std::span<NavAgentComponent> navs) {
            const usize n = std::min(
                {nodes.size(), xforms.size(), senses.size(), agents.size(), navs.size()});
            if (n == 0)
                return;
            // One scratch slot per worker (see header) — claimed once per chunk
            // body, reused across this body's rows. No allocation.
            NavQuery& query = ctx.nav_query[nav_slot_for_worker()];
            for (usize i = 0; i < n; ++i) {
                AiAgentComponent& agent = agents[i];
                NavAgentComponent& nav = navs[i];
                // Snapshot the agent's position every tick so the parallel act()
                // separation step reads a stable neighbour position rather than a
                // value another worker is mid-write on. Done before any early-out.
                nav.last_pos = xforms[i].local.translation;
                if (agent.state == AiState::Dead) {
                    nav.has_path = 0u;
                    nav.count = 0u;
                    continue;
                }

                math::Vec3 goal{};
                if (!resolve_goal(registry, nodes[i].entity, agent, senses[i], goal)) {
                    // No movement goal this tick — drop any stale path.
                    nav.has_path = 0u;
                    continue;
                }
                // Squad / flank layer (opt-in): offset a Chase goal to this
                // agent's distinct slot around the target so the squad surrounds
                // instead of stacking. No-op (goal unchanged) when squad.enabled
                // is off => the existing routed behaviour is preserved exactly.
                goal = apply_squad_offset(registry, ctx, nodes[i].entity, agent,
                                          xforms[i].local.translation, goal,
                                          /*count*/ false);

                if (nav.repath_cooldown > 0.0f)
                    nav.repath_cooldown -= dt;

                // Decide whether to repath: no path yet, the cooldown elapsed, or
                // the goal drifted past repath_dist from what we planned for.
                const math::Vec3 drift = math::sub(goal, nav.planned_goal);
                const f32 drift2 = math::dot(drift, drift);
                const f32 rd2 = nav.repath_dist * nav.repath_dist;
                const bool need =
                    nav.has_path == 0u || nav.repath_cooldown <= 0.0f || drift2 > rd2;
                if (!need)
                    continue;

                nav.repath_cooldown = nav.repath_interval;
                ctx.repaths.fetch_add(1u, std::memory_order_relaxed);

                const math::Vec3 pos = xforms[i].local.translation;
                const NavCell start = grid.world_to_cell(pos);
                const NavCell gcell = grid.world_to_cell(goal);

                NavPath path;
                const bool ok = query.find_path(grid, start, gcell, path);
                if (ok && path.count > 0u) {
                    smooth_path(grid, path);
                    const u32 keep = path.count < NavAgentComponent::kMaxWaypoints
                                         ? path.count
                                         : NavAgentComponent::kMaxWaypoints;
                    for (u32 k = 0; k < keep; ++k)
                        nav.waypoints[k] = path.points[k];
                    nav.count = keep;
                    // Start walking toward the first waypoint that is not the
                    // cell we already stand in (index 0 is the start cell).
                    nav.cursor = (keep > 1u) ? 1u : 0u;
                    nav.has_path = 1u;
                    nav.planned_goal = goal;
                } else {
                    // No route (goal walled off / off-grid): clear the path so
                    // act() falls back to the straight-line steer + LOS nudge
                    // rather than freezing. Clean, alloc-free, no crash.
                    nav.has_path = 0u;
                    nav.count = 0u;
                    nav.planned_goal = goal;
                }
            }
        });
}

// ─── act ─────────────────────────────────────────────────────────────────
namespace {

// Light, deterministic local-avoidance separation. Sums a push away from every
// other agent within `nav.separation_radius`, reading NEIGHBOURS' position
// SNAPSHOTS (NavAgentComponent::last_pos, captured in the serial-ordered
// navigate pass) rather than their live Transform — so this never races the
// Transform writes act() makes from other workers. Alloc-free (a nested
// read-only query, same pattern perceive uses), deterministic (sum is
// commutative + order-independent; the snapshot is fixed for the whole pass).
// Returns a world-space offset to add to the agent's desired step.
[[nodiscard]] math::Vec3 separation_nudge(EcsRegistry& registry,
                                          Entity self,
                                          math::Vec3 pos,
                                          f32 radius,
                                          f32 weight,
                                          f32 dt) {
    if (!(radius > 0.0f) || !(weight > 0.0f) || !(dt > 0.0f))
        return math::Vec3{0.0f, 0.0f, 0.0f};
    const f32 r2 = radius * radius;
    math::Vec3 push{0.0f, 0.0f, 0.0f};
    registry.query<reads<SceneNodeComponent, NavAgentComponent>, writes<>>(
        [&](std::span<const SceneNodeComponent> nnodes,
            std::span<const NavAgentComponent> nnavs) {
            const usize m = std::min(nnodes.size(), nnavs.size());
            for (usize j = 0; j < m; ++j) {
                if (nnodes[j].entity == self)
                    continue;
                const math::Vec3 other = nnavs[j].last_pos;
                math::Vec3 away = math::sub(pos, other);
                away.y = 0.0f;  // separate in the ground plane only
                const f32 d2 = math::dot(away, away);
                if (d2 >= r2 || !(d2 > 1e-8f))
                    continue;
                // Linear falloff: stronger the closer they are. 1 - d/radius.
                const f32 d = std::sqrt(d2);
                const f32 falloff = 1.0f - d / radius;
                push = math::add(push, math::mul(away, falloff / d));
            }
        });
    return math::mul(push, weight * dt);
}

// Apply one kinematic step toward `goal`, honouring move_speed*dt and stopping
// at the goal, plus an optional separation offset. When the straight path is
// blocked (LOS hook reports an obstruction) AND the agent is NOT following a
// routed nav path, sidestep perpendicular instead of burrowing into the wall —
// the original navigation-v1 fallback. When a nav path IS being followed the
// route already goes around geometry, so we do not perpendicular-nudge (that
// would fight the path). Writes through ctx.apply_move when set, else straight
// into the Transform.
void steer_toward(EcsRegistry& registry,
                  AiContext& ctx,
                  Entity self,
                  TransformComponent& xform,
                  const AiAgentComponent& agent,
                  math::Vec3 goal,
                  math::Vec3 separation,
                  bool on_nav_path,
                  f32 dt) {
    (void)registry;
    const math::Vec3 pos = xform.local.translation;
    math::Vec3 to = math::sub(goal, pos);
    const f32 dist = math::length(to);
    if (!(dist > 1e-4f) || !(agent.move_speed > 0.0f) || !(dt > 0.0f)) {
        // Even when "arrived" at the (sub)goal, still apply separation so a
        // stacked cluster spreads out instead of freezing on top of each other.
        if (math::dot(separation, separation) > 0.0f) {
            const math::Vec3 d = math::add(pos, separation);
            xform.local.translation =
                ctx.apply_move ? ctx.apply_move(ctx.move_user, self, d) : d;
        }
        return;
    }
    math::Vec3 dir = math::mul(to, 1.0f / dist);

    // Obstacle-avoid v1 (only off-path): if the direct line to the goal is
    // blocked, steer perpendicular so the agent slides along cover rather than
    // walking into it. With a routed nav path this is unnecessary (and would
    // fight the route), so it is suppressed.
    if (!on_nav_path && ctx.los != nullptr && !ctx.los(ctx.los_user, pos, goal)) {
        math::Vec3 side{dir.z, 0.0f, -dir.x};
        const f32 sl = math::length(side);
        if (sl > 0.0f)
            dir = math::mul(side, 1.0f / sl);
    }

    const f32 step = std::min(agent.move_speed * dt, dist);
    const math::Vec3 desired =
        math::add(math::add(pos, math::mul(dir, step)), separation);
    const math::Vec3 reached =
        ctx.apply_move ? ctx.apply_move(ctx.move_user, self, desired) : desired;
    xform.local.translation = reached;
}

// Pick the world goal an agent should steer toward this tick. When it owns a
// NavAgentComponent with a live route, follow the smoothed waypoint at the
// cursor, advancing the cursor as each is reached; the LAST waypoint is the real
// goal. Returns the sub-goal and whether the agent is currently on a nav path
// (so steer_toward can skip the perpendicular wall-nudge). `fallback` is the
// straight-line goal used when there is no route.
[[nodiscard]] math::Vec3 follow_goal(EcsRegistry& registry,
                                     Entity self,
                                     math::Vec3 pos,
                                     math::Vec3 fallback,
                                     bool& out_on_path) {
    NavAgentComponent* nav = registry.get<NavAgentComponent>(self);
    out_on_path = false;
    if (nav == nullptr || nav->has_path == 0u || nav->count == 0u)
        return fallback;
    if (nav->cursor >= nav->count)
        nav->cursor = nav->count - 1u;
    // Advance the cursor past any waypoints already reached.
    while (nav->cursor < nav->count - 1u) {
        const math::Vec3 wp = nav->waypoints[nav->cursor];
        if (math::length(math::sub(wp, pos)) <= nav->arrive_radius)
            ++nav->cursor;
        else
            break;
    }
    out_on_path = true;
    return nav->waypoints[nav->cursor];
}

}  // namespace

void act(EcsRegistry& registry, AiContext& ctx, f32 dt) {
    // PatrolComponent is intentionally NOT part of the query signature: a query
    // matches only entities that own EVERY listed component, so requiring Patrol
    // here would silently skip every Chase/Attack/Idle agent that has no route.
    // We fetch the route on demand via registry.get<PatrolComponent>() inside the
    // Patrol branch instead — the only branch that needs it. This is a per-row,
    // single-entity read/write (the agent's own route), so it stays lock-free.
    registry.query<reads<SceneNodeComponent, PerceptionComponent>,
                   writes<AiAgentComponent, TransformComponent>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const PerceptionComponent> senses,
            std::span<AiAgentComponent> agents,
            std::span<TransformComponent> xforms) {
            const usize n =
                std::min({nodes.size(), senses.size(), agents.size(), xforms.size()});
            for (usize i = 0; i < n; ++i) {
                AiAgentComponent& agent = agents[i];
                const Entity self = nodes[i].entity;

                // Separation snapshot offset (zero unless the agent has a
                // NavAgentComponent with separation enabled). Read once per row;
                // uses neighbour position snapshots => race-free.
                math::Vec3 sep{0.0f, 0.0f, 0.0f};
                if (const NavAgentComponent* nav = registry.get<NavAgentComponent>(self);
                    nav != nullptr && nav->separation_radius > 0.0f) {
                    sep = separation_nudge(registry, self, xforms[i].local.translation,
                                           nav->separation_radius, nav->separation_weight,
                                           dt);
                }

                switch (agent.state) {
                    case AiState::Patrol: {
                        PatrolComponent* patrol = registry.get<PatrolComponent>(self);
                        if (patrol == nullptr || patrol->count == 0u)
                            break;
                        if (patrol->current >= patrol->count)
                            patrol->current = 0u;
                        const math::Vec3 wp = patrol->waypoints[patrol->current];
                        const math::Vec3 pos = xforms[i].local.translation;
                        const f32 d = math::length(math::sub(wp, pos));
                        if (d <= patrol->arrive_radius) {
                            // Arrived — dwell, then advance to the next waypoint.
                            if (patrol->wait_timer > 0.0f) {
                                patrol->wait_timer -= dt;
                            } else {
                                patrol->current = (patrol->current + 1u) % patrol->count;
                                patrol->wait_timer = patrol->wait_time;
                            }
                        } else {
                            // Follow the routed nav path toward the patrol
                            // waypoint when one exists, else straight-line.
                            bool on_path = false;
                            const math::Vec3 goal =
                                follow_goal(registry, self, pos, wp, on_path);
                            steer_toward(registry, ctx, self, xforms[i], agent, goal,
                                         sep, on_path, dt);
                        }
                        break;
                    }
                    case AiState::Chase: {
                        // Move toward the last place the target was seen, routed
                        // around geometry via the nav path when available. The
                        // PerceptionComponent memory means a chase continues even
                        // after the target slips behind cover.
                        const math::Vec3 pos = xforms[i].local.translation;
                        // Squad / flank layer (opt-in): when no routed nav path is
                        // followed (the straight-line steer case — e.g. df_demo's
                        // terrain soldiers), offset the straight-line fallback goal
                        // to this agent's flank slot so the squad surrounds the
                        // target. With a nav path the navigate pass already routed
                        // to the same offset goal, so follow_goal walks the routed
                        // corridor and the fallback is unused. `count`=true makes
                        // act the single owner of the ctx.flankers tally.
                        const math::Vec3 fallback = apply_squad_offset(
                            registry, ctx, self, agent, pos,
                            senses[i].last_seen_pos, /*count*/ true);
                        bool on_path = false;
                        const math::Vec3 goal =
                            follow_goal(registry, self, pos, fallback, on_path);
                        steer_toward(registry, ctx, self, xforms[i], agent, goal, sep,
                                     on_path, dt);
                        break;
                    }
                    case AiState::Attack: {
                        // Stand and shoot via the host hook (combat fire_weapon
                        // wrapper). The hook is the ONLY shared side effect and
                        // the host makes it thread-safe; AI adds no accumulator.
                        if (ctx.fire != nullptr && agent.target_entity.valid()) {
                            if (ctx.fire(ctx.fire_user, self, agent.target_entity))
                                ctx.shots_fired.fetch_add(1u, std::memory_order_relaxed);
                        }
                        break;
                    }
                    case AiState::Idle:
                    case AiState::Dead:
                    default:
                        break;
                }
            }
        });
}

}  // namespace psynder::ai
