// SPDX-License-Identifier: MIT
// Psynder — M-AI enemy-AI systems implementation.

#include "ai/AiSystems.h"

#include "gameplay/GameplayComponents.h"
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

// ─── act ─────────────────────────────────────────────────────────────────
namespace {

// Apply one kinematic step toward `goal`, honouring move_speed*dt and stopping
// at the goal. When the straight path is blocked (LOS hook reports an
// obstruction), sidestep perpendicular instead of burrowing into the wall —
// navigation v1's "simple obstacle-avoid". Writes through ctx.apply_move when
// set, else straight into the Transform. Returns nothing (mutates xform).
void steer_toward(EcsRegistry& registry,
                  AiContext& ctx,
                  Entity self,
                  TransformComponent& xform,
                  const AiAgentComponent& agent,
                  math::Vec3 goal,
                  f32 dt) {
    (void)registry;
    const math::Vec3 pos = xform.local.translation;
    math::Vec3 to = math::sub(goal, pos);
    const f32 dist = math::length(to);
    if (!(dist > 1e-4f) || !(agent.move_speed > 0.0f) || !(dt > 0.0f))
        return;
    math::Vec3 dir = math::mul(to, 1.0f / dist);

    // Obstacle-avoid v1: if the direct line to the goal is blocked, steer
    // perpendicular (in the horizontal plane) so the agent slides along cover
    // rather than walking into it. No navmesh — a grid/navmesh path is a
    // follow-up wave (see header).
    if (ctx.los != nullptr && !ctx.los(ctx.los_user, pos, goal)) {
        // Perpendicular in XZ: rotate the heading 90 deg about +Y.
        math::Vec3 side{dir.z, 0.0f, -dir.x};
        const f32 sl = math::length(side);
        if (sl > 0.0f)
            dir = math::mul(side, 1.0f / sl);
    }

    const f32 step = std::min(agent.move_speed * dt, dist);
    const math::Vec3 desired = math::add(pos, math::mul(dir, step));
    const math::Vec3 reached =
        ctx.apply_move ? ctx.apply_move(ctx.move_user, self, desired) : desired;
    xform.local.translation = reached;
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
                            steer_toward(registry, ctx, self, xforms[i], agent, wp, dt);
                        }
                        break;
                    }
                    case AiState::Chase: {
                        // Move toward the last place the target was seen. The
                        // PerceptionComponent memory means a chase continues even
                        // after the target slips behind cover.
                        steer_toward(registry, ctx, self, xforms[i], agent,
                                     senses[i].last_seen_pos, dt);
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
