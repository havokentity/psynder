# Changelog

## v0.2.0-wave-b-partial (2026-05-19) — Wave B partial (3 lanes), M2 demo running

Three of 25 Wave B lanes shipped before the deadline. The other 22
hit a rate-limit reset window mid-spawn and were not re-attempted.
Everything that landed is solid; the remaining 22 Wave B lanes are
their original GitHub Issues (#50-74, minus the three closed by #76, #77, #78).

### Wave B / Lane 07 / render-raster
- **Surface cache auto-engage** per ADR-001 — 4 MiB LRU slab keyed by
  (surface_id, lightmap_version, mip_level), 4-frame hysteresis,
  one-byte `ShadingPath` dispatch tag on `DrawCmd`. `r_force_shading_path`
  cvar for A/B profiling.
- **HiZ buffer at 8×8 per tile** — conservative max-z per cell,
  rebuilt at tile-pass start, early-rejects pixels before the inner loop.
- **Trilinear sampling** — `Texture::mip_count` + mip chain, per-draw
  mip LOD from UV finite-diffs, lerps two bilinear samples; falls back
  to bilinear when only one mip exists.
- 19 new Catch2 cases; full `[raster]` suite green (33 cases).

### Wave B / Lane 18 / editor-core
- **Physgun fully wired** — cursor-ray AABB pick (slab method), drag
  through 3D at adjustable `grab_distance`, quaternion rotate, uniform
  scale, freeze toggle, weld via `constraints::Weld` onto the editor's
  constraint graph.
- **Constraints viz** — per-kind 2D screen-space line glyphs for all
  six constraint kinds via `ui::imm::line`.
- **.psylevel + .psyc save/load round-trip** — encode + decode tests
  covering brushes, entities, bodies, constraints, heightfield, splat
  weights, including truncated-blob regressions and empty-scene
  handling.

### Wave B / Lane 25 / samples + tests
- **Sample 02 / M2 demo** — drops the readiness probe; now renders
  four spinning unit cubes with per-face vertex colors. Exercises the
  full vertex transform → triangle setup → tile bin → perspective-
  correct interpolation → Z reject path.
- **Golden cells** — `psynder_add_golden_cell` registered for
  `clear`, `triangle`, `crate_room`. Darwin PNG baselines committed
  (byte-identical across reruns thanks to frame-indexed deterministic
  clocks).
- **Bench-gate runner** — `raster_pipeline_tile32/64/128` driving the
  full pipeline at each ADR-002 specialization via `r_tile_size`
  swap. Each variant emits its own JSON entry so the >2% gate fires
  per-tile-size.
- **PNG writer + capture flag** — header-only stored-deflate PNG
  encoder (real PNG signature/CRC32/Adler32), `--smoke-capture-out PATH`
  on samples 00/01/02.

### Cross-cutting fixes
- `tests/asset_vfs.cpp` — spin-wait for the async-VFS callback after
  Lane 04's Chase-Lev pool made `read_async` actually async.
- `tests/unit/CMakeLists.txt` — `RESOURCE_LOCK psynder_ipc_port`
  attached post-discovery to all `ipc:` tests.
- `cmake/Goldens.cmake` — semicolon-in-`if()` parse error fixed.
- `cmake/missing_golden.cmake` — driver for absent-baseline cells.

### Test + demo status

- **404/404 Catch2 cases passing** on macOS Apple Silicon (mac-release
  preset).
- **M0 demo** (`sample_00_clear`) — running, animated clear color in
  AppKit window via CAMetalLayer scanout.
- **M1 demo** (`sample_01_triangle`) — running, rotating textured
  triangle through the rasterizer.
- **M2 demo** (`sample_02_textured_quad`) — running, four spinning
  vertex-colored cubes through the full rasterizer pipeline.

### Outstanding (Wave B remaining + future waves)

- 22 of 25 Wave B lanes have not been re-spawned. Their original
  Issues (#50, 51, 52, 53, 54, 55, 57, 58, 59, 60, 61, 62, 63, 64,
  65, 66, 68, 69, 70, 71, 72, 73) remain open with the lane briefs
  intact.
- Wave C (M4 + M5 — vehicles + raytraced headlights), Wave D
  (M6 + M7 — tactical FPS + RmlUi production HUDs + networking),
  and Wave E (M8 polish + 1.0) per `docs/waves-roadmap.md` are
  scoped but not started.

## v0.1.0-wave-a (2026-05-19) — Wave A complete

The "thousand-finger first push." Twenty-five parallel ownership lanes
landed real C++ implementations behind the frozen public API headers in
roughly an afternoon. M0 demo verified on macOS; ~350 Catch2 unit tests
green; the entire repo scaffold (build system, CI, ADRs, design doc)
shipped.

### Highlights

- **All 25 subsystem lanes integrated** (engine/core, math, simd, jobs,
  asset, scene/ECS, render-raster, render-rt, render-post, world-bsp,
  world-outdoor, audio, physics, net, script, ui-imm, ui-rml,
  editor-core, editor-ipc, editor-web, platform-{macos,win32,linux},
  tools, samples-tests).
- **M0 demo** (`sample_00_clear`) opens a real 1280×720 window on
  Apple Silicon via `AppKit` + `CAMetalLayer` scanout and animates a
  sin-driven clear color. No engine code touches the GPU — Metal is
  used only as a passthrough quad blit per ADR-007/§11.3.
- **M1 demo** (`sample_01_triangle`) renders a rotating textured
  triangle through the tiled software rasterizer (Q24.8 edges,
  perspective-correct attrs, bilinear sampling, runtime-switchable
  32/64/128 tile specializations per ADR-002).
- **Full dmonte console port** in lane 01: cvar archive/load, undo/redo,
  favourites with f1..fN magic, history persisted across sessions,
  smart-resolve unique-prefix match, bracket-batch transactions,
  per-allowed-value platform tags, `requires_predicate` cross-cvar
  dependency warnings.
- **Chase-Lev work-stealing job system** in lane 04 — single-owner
  deque per worker, ABA-resistant pool free list, 100,077 stress
  assertions clean.
- **Software BVH8 + AVX2 packet traversal** in lane 08 — no Embree
  dependency in the engine runtime per ADR-007. SAH builder, refit with
  1.3× rebuild trigger, à-trous denoiser, scalar fallback on NEON
  (real wide-NEON kernel in Wave B).
- **`psynder_phys`** in lane 13 — fixed 120 Hz tick with accumulator,
  parallel SAP broadphase across 3 axes, GJK+EPA narrowphase with
  closed-form sphere/capsule/box specializations, union-find island
  detection, projected Gauss-Seidel solver, vehicle + character
  controller scaffolded, deterministic FP via `-fno-fast-math` +
  per-step `FpGuard`.
- **Editor IPC** in lane 19 — hand-rolled WebSocket + HTTP server (no
  third-party WS library), session-token auth with constant-time
  compare, msgpack codec, RFC 6455 handshake, `protocol.psy` IDL with
  Python-generated C++ + TypeScript stubs.
- **React + TypeScript editor panels** in lane 20 — Vite-built
  Inspector / Console / Profiler with msgpack-over-WS client, mock-mode
  for offline dev, schema → form-widget binding for
  `PSYNDER_COMPONENT(...)` declarations.
- **Lua 5.4 binding** in lane 15 — sandboxed stdlib, DOTS-shaped API
  forcing `world:register_system({reads=, writes=}, fn)` style (no
  `entity:tick` exposed anywhere).
- **`lm_pak` + `lm_cook` + `lm_qbsp` + `lm_bake` + `lm_mapimport`**
  tools in lane 24 — full offline asset pipeline driver.
- **Documentation:** seven decided ADRs filed long-form under
  `docs/adr/`; Wave A/B/C/D/E roadmap under `docs/waves-roadmap.md`;
  parallel-agent coordination conventions in `AGENTS.md`.

### Tests + benches

- 353/353 Catch2 cases passing on macOS Apple Silicon (mac-release
  preset). Lint passes clang-format + clang-tidy CI lane.
- Golden-image harness in place (`tests/golden/runner.cpp` + 0.5%
  per-pixel tolerance imgdiff), with starter reference PNGs.
- Bench harness in place — per-tile raster + per-island physics
  microbenches emit JSON, CI gate enforces >2% regression must justify.
- One known flake: `ipc: live server accepts good WebSocket token` —
  occasional port-acquisition race under parallel ctest. Single retry
  passes. Wave B follow-up.

### Process

- Twenty-five GitHub Issues filed with strict file-ownership specs
  (`docs/wave-a-bar.md`); 25 parallel background agents in
  `/tmp/psynder-wts/lane-NN-*` worktrees each delivered against their
  owned directory; squash-merged into `main` with no
  `Co-Authored-By: Claude` trailers per the repo's contributor-list
  preference. Two PRs (08-render-rt + 14-net) hit `tests/unit/CMakeLists.txt`
  merge conflicts; resolved by cherry-pick + manual integration.

### Cross-lane API additions (additive, public headers stable)

- `engine/scene/World.h` added `query<reads<>, writes<>>()`,
  `set_structural_deferred()`, `apply_structural_changes()` —
  additive only.
- `cmake/Goldens.cmake` adapter from dmonte's `pt_add_golden_cell`.
- `cmake/missing_golden.cmake` driver for absent-baseline cells.

### Next: Wave B (M2 + M3)

See [docs/waves-roadmap.md](docs/waves-roadmap.md) for the lane-by-lane
Wave B focus. M2 = tiled raster polish + crate-room demo. M3 = BSP +
lightmaps + first React editor session + brush CSG v0.
