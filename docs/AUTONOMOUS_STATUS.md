<!-- SPDX-License-Identifier: MIT -->
# Psynder — Autonomous Build Status (live log)

Read `docs/AUTONOMOUS_MISSION.md` for the operating loop. This file is the
running ledger: update it every wake-up. Newest entries on top.

## >>> RESUME HERE (2026-05-30) <<<
HEAD = `a220fa8` on `integration/wave-hybrid-material-shadows`, pushed + in sync, working tree CLEAN (only untracked `.claude/`). Gated checks GREEN at HEAD: debug `psynder_unit` 0-failed (926 cases) x3 deterministic, release 0-failed (927), goldens 4/4, debug+release builds 0 error, arcade relinks. Waves 5 + 6 + 7 COMPLETE.

## Wave 8 (in progress) — 2 of 3 lanes LANDED
Dispatched 3 file-disjoint lanes as parallel worktree agents. WORKTREE-BASE HAZARD discovered: the agent worktree-isolation harness based 2 of 3 lanes on origin/main (38e31bc, 248 commits STALE) instead of integration HEAD. Lane A lucked into the correct base; B + C were stale. Recovery: re-dispatch with a mandatory `git fetch origin && git reset --hard origin/integration/wave-hybrid-material-shadows` base-fix preamble (now reliable). Agents also hit the shared session-token limit mid-flight; their WIP was preserved to branches and FINISHED on integration. Net:
- **[LANDED d81e6a2] Lane A — no-code drivable-vehicle-on-terrain editor authoring.** scene::VehicleComponent proxy (PhysicsComponents.h) gains a speed governor + steer-authority trio + ground-binding mode (Plane|Heightfield) with a compact self-contained procedural rolling-hills surface (pure alloc-free vehicle_terrain_height helper, round-trips with the scene, no terrain subsystem). PlayRuntime::begin synthesizes a physics::vehicle from the proxy + binds set_ground_heightfield (heightfield-pool reserved to worst case BEFORE the loop so the borrowed sampler addresses never realloc) / set_ground_plane. New backward-compat v6 SVHX scene chunk (count 19->20; old files load Plane defaults). WebPanels.cpp Inspector schema + CBOR + hash back the pre-existing data-driven "Add Vehicle" button. New tests/unit/editor_vehicle_authoring.cpp (author->serialize->deserialize->Play synth->throttle moves chassis). Debug 918 x3 + release 919 + web bundle rebuilds clean (tsc+vite, deterministic hash) + goldens 4/4.
- **[LANDED a220fa8] Lane C — GJK-distance box-box/capsule-box speculative anti-tunnelling.** From-scratch gjk_distance (GJK distance sub-algorithm, reuses existing GjkSupport; deterministic, alloc-free, 32-iter bound) wired into kernel_pair_separation; a fast box no longer tunnels another box. ADR-011. CAUGHT + FIXED A REGRESSION the lane agent's WIP shipped: GJK's witness-point normal degenerates for nearly-parallel faces, so the speculative speed-limit let a SETTLING box creep through its support (parented box sank to y=-83; the runaway body's huge AABB degraded the broadphase to a NEAR-HANG that stalled the unit suite). Fix = a 10 cm near-contact band: small gaps go to the proven EPA overlap path (stable face normal), only larger gaps (where tunnelling pairs are caught, closing*dt >> band) use the GJK speculative path. New tests/unit/physics_gjk_speculative.cpp (distance correctness, gating, end-to-end no-tunnel, resting no-regression). Debug 926 x3 (no hang) + release 927 + goldens 4/4.
- **[DEFERRED] Lane B — physics solver SIMD kernelization.** ASSESSED + deferred (NOT a regression — nothing landed). kernel_solve_island is a SEQUENTIAL-IMPULSE (Gauss-Seidel) solver: each contact's impulse reads velocities the previous contact just wrote. Bit-identical SIMD is infeasible — any cross-constraint batching/reordering changes float accumulation order and breaks the determinism + goldens HARD gate (Jacobi != Gauss-Seidel numerically). A genuine win needs a SOLVER REDESIGN (block/Jacobi solver or graph-colored parallel batches) that DELIBERATELY re-baselines goldens/determinism = its own ADR-level lane, not a drop-in perf pass. Per the mission's own "do not hand-SIMD / kernelization is a perf pass not a landing-blocker" rule, not forced. The SIMD-friendly bit-identical targets that DO exist (integrate_forces/positions, AABB rebuild — embarrassingly parallel, no reassociation) are lower-value (memory-bound, not the bottleneck) and remain available if a perf pass is wanted.

Both PRIOR-session open items were DONE earlier this session (Wave 7):
1. **[FIXED 725f75c] Demo teardown SIGABRT (exit 134).** Root cause exactly as predicted: the `this`-keyed `StateRegistry` (engine/render/rt/Bvh.cpp) was a function-local static holding a `std::mutex`; a `Bvh8`/`Tlas` destroyed during teardown AFTER the registry static -> erase locks a destroyed mutex -> libc++ throws `std::system_error` ("mutex lock failed: Invalid argument") from a noexcept dtor -> terminate -> SIGABRT. FIX = leak-on-exit heap singletons (registry never destroyed; only static is a raw ptr -> no atexit hook). REGRESSION = namespace-scope guard in render_rt_frame_helpers.cpp builds+tears down a BLAS+TLAS from its dtor (runs at exit after the registry); old code -> unit binary passes all cases but exits 134, fixed -> 0. PROVEN both ways.
2. **[DONE 064f8f4 + bda47cb] Vehicle-on-terrain demo wiring (#80).** While wiring it, the racer surfaced a real ENGINE bug the WIP had only worked around: the Pacejka **lateral tire force had the wrong sign** (applied along +tire_right, same sense as the slip -> positive feedback -> any steer spun the chassis; repro'd in samples/04 too). Fixed in engine/physics/Vehicle.cpp (lateral force is now restoring: `-tf.Fy`; friction-circle magnitude unchanged) with a new restoring regression in physics_vehicle_terrain.cpp (inject a pure sideways slide, assert it DECAYS; fails on the old sign). With steering unblocked, racer_demo now drives the closed-loop `auto_drive` (chases the spline look-ahead, steers against chassis heading) and **actually corners + LAPS the oval** (lap 1 @ 37.48s ~2250 frames; 1400-1500f smoke -> UNDER-CAP PROGRESSED CORNERED + auto-drive PASS, deterministic debug==release). Retuned throttle_kp 0.15->0.5 (engine governor enforces the hard cap). df_demo drivable jeep terrain-follow PASS, DBG logging removed. All demos exit 0 debug+release. The `wip/vehicle-on-terrain` branch is now SUPERSEDED (safe to delete).

NEXT (Wave 8 wrap + Wave 9 candidates):
- ECS query kernelization (engine/scene): land a bench FIRST (none exists yet), then kernelize — the only named Wave-8 lane not yet attempted. Bit-identical-friendly (iteration/dispatch, not float reassociation).
- If a physics perf pass is wanted: vectorize integrate_forces/positions + AABB rebuild (embarrassingly parallel, bit-identical) — NOT the sequential solver (see Lane B above; that needs a redesign ADR first).
- Capsule-capsule / capsule-box EPA contact-manifold polish; capsule-box was covered for speculative distance but verify resting.
- Reference-game depth: Duke3D/Quake BSP-PVS indoor demo; multi-objective DF flow; net MP for a demo.
- HOUSEKEEPING (do soon): prune stale agent worktree branches (worktree-agent-* at 38e31bc/35c1335 + a779c72/a0f55b598 + the killed Lane B) and any leftover worktree dirs; AGENT-WORKTREE-BASE GOTCHA: always have spawned agents `git reset --hard origin/integration/...` as step 0 (the harness bases worktrees on origin/main = 248 commits stale).

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

## Wave 6 — COMPLETE (HEAD 76fd68c): editor no-code authoring 3c760d2, vehicle-on-terrain 452bb3a, render-perf 199f0b4, RT telemetry fix 76fd68c. RELEASE psynder_unit now 0-FAILED (898 cases) for the first time; debug 897 x3; goldens 4/4 bit-identical; FULL release build (with web) green; all 3 demos build.
- Editor no-code gameplay authoring 3c760d2 (editor/web/scene/PlayRuntime): GameplayComponents.h POD proxies (Faction/Hitbox/WeaponMode/Ai/Perception/Patrol) + ColliderShape::Plane; SGAI scene chunk v3->v4 backward-compat (+ fixed latent chunk-count save bug 16->17); Inspector "Gameplay & AI" add/remove via main-thread IPC; PlayRuntime synthesizes live components from proxies on begin(), strips on end(). A designer can author an armed-AI shooter with a plane floor entirely in-editor. (Superseded prior-session WIP stashes — dropped.)
- Vehicle-on-terrain physics + governor #58 452bb3a (engine/physics): set_ground_heightfield per-wheel borrowed-callback sampler + governor/steering-authority. #58 vehicle/governor CLOSED. Demo/editor call-site wiring = Wave 7.
- Render perf 199f0b4 (engine/render): raster fragment hoist + RT closest-hit normal-deferral; raster_unlit -17.8%/lit -5.0%/rt_bvh -3.4%; bit-identical; new psynder_bench_render_hot.
- RT telemetry state-leak fix 76fd68c (engine/render/rt): erase address-keyed Bvh8/Tlas state on destruct (now non-copyable/non-movable) + generation-guarded eviction; fixed the pre-existing release-only order-dependent failure. Regression test forces same-address reuse. Bonus: fixed a WorkerTileScheduling timing flake.

## Release gate hardened
The release gate now ALSO runs the RELEASE psynder_unit suite to 0-failed (not just build_release.sh which only links the arcade). As of 76fd68c it is green.

## Wave 7 — COMPLETE (HEAD 064f8f4): PsyGraph 5b262e3, AOI netcode f63d848, teardown SIGABRT fix 725f75c, Pacejka lateral-sign fix bda47cb, vehicle-on-terrain demo wiring 064f8f4. Debug unit 0-failed (914), release 0-failed (915), goldens 4/4, all 3 demos exit 0 debug+release.
- PsyGraph editor graph-panel (web/editor/scene/script/PlayRuntime). [LANDED 5b262e3 (resumed from WIP 878c2be) — ScriptGraphComponent + Scene graph side-table + SceneFile v5 SSCG/SCGB chunks (backward-compat) + PlayRuntime compile+alloc-free-VM-run authored graphs + main-thread IPC (psygraph_set/clear/list) + PsyGraph.tsx node editor (6 node categories) + byte-identical browser serializer. Designer authors+runs node-graph logic in-editor. Next: richer palette, subgraphs, live VM debug viz.]
- AOI per-peer netcode + handshake/lobby + jitter buffer (engine/net). [LANDED f63d848 — AOI-gated per-peer snapshots (interest sphere + byte budget + priority + leave-AOI despawn, reconstructed-world history fix), SYN/FIN Lobby slot table, server InputJitterBuffer; new tagged handshake/ack wire. Next: dedicated server + matchmaking + AEAD/anti-cheat.]
- Demo teardown SIGABRT fix [LANDED 725f75c — leak-on-exit RT StateRegistry singletons; static-destruction-order fiasco fixed; unit-teardown regression guard. See item 1 above.]
- Pacejka lateral tire-force SIGN fix [LANDED bda47cb (engine/physics) — lateral cornering force is now restoring (-tf.Fy) instead of positive-feedback; restoring regression in physics_vehicle_terrain.cpp. Unblocks ALL vehicle cornering (racer, df jeep, samples/04). See item 2 above.]
- Vehicle-on-terrain demo wiring #80 [LANDED 064f8f4 (games/) — racer_demo closed-loop auto_drive corners + LAPS the oval (CORNERED PASS, lap 1 @ 37.48s); df_demo drivable jeep terrain-follow PASS; DBG logging removed; throttle retuned. `wip/vehicle-on-terrain` SUPERSEDED.]

## NOTE — token-out recovery (2026-05-30 ~07:5x)
Session ran out of tokens during the Wave-7 idle wait; the two in-flight worktree agents (PsyGraph, vehicle) DIED mid-flight with uncommitted partial work. On resume: committed each worktree's WIP to its branch (878c2be / ac02d3d) so nothing was lost, removed nothing (dead worktrees stayed locked but commits are in the shared object store), and dispatched fresh FINISHER agents that reset --hard to the WIP and complete+verify+commit. Also: ~40 stale locked agent worktrees accumulated across waves under .claude/worktrees + .worktrees — harmless (disk only); batch-prune with `git worktree remove -f -f` when convenient. AOI netcode (f63d848) had already landed+pushed before the token-out, so it is safe on origin.

## IMPORTANT — release-suite gap + uncommitted WIP found (2026-05-30)
- RELEASE `psynder_unit` has 1 PRE-EXISTING failure: render_rt_frame_helpers.cpp:262 (TlasBuilds counter), root cause = StateRegistry<T> keyed by `this` in engine/render/rt/Bvh.cpp leaks stale counters on address reuse; order-dependent, release-only, passes in isolation. Does NOT affect the arcade binary. Being fixed by lane a5238ec. GOING FORWARD: the release gate must also run the RELEASE psynder_unit suite, not just build_release.sh (which only builds the arcade).
- RESOLVED: prior-session editor-authoring WIP that was uncommitted in the MAIN worktree (GameplayComponents.h untracked + dirty editor/scene/player files) was stashed, then the clean committed lane version (3c760d2) superseded it; both WIP stashes were verified-redundant and DROPPED. Tree is clean. No pushed commit was ever contaminated.

## Engine gaps surfaced by demos (track)
- Box-box / capsule / GJK speculative-contact coverage (anti-tunneling only covers sphere/box/capsule-vs-plane so far; needs GJK-distance).
- [CLOSED bda47cb] Pacejka lateral tire force had the WRONG SIGN (positive feedback) -> any steer spun the chassis; both demos had worked around it by driving straight. Fixed: lateral force is restoring. Cornering now works engine-wide.
- [CLOSED 064f8f4] Vehicle-on-terrain physics API (452bb3a) is now WIRED into df_demo (drivable jeep, set_ground_heightfield) + racer_demo (governor + closed-loop auto_drive). Editor authoring of it is the remaining piece (Wave 8).
- Steady-state cruise sits ~10.8 m/s under the 12 m/s cap because the engine governor's torque-taper band (0.85*cap..cap) + drag balance there; the DRIVER throttle barely moves it. Fine for a believable cruise; if a tighter hold-at-cap is wanted later, narrow the governor taper band rather than the driver gain.

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
