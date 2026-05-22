# Wave roadmap (Wave A → E, target v1.0)

The Psynder engine is being built via concurrent 25-lane waves. Each wave
re-uses the same 25-lane carve-up (see [AGENTS.md](../AGENTS.md)). This
doc tracks **what each wave covers** so contributors know what's coming
next without having to re-derive scope from DESIGN.md.

> **Caveat:** the yolo timeline (push toward v1.0 in a few days) means
> each wave covers *more* surface than is realistic to ship perfectly.
> Bugs are expected; final-week polish is a separate workstream.

## Wave A — M0 + M1 + every subsystem scaffolded real (in-flight 2026-05-19)

Lane bar: each lane lands real `.cpp` files replacing Phase-0 stubs;
public headers stay FROZEN. M0 demo (sample 00 — animated clear color)
must run end-to-end on Mac after lane 23 lands. Hot subsystems (raster,
ECS, jobs, allocators, console) have working implementations.

- See `gh issue list --state all --label "" --limit 30` — issues #1-25.
- See [docs/wave-a-bar.md](wave-a-bar.md) for the shared deliverable
  rubric every lane is graded by.

## Wave B — M2 + M3 (tiled raster + BSP + lightmaps + editor v0)

Lane focus per DESIGN.md §13 M2-M3:

| Lane | Wave B push |
|---|---|
| 01-core | Tracy zones throughout, allocator heatmap, flight recorder |
| 02-math | Per-frame big-coord re-centering driver |
| 03-simd | AVX-512 packet kernels, prefetch + streaming-store helpers |
| 04-jobs | Fibers impl, P/E heterogeneous pools |
| 05-asset | zstd wired, cooker formats `.lmm/.lmt/.lma` round-trip |
| 06-scene | Spatial indices: BVH refit, SAP, hashed grid, query router |
| 07-render-raster | All three tile specializations, surface cache, HiZ, trilinear + anisotropic |
| 08-render-rt | Real 8-wide packet intersect, refit, denoiser à-trous |
| 09-render-post | Bloom (separable), motion blur, scanline filter |
| 10-world-bsp | Portal culling, lightmap atlas streaming |
| 11-world-outdoor | Heightmap raymarcher SIMD path, spline track editor |
| 12-audio | HRTF + FFT convolution reverb + FDN |
| 13-physics | GJK/EPA, island solver, Pacejka vehicle module, character controller |
| 14-net | Sliding window + selective ACKs, lockstep + snapshot |
| 15-script | Component / system registration macros, REPL host wiring |
| 16-ui-imm | Gizmos, brush previews, allocator heatmap |
| 17-ui-rml | Asset-VFS hot reload of `.rml`/`.rcss`, Lua bindings |
| 18-editor-core | Brush CSG, heightmap sculpt, physgun, constraints, save/load |
| 19-editor-ipc | IDL stub gen (C++/TS), schema-versioned protocol |
| 20-editor-web | Asset browser, profiler, REPL, prop spawn menu |
| 21-platform-win32 | WASAPI audio, force-feedback wheel |
| 22-platform-linux | PipeWire audio, ALSA fallback |
| 23-platform-macos | CoreAudio AUHAL |
| 24-tools | `lm_qbsp` + `lm_bake` (offline path tracer) + `lm_mapimport` |
| 25-samples-tests | Sample 02 + 03; golden images; bench-gate runner |

Demo target: **walk through a small Quake-style BSP room with lightmaps**
(sample 03). Editor v0 spawns / moves / deletes entities via the React
inspector panel.

## Wave C — M4 + M5 (vehicles + hybrid raytracing)

Lane focus per DESIGN.md §13 M4-M5:

- 07-render-raster: env cubemaps for car bodies, normal maps, specular
- 08-render-rt: real BVH8 build + 8-wide AVX2 intersect, denoiser
  shipping, tile-light list, headlight cookie textures
- 09-render-post: volumetric fog froxel grid (160×90×64)
- 11-world-outdoor: NFS-style racing tracks with banking, weather
- 13-physics: full Pacejka tire model, drivetrain (engine torque curve
  → clutch → gearbox → differential → wheels), aero drag + downforce,
  damage model
- 25-samples-tests: sample 04 (NFS track lap), sample 05 (RT shadow-packet
  colored-light scene)

Demo target: **colored dynamic lights casting raytraced packet shadows
across a small night scene** (sample 05).

## Wave D — M6 + M7 (tactical FPS + RmlUi + networking)

Lane focus per DESIGN.md §13 M6-M7:

- 11-world-outdoor: heightmap raymarcher real SIMD, scatter system,
  Rayleigh sky
- 13-physics: character controller polish (crouch/prone/ladder/water)
- 14-net: rUDP shipping, 16-32 player tactical FPS multiplayer, lockstep
  racing demo
- 17-ui-rml: shipping HUDs / menus, racing dashboard, mission briefings
- 18-editor-core: physgun + welds + axis/slider/ball-socket/rope/elastic
  constraints; multi-user co-op editing via lane 14
- 25-samples-tests: sample 06 (4 km × 4 km tactical map)

Demo target: **patrol a 4 km × 4 km tactical map at dawn, then sandbox-
mode weld a rocket to a watchtower and watch the physics fling debris**
(sample 06).

## Wave E — M8 polish + 1.0

- Bench gate enforcement across all three tile sizes
- Memory budget tracking + allocator heatmap shipping
- Determinism gate via golden-image cross-host comparison
- Documentation pass: `docs/01-getting-started.md`, `docs/02-rendering.md`,
  `docs/03-lighting.md`, `docs/04-world-formats.md`
- Contributor guide finalised
- `lm_bake` ships full path-traced multi-bounce
- Asset hot-reload battle-tested
- `tools/dmonte_pt/` retained as reference renderer (Embree opt-in)
- `git tag v1.0.0` + release notes

Demo target: the §18 sizzle reel — driver pulling out of a tunnel into
rain at night → indoor flashlight scene → dawn tactical map → editor
flip → three terminals (Ryzen 9 / Core Ultra / M3 Max).

## How waves get spawned

Each wave: orchestrator (project owner) files 25 GitHub issues with
strict file-ownership specs, then spawns 25 background agents (each in
its own `git worktree`) that:

1. `cd /tmp/psynder-wts/lane-NN-name`
2. Implement per the Issue, run `cmake --build --preset mac-release` to
   confirm green
3. `git push -u origin lane/NN-name` + `gh pr create`
4. Orchestrator merges (no `Co-Authored-By: AI tool` trailer per repo
   convention), runs final smoke

See [AGENTS.md](../AGENTS.md) for the lane → directory map. See
[scripts/file_wave_a_issues.sh](../scripts/file_wave_a_issues.sh) for
the issue-filer pattern used in Wave A; Wave B-E re-use the same shape.
