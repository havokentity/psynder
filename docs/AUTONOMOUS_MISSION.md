<!-- SPDX-License-Identifier: MIT -->
# Psynder — Autonomous Build Mission (self-driving prompt)

This document IS the prompt. On every autonomous wake-up, re-read this file,
read `docs/AUTONOMOUS_STATUS.md` (the live progress log), pick the next
highest-value work, execute it, verify it, push it, update the status log, and
schedule the next wake-up. **Never ask the user anything.** When a decision is
needed, make the most defensible architectural choice, record it as an ADR in
`DESIGN.md` §16, and proceed.

## North star
A pure-CPU (no GPU, ever) C++23 software-rendering game engine with **hybrid
software raster + software raytracing**, custom DOTS/ECS, custom physics, a
Unity-style WYSIWYG editor, a compiling player runtime, **PsyGraph** visual
scripting, and **networking** — capable of recreating these reference games as
shippable demo projects:
- **Duke Nukem 3D** / **Quake** / **Quake II** — indoor BSP shooters (portals/PVS, hitscan + projectile weapons, enemy AI, pickups, doors/triggers).
- **Delta Force 1 & 2** — large outdoor heightmap-terrain tactical shooter, long sightlines, vehicles + helicopter, networked multiplayer.
- **NFS II SE** — spline-road racing, vehicle physics, chase cam, lap/timing.

## Non-negotiable principles (apply to every change)
1. Pure CPU. No GPU code anywhere in `engine/` runtime (ADR-007).
2. Cache-coherent DOTS/ECS: state in archetype/column storage, processed via `EcsRegistry::query<reads,writes>` (parallel per-chunk). Linkage lives in pooled ECS components, never `unordered_map` side-tables for per-frame data.
3. Zero per-frame heap garbage. Pre-size + pool. No transient alloc in update/render/sim hot loops (DESIGN §3.4). SBO/arena where needed; prove re-entrancy/thread-safety.
4. No-code-first authoring (editor) AND C++-capable. Every gameplay feature should be ECS components + systems an editor panel can expose; if editor wiring is unclear, build it as a standalone demo/game target first and migrate later.
5. Generation-safe handles; deterministic, -fno-fast-math-friendly sim.

## The autonomous work loop (each wake-up)
1. `git -C "<repo>" status`/`log` — confirm branch `integration/wave-hybrid-material-shadows`, clean tree, in sync with origin.
2. Read `docs/AUTONOMOUS_STATUS.md`; pick the next milestone/lane from the roadmap (or the highest-value unblock).
3. Decompose into **file-disjoint** lanes (so parallel agents never clobber). Spawn each as a background `Agent` in an isolated **worktree** with a precise brief: new files where possible; additive edits to shared files (`player/main.cpp`, CMake) only; the agent owns its CMake; verify in mac-debug (ASan+UBSan) with **verbatim `error:` grep**, run the full unit suite ×3 + goldens, and report.
4. As lanes land: cherry-pick onto integration, reconcile conflicts, **re-read critical/render/physics changes yourself**, then VERIFY:
   - `cmake --build build/mac-debug -j` → grep `error:` verbatim (0).
   - `touch player/main.cpp && cmake --build build/mac-debug --target psynder_arcade -j` → 0 errors, **binary mtime advances** (stale-binary trap).
   - Full `psynder_unit` exit 0, run ×3 (order-dependent flakes); zero sanitizer hits.
   - `ctest -R golden` → 100% (goldens must stay green; only regenerate a reference for a deliberate, documented visual change).
   - **RELEASE**: `PSYNDER_SKIP_WEB=<0 if web changed else 1> ./scripts/build_release.sh` → green (the user only tests release; release = no-sanitizer + LTO + -O2, where aliasing/restrict UB surfaces). Rebuild + verify the web bundle when editor-web changed (dist hashed assets are gitignored; confirm `index.html` asset refs match files on disk).
5. Commit (no `Co-Authored-By` trailer; never reorder includes; ASCII only) and `git push`.
6. Update `docs/AUTONOMOUS_STATUS.md`: what landed, what's next, open risks/ADRs.
7. `ScheduleWakeup` to continue (heartbeat ~1500-1800s; sooner cadence is unnecessary because background-agent completions auto-re-invoke). Keep going until the roadmap's Definition of Done is met, then start hardening/polish passes.

## Guardrails / lessons (do not relearn the hard way)
- Stale-binary trap: incremental builds have masked broken compiles + link breaks. Always grep `error:` verbatim and confirm binary mtime advanced. `psynder_unit` is a separate target from `psynder_arcade` — a green unit suite does NOT prove the editor binary links.
- ECS `query` runs bodies once-per-chunk ACROSS worker threads → any shared accumulation in a query body must be atomic/mutex'd.
- IPC handlers that mutate the ECS MUST run on the main thread via `inbound_`/`pump()` (never inline on the socket worker).
- Editor headers in `namespace psynder::editor` must qualify engine refs as `::psynder::render::X` (the `editor::render` lane shadows bare `render::`).
- Physics `World` is instance-owned (PIMPL, ADR-010); `World::Get()` is a back-compat default only.
- Mac smokes serialize on `/tmp/psynder_smoke.lockdir`; if the user's arcade holds it, skip the smoke — the green release LINK is what matters.
- Copilot review requests use `gh pr edit N --add-reviewer @copilot`.

## When blocked / ambiguous
- Editor-integration unclear → build a standalone demo/game target under `games/` (or `samples/` style) that exercises the feature via public engine APIs; migrate to the editor later. Demo targets are first-class drivers of engine work.
- Algorithmic unknowns (BSP/PVS build, portal clipping, Pacejka, rotor/flight, net delta-compression, lockstep vs client-server, navmesh/AI, visual-scripting VM) → use `WebSearch`/`WebFetch` for established techniques; pick one, cite it in an ADR, implement from scratch (no GPL deps).
- Never block on the user. Make the call, document it, move on.

## Roadmap (ordered; each milestone ships a demo/game target + tests)
- **M-HYB — Hybrid shadows (Phase B):** raster primary + per-pixel RT shadow rays (BVH for meshes; heightmap march for raymarched terrain), MAX-combined into the raster lighting; driven by RenderSettings render_mode=Hybrid. Demo: a lit room with dynamic shadows.
- **M-COMBAT — Gameplay core:** Health/Damage, Weapon (hitscan + projectile), Projectile sim, Pickup, Hitbox, Faction; ECS components + systems + editor exposure. Demo: shootable targets/dummies.
- **M-AI — Enemy AI:** perception, navigation (grid/navmesh), state machine (idle/patrol/chase/attack), pathing over BSP + terrain. Demo: enemies that hunt the player.
- **M-PSYGRAPH — Visual scripting:** node graph (events/conditions/actions) compiled to a pooled bytecode VM driving ECS; editor graph panel + serialization. Demo: door/trigger/spawn logic authored as a graph.
- **M-NET — Networking:** UDP transport, snapshot + delta + interpolation, client-server authoritative, entity replication of ECS components. Demo: two clients sharing a world.
- **M-DUKE — Duke3D/Quake demo game:** indoor BSP level + weapons + enemies + pickups + doors, first-person.
- **M-DF — Delta Force demo game:** terrain map + vehicle/heli + ranged combat + (net stretch).
- **M-NFS — NFS2SE demo game:** spline track + vehicle + chase cam + lap timing.
- **M-EDITOR — Migrate everything into the editor:** every above system authorable no-code (components in Inspector, PsyGraph for logic, play mode runs it).
- **Continuous:** anti-tunneling (plane primitive + speculative contacts), joints-in-play, vehicle-on-terrain raycast, wheel visuals, perf passes, web Vite upgrade, docs/ADRs.

## Definition of Done
The editor can author + play a small level for each genre (indoor shooter, terrain shooter, racer), hybrid raster+RT renders correctly, physics/joints/vehicles/characters simulate, PsyGraph drives gameplay logic, networking replicates a shared session, the player target compiles a standalone build, the full sanitized suite + goldens are green, and the **release** build is green. Then iterate on fidelity + the reference-game recreations.
