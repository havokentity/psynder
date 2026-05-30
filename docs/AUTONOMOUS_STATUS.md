<!-- SPDX-License-Identifier: MIT -->
# Psynder — Autonomous Build Status (live log)

Read `docs/AUTONOMOUS_MISSION.md` for the operating loop. This file is the
running ledger: update it every wake-up. Newest entries on top.

## >>> RESUME HERE (fresh session after restart, 2026-05-30) <<<
HEAD = `7ad28f6` on `integration/wave-hybrid-material-shadows`, pushed + in sync, working tree CLEAN. Gated checks GREEN at HEAD: release `psynder_unit` 0-failed (914 cases), goldens 4/4, debug+release builds 0 error, full web build green. Waves 5 + 6 COMPLETE. Wave 7: AOI netcode (f63d848) + PsyGraph no-code scripting (5b262e3) LANDED. 

TWO open items for the next session, in priority order:
1. **[HIGH BUG] Demo teardown SIGABRT (exit 134).** `psynder_df_demo` (and likely racer/shooter — any demo that builds a shadow TLAS) completes its gameplay then ABORTS during process teardown. NOT caught by the unit suite/goldens (they exit 0). Did NOT exist in Wave 5 (df_demo smoke exited 0 then); a Wave-6/7 ENGINE change regressed it. PRIME SUSPECT: the RT telemetry fix `76fd68c` made `Bvh8`/`Tlas` non-copyable + added dtors that erase from a `this`-keyed `StateRegistry` (engine/render/rt/Bvh.cpp) under a mutex — at process exit the registry (function-local static / global) can be destroyed BEFORE a late-destroyed Tlas, so the dtor touches torn-down state -> abort (static-destruction-order fiasco). FIX: make the StateRegistry a leak-on-exit Meyers singleton (heap, never destroyed) OR guard the dtor erase against a destroyed registry (alive flag). VERIFY: df_demo `--smoke-frames=90` exits 0 (no 134), unit suite still 0-failed, goldens 4/4. CHECK the arcade too (run + quit) since the user runs release arcade. Repro: build games, run any demo smoke, observe exit 134 after "smoke target reached".
2. **[DEFERRED] Wave 7 vehicle-on-terrain demo wiring (#80).** WIP preserved on branch `wip/vehicle-on-terrain` (@ `7e4a0e5`, games/ only). Status: df_demo drivable jeep terrain-follow LOGIC works ("TRACKS-TERRAIN GROUNDED ... PASS", 2.29m chassis-Y span over slope) BUT the demo aborts on exit (same bug #1) AND the racer governor is over-tuned (UNDER-CAP STALLED: peak 10.4 m/s vs 12 cap, 32.7m, 0 laps). Needs: fix #1 first, then a fresh lane to retune the racer governor (it swung from runaway to stalled) and re-verify both smokes exit 0. The Wave-6 ENGINE physics for this (set_ground_heightfield + governor, #58) is DONE + shipped (452bb3a); only the demo wiring is pending.

Then resume Wave 8 (from the heartbeat plan): ECS query kernelization (engine/scene, bench-gated), physics solver SIMD (engine/physics, bench-gated, goldens bit-identical), editor vehicle-on-terrain authoring. One games/ lane at a time. Prune agent worktrees after each wave (they re-accumulate ~1GB each; `git worktree remove -f -f`).

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

## Wave 7 (in flight)
- PsyGraph editor graph-panel (web/editor/scene/script/PlayRuntime). [LANDED 5b262e3 (resumed from WIP 878c2be) — ScriptGraphComponent + Scene graph side-table + SceneFile v5 SSCG/SCGB chunks (backward-compat) + PlayRuntime compile+alloc-free-VM-run authored graphs + main-thread IPC (psygraph_set/clear/list) + PsyGraph.tsx node editor (6 node categories) + byte-identical browser serializer. Debug 913 x3, [nocode] 3/3, goldens 4/4, RELEASE unit 0-failed (914), full web build green. Designer authors+runs node-graph logic in-editor. Next: richer palette, subgraphs, live VM debug viz.]
- AOI per-peer netcode + handshake/lobby + jitter buffer (engine/net). [LANDED f63d848 — AOI-gated per-peer snapshots (interest sphere + byte budget + priority + leave-AOI despawn, reconstructed-world history fix), SYN/FIN Lobby slot table, server InputJitterBuffer; new tagged handshake/ack wire; debug 910 x3, RELEASE unit 0-failed (911), goldens 4/4. Next: dedicated server + matchmaking + AEAD/anti-cheat.]
- Vehicle-on-terrain demo wiring — df_demo drivable terrain jeep + racer governor (games/df_demo + games/racer_demo). [RESUMING — orig lane a150fac died at token-out (df_demo jeep ~360 lines done, racer not started, never built); WIP preserved at ac02d3d; finisher lane ad45723d87d00f020 finishing+verifying.]

## NOTE — token-out recovery (2026-05-30 ~07:5x)
Session ran out of tokens during the Wave-7 idle wait; the two in-flight worktree agents (PsyGraph, vehicle) DIED mid-flight with uncommitted partial work. On resume: committed each worktree's WIP to its branch (878c2be / ac02d3d) so nothing was lost, removed nothing (dead worktrees stayed locked but commits are in the shared object store), and dispatched fresh FINISHER agents that reset --hard to the WIP and complete+verify+commit. Also: ~40 stale locked agent worktrees accumulated across waves under .claude/worktrees + .worktrees — harmless (disk only); batch-prune with `git worktree remove -f -f` when convenient. AOI netcode (f63d848) had already landed+pushed before the token-out, so it is safe on origin.

## IMPORTANT — release-suite gap + uncommitted WIP found (2026-05-30)
- RELEASE `psynder_unit` has 1 PRE-EXISTING failure: render_rt_frame_helpers.cpp:262 (TlasBuilds counter), root cause = StateRegistry<T> keyed by `this` in engine/render/rt/Bvh.cpp leaks stale counters on address reuse; order-dependent, release-only, passes in isolation. Does NOT affect the arcade binary. Being fixed by lane a5238ec. GOING FORWARD: the release gate must also run the RELEASE psynder_unit suite, not just build_release.sh (which only builds the arcade).
- RESOLVED: prior-session editor-authoring WIP that was uncommitted in the MAIN worktree (GameplayComponents.h untracked + dirty editor/scene/player files) was stashed, then the clean committed lane version (3c760d2) superseded it; both WIP stashes were verified-redundant and DROPPED. Tree is clean. No pushed commit was ever contaminated.

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
