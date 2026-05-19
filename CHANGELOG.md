# Changelog

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
