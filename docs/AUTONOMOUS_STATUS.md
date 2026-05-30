<!-- SPDX-License-Identifier: MIT -->
# Psynder — Autonomous Build Status (live log)

Read `docs/AUTONOMOUS_MISSION.md` for the operating loop. This file is the
running ledger: update it every wake-up. Newest entries on top.

## Current branch
`integration/wave-hybrid-material-shadows` (push to origin every cycle).

## Resume mechanism (survive usage-limit pauses)
- In-session: `ScheduleWakeup` heartbeat (~30 min, re-armed each turn) + background-agent completions auto-re-invoke.
- Cross-pause: recurring cron `2a5cb303` (hourly at :23) fires the mission-continue prompt at the first idle tick after a usage-limit reset. IDEMPOTENT — it integrates only what's ready and won't start redundant waves while lanes are in flight. Auto-expires after 7 days (re-create if needed).
- Cross-restart (full session exit): this file + AUTONOMOUS_MISSION.md + memory are the durable record; a fresh session resumes from here.

## Wave 1
- M-COMBAT gameplay core: LANDED `9007b38` (new engine/gameplay lib; hitscan/projectile/damage/death DOTS systems; 14 tests; release-green). Gap flagged: no public `physics::World::raycast(origin,dir,max_t)` — gameplay sweeps its own HitboxComponents; add a public raycast so combat hits physics bodies/static geometry (real cover). [tracked]
- M-HYB hybrid shadows: LANDED `d764114`. Raster primary + per-pixel hard shadow rays (sun/point/spot), Hybrid mode, opaque ShadowOccluder trampoline (raster core doesn't link rt), goldens unchanged, release-green. Deferred fidelity: soft penumbra, terrain heightmap-march occlusion, spot-cone sampling.
- M-PSYGRAPH visual scripting (new engine/script/psygraph) — in flight.

## Wave 1 — COMPLETE (HEAD f2275e6): M-COMBAT 9007b38, M-HYB d764114, M-PSYGRAPH f2275e6. All release-green, goldens 4/4.

## Wave 2 (in flight)
- #69 physics World::raycast (engine/physics) — public scene raycast for LOS/bullets.
- M-AI (new engine/ai) — perception/state-machine/nav-v1 driving combat via host hooks.
- M-NET (engine/net) — ECS snapshot+delta replication over existing Loopback/Frame/Snapshot; loopback convergence test.

## Wave 2 — COMPLETE (HEAD 342cca6): #69 raycast 89b5e33, M-AI 80eefd3, M-NET 342cca6. All release-green, goldens 4/4.

## Wave 3 (in flight)
- DEMO GAME 1: indoor BSP shooter (games/shooter_demo) — level + hybrid render + physics + combat + AI, FP controls, headless smoke.
- Editor play runs gameplay: PlayRuntime ticks combat + AI (LOS via world_.raycast, fire via gameplay) + PsyGraph during Play mode.

## Wave 3 — COMPLETE (HEAD 71c0dfc): editor-play gameplay a7992cb, shooter_demo 71c0dfc.
First playable FPS demo (games/shooter_demo): BSP room + hybrid shadows + combat + AI; headless run player killed all 3 enemies. Editor Play mode now ticks combat/AI/PsyGraph. Release-green, goldens 4/4.

## Wave 4 — COMPLETE (HEAD 9e43de2): render fidelity 4714819, anti-tunneling 6ede3ef (#63), racer demo 9e43de2. All release-green, goldens 4/4; golden+plane+tunneling 19/19.
- Render fidelity 4714819: soft-penumbra shadows (R2-jittered multi-ray, deterministic) + terrain heightmap-march MAX-combine occlusion.
- Anti-tunneling 6ede3ef (#63 DONE): Shape::Plane half-space primitive (sphere/box/capsule narrowphase) + speculative contacts (swept-AABB broadphase + separation contact + velocity clamp). No fake thickness. Deferred: box-box/GJK speculative coverage; Plane not yet in scene::ColliderShape for editor authoring.
- DEMO GAME 2 9e43de2: NFS2SE-style racer (games/racer_demo) — spline track + vehicle physics + chase cam + lap timing. Deferred: vehicle speed-governor/steering-authority weak under auto-drive; terrain elevation (flat track only).

## Wave 5 — COMPLETE (HEAD 9b14f4b): netcode 08eeb99, DF terrain demo c917163, navmesh AI 9b14f4b. All release-green; full suite 883 cases / 541074 assertions x3, goldens 4/4, all 3 demos build release.
- Netcode hardening 08eeb99 (engine/net): real localhost UDP transport (non-blocking 127.0.0.1, pooled recv) + entity-despawn replication (PRP1->PRP2 per-client despawn set, baseline-drop, client scrub) + client prediction & server reconciliation (Predictor/ServerInputProcessor over pure step_state + 256-cap InputRing, PredictedComponent). 3 new [net] tests + real-socket UDP smoke (ephemeral 57978). Next: AOI-gated per-peer snapshots, handshake/lobby, server input jitter buffer.
- DEMO GAME 3 c917163 (games/df_demo): Delta Force-style terrain tactical shooter. 256x256 heightmap terrain (loader MeshLibrary patch), bilinear-grounded FP player + 6 ranged AI soldiers (Health/Faction/Hitbox/AiAgent/Perception/Patrol/Weapon), Hybrid render w/ sun + WIRED terrain heightmap-march shadow occluder (long ridge shadows), LOS = terrain march + World::raycast body occlusion, fire = gameplay::fire_weapon. Alloc-free frame loop. Smoke: player grounded (eye 17.9/ground 16.2), 8 hitscans 104->52m, 3 kills, exit 0. Deferred: drivable vehicle/heli on terrain (physics::vehicle::set_ground_plane is flat-y only — engine gap shared w/ racer); multi-objective flow, networked MP, ragdolls, tracers.
- AI navigation 9b14f4b (engine/ai): NavGrid (uniform u8 occupancy, host-filled, no physics/render dep) + NavQuery deterministic A* (octile heuristic, indexed binary min-heap w/ decrease-key, tie-break on cell index, generation-stamped O(1) scratch reset) + greedy string-pull LOS-skip smoothing + per-agent repath throttle + deterministic separation from position snapshots. Parallel-safe via 64-slot per-worker NavQuery pool (16-core machine -> slots 1..16, no collision). Opt-in via AiContext::nav_grid (null = unchanged steer-v1). 7 new [ai][nav] tests. Next: polygon navmesh + true funnel, dynamic obstacle carving, jump/drop links.

## Wave 6 (in flight)
- Editor no-code gameplay authoring (editor/web/scene/PlayRuntime) — Health/Weapon/Faction/Hitbox/AI components in the inspector + SceneFile serialization + ColliderShape::Plane + authored-into-Play. [IN FLIGHT]
- Vehicle-on-terrain physics + governor #58 (engine/physics). [LANDED 452bb3a — set_ground_heightfield per-wheel borrowed-callback sampler + governor/steering-authority; 343-line test; physics 89 x3, goldens 4/4, RELEASE green. #58 vehicle/governor portion CLOSED. Wiring into demos/editor is a Wave-7 follow-up.]
- Render perf kernelization, bench-gated (engine/render) — raster fragment + BVH8 traverse, goldens bit-identical. [IN FLIGHT]

## Engine gaps surfaced by demos (track)
- Box-box / capsule / GJK speculative-contact coverage (anti-tunneling only covers sphere/box/capsule-vs-plane so far; needs GJK-distance).
- Vehicle-on-terrain physics API now EXISTS (452bb3a) but is NOT yet wired into df_demo/racer_demo/editor — Wave 7 wires set_ground_heightfield + VehicleDesc governor fields at the call sites.

## Wave 5+ (planned)
First demo GAME (indoor BSP shooter: combat + lights + hybrid shadows + AI + a level) under games/ — orchestrator wires it (root CMake + host). Then NFS racer demo, Delta Force terrain demo. Plus: wire PsyGraph tick + AI + combat into the editor play runtime; deferred render fidelity (soft penumbra, terrain-march shadows); anti-tunneling #63; editor graph-panel for PsyGraph.

## Done (recent → older)
- Editor render-settings panel (mode/sun/ambient/shadow/RT quality) + scene-level RenderSettings serialized to .psyscene.
- Raster path consumes scene lights + sun (root-cause fix: lights now actually shade in raster; goldens unchanged via opt-in gate).
- Material albedo plumbed into raster fragment shader (colors show); mat_color/mat_tex console + Inspector material editor.
- Add/Remove components in editor (no-code) over main-thread IPC, with undo.
- Physics: gen-safe handles, instance-owned World (ADR-010), rotated inertia, collider-scale fix, parenting writeback + graph-sync, zero-garbage hot paths, joint solver (kernel), angular API, vehicle + helicopter play, char controller.
- Editor: gizmo modes (Tab/G/R/Y), play mode, RT viewport toggle + lightmap bake, BSP + terrain level loaders, two independent code-review backlogs fully closed.

## In flight (this wave)
- (set by the loop)

## Next milestones (see MISSION roadmap)
M-HYB hybrid shadows → M-COMBAT → M-AI → M-PSYGRAPH → M-NET → demo games (Duke/Quake, Delta Force, NFS) → editor migration.

## Open risks / deferred
- Phase B hybrid compositing (raster+traced shadows) — biggest renderer item.
- Anti-tunneling (plane primitive + speculative contacts) — #63.
- Joints-in-play wiring on the promoted scene components; vehicle-on-terrain raycast; wheel visuals.
- Web Vite major upgrade (2 dev-only advisories).
- Pre-existing `-Wunused-function: active_arcade_rt_mode` warning (harmless).
