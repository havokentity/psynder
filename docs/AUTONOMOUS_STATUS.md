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

## Wave 2 (next, after M-PSYGRAPH integrates)
M-AI (enemy perception/nav/state machine — uses combat + needs World::raycast #69), M-NET (UDP snapshot+delta replication), first demo game (indoor BSP shooter exercising combat+lights+hybrid shadows). Also fold in #69 raycast (unblocks AI LOS + combat cover) and deferred fidelity/anti-tunneling (#63) as capacity allows.

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
