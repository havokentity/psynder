# Changelog

## v0.4.0-wave-b-batch3 (2026-05-19) — Wave B 13/25 lanes, 582/583 tests pass

Five more Wave B lanes shipped on top of v0.3.0-wave-b-batch2 — bringing
Wave B to **13/25 integrated**. 12 Wave B lanes remain (Issues #51, 52,
53, 59, 63, 64, 65, 68, 69, 70, 71, 72).

### Wave B / Lane 05 / asset (#86)
- **zstd cooker side** — `LmpakWriter::write_entry(..., compress=true)`
  zstd-compresses payloads at level 6, sets per-entry flag, stores both
  compressed + uncompressed sizes. Default + `std::span` overload.
- **`.lmm` / `.lmt` / `.lma` round-trip** — new `Codecs.h` + `Codecs.cpp`
  with `lmm::write_mesh/read_mesh`, `lmt::write_texture/read_texture`,
  `lma::write_audio/read_audio`. `static_assert`s pin wire-byte sizes
  (16/56/16/32/16/40) to catch ABI drift.
- **Hot-reload watcher hardened** — single state machine handles mtime
  change, delete, rename-away, recreate, and rename-into-place as
  single-edge events.

### Wave B / Lane 09 / render-post (#84)
- **Motion blur** — `VelocityField` path + depth-reproject path; cvars
  `r_motion_blur_strength` (0 is strict no-op, verified) and
  `r_motion_blur_max_pixel`.
- **Volumetric fog froxel grid** — exact §8.4 dims (160×90×64 = 921 600
  cells), CPU-side in-scatter, optional `OccluderFn` callback for
  shadowed contributions binds to lane 08's `Tlas::occluded` at call
  site without a hard dep. Beer-Lambert ray-march at resolve with
  per-pixel depth back-stop. Cvars `r_fog_enable`, `r_fog_density`.
- **Rain streaks** — 4 096-streak pool, gravity-driven sim, alpha-blended
  HDR composite lit by the same `FogLight` list. Cvars `r_rain_enable`,
  `r_rain_intensity`.

### Wave B / Lane 11 / world-outdoor (#85)
- **Heightmap raymarcher SIMD** — 8-wide AVX2 + 4-wide NEON column-batch
  kernels (`simd_step_columns8/4`, `simd_sample_bilinear8/4`, etc.) on
  top of lane 03's `f32x8`/`f32x4`. Scalar reference preserved.
- **CDLOD inter-LOD vertex morph** — `CdlodMorph` struct + endpoint-clean
  morph (morph=0 → fine bitwise, morph=1 → coarse bitwise). Watertight
  invariant preserved at every (lod_level, morph_factor) for same-LOD
  shared edges.
- **Spline track editor ops** — `SplineEditor::insert_control_point`
  (de Casteljau split preserving curve geometry exactly), move/translate
  isolation, delete + neighbor stitching, banking write/read round-trip.

### Wave B / Lane 12 / audio (#87)
- **HRIR set** — 2 elevations × 12 azimuths × 256 taps per ear via
  deterministic closed-form IRCAM-LISTEN-style synthesis (ITD + 1-pole
  shadow LPF + elevation-dependent pinna echo). Bilinear lookup in (az,
  el) plus per-bin magnitude crossfade between adjacent azimuth bins.
- **Partitioned FFT convolution reverb** — overlap-save with up to 32
  partitions, configurable overlap factor (1/2/4), per-channel
  impulses, scratch pool sized once in `reset()`.
- **Modulated FDN reverb** — four mutually-incommensurate LFOs (0.83×,
  1.00×, 1.21×, 1.47×) drive fractional read-pointer offsets with
  linear interp; depth 3.5 samples. Kills fixed-frequency ringing.
- **Doppler shift** — classical from positions+velocities, clamped to
  [0.5×, 1.5×]. `Voice` struct gained a `velocity` field defaulting
  to zero so existing Wave-A tests still pass.

### Wave B / Lane 17 / ui-rml (#88)
- **Hot reload between frames** — parses incoming `.rml`/`.rcss` into
  `ParsedPair` temp, atomically swaps `doc.root` + `doc.sheet` into the
  existing map slot. `needs_reload` is now `std::atomic<bool>` for
  watcher-thread → main-thread handoff. Document identity (keyed by
  name) and `visible` preserved across reload.
- **Lua handler payload** — generated prelude
  `local event = { kind, target_id, mouse_x, mouse_y, button };` is
  prepended to each handler body before dispatch via the
  `set_lua_backend` hook. Strings Lua-escaped so generated chunks parse
  cleanly even with `"`, `\`, or newlines in element ids.
- **Sample HUD** — `samples/03_quake_room/assets/hud.rml` + `.rcss`
  shipped: Quake-style HUD (health bar, armor bar, ammo counter,
  minimap shape, crosshair, pause menu with Lua handlers).

### Test + demo status

- **582/583 Catch2 cases passing** (single known flake: IPC port-race
  under parallel ctest, passes on retry; lane 19 Wave B may fix in next
  batch).
- All M0/M1/M2 demos still smoke-pass.
- New unit tests this batch: 16 (asset) + 9 (post) + 32 (world-outdoor)
  + 25 (audio) + 20 (ui-rml) = 102 new cases.

### Wave B remaining (12 lanes)

02 math, 03 simd, 04 jobs (fibers + P/E pools), 10 world-bsp (portal
culling, lightmap atlas streaming), 14 net, 15 script, 16 ui-imm
(gizmos, brush previews, allocator heatmap), 19 editor-ipc (IDL regen,
state-delta, IPC flake fix), 20 editor-web (asset browser, profiler
upgrade, prop menu), 21 platform-win32, 22 platform-linux, 23
platform-macos.

## v0.3.0-wave-b-batch2 (2026-05-19) — Wave B 8/25 lanes, 477/478 tests pass

Five more Wave B lanes shipped on top of v0.2.0-wave-b-partial — bringing
Wave B to 8/25 integrated. 17 Wave B lanes remain (Issues #51, 52, 53,
54, 58, 59, 60, 61, 63, 64, 65, 66, 68, 69, 70, 71, 72).

### Wave B / Lane 01 / core (#79)
- **Tracy zones** via `PSY_TRACE_ZONE(name)` and `PSY_TRACE_ZONE_COLOR`
  macros — additive on top of `Tracy.h`, no-op when
  `PSYNDER_ENABLE_TRACY=OFF`. Tracy v0.11.1 vendored via FetchContent.
- **Allocator heatmap** (`Heatmap.h`) — `tag_stat()`, `tag_stats()`,
  `reset_peak()`, `reset_peak_all()` snapshot per-`Tag`
  `{current, peak, budget}`. Wired into `LinearArena::alloc/reset` +
  `page_alloc/free`.
- **Allocation flight recorder** (`FlightRecorder.h`) — debug-only
  circular ring with FNV-1a call-site hash, lock-free record path,
  `PSY_FLIGHT_RECORD(op, size, tag)` macro, dump-to-file with
  oldest-to-newest ordering across wraps.

### Wave B / Lane 06 / scene (#82)
- **BVH refit** for dynamic actors — SAH-binned top-down build,
  bottom-up per-frame refit, 1.3× SAH-cost-vs-as-built trigger for
  async rebuild per DESIGN.md §9.4. `update()` marks `needs_refit`
  only; `insert`/`remove` mark `dirty`.
- **SAP (sweep-and-prune)** — 3-axis sorted endpoint lists,
  x-sweep candidate emission with y/z validation,
  `sap_overlap_pairs()` exposed for physics broadphase.
- **Hashed grid** — sparse 4096-bucket chained store with sphere-vs-AABB
  closest-point radius queries.
- **Query router** — `route_query(QueryKind, region)` honours per-region
  + global-kind overrides on top of the §9.4 default table.
- No `virtual` in any BVH / SAP / grid hot loop; only the editor/debug
  `ISpatialIndex` virtual surface preserved from Wave A.

### Wave B / Lane 08 / render-rt (#80)
- **Real 8-way SIMD slab test** in single-ray BVH traversal — AVX2
  loads each axis as `_mm256_load_ps` × 6 + branchless slab math +
  single `_mm256_movemask_ps` for the hit mask. NEON splits to two
  4-wide halves over the same SoA layout.
- **NEON 4-wide packet shadow driver** — 8-ray `ShadowPacket8` as two
  4-wide halves on arm64.
- **Denoiser à-trous** — 2-pass with step-doubling (1, 2); bilateral
  convex combination guarantees monotonicity.
- **Heightmap shadow path** — `trace_heightmap_shadow(hm, ray, max_steps)`
  with logarithmic-distance steps, bilinear sub-cell sampling,
  Y-slab pre-cull. No BVH, no allocations. Per DESIGN.md §8.2.

### Wave B / Lane 13 / physics (#81)
- **Full Pacejka '94 magic formula** — combined slip (longitudinal ×
  lateral), B/C/D/E coefficients, friction-circle clipping, monotone
  in slip ratio + zero force at zero load.
- **Drivetrain** — engine torque curve (RPM → N·m table interp),
  clutch with slip, 6+R gearbox, 50/50 final-drive differential.
  Persistent engine RPM across steps.
- **Aero** — ½ρv²·Cd·A drag + ½ρv²·Cl·A downforce applied at COM.
- **Character stair-step climb-up** — templated overlap predicate
  + intent/environment flag transitions for Stand/Crouch/Prone/
  Ladder/Water states with priority ordering.
- Bench `physics_vehicle.cpp` — ~0.2 µs/step on M4 Max.

### Wave B / Lane 24 / tools (#83)
- **`lm_qbsp` portal generation** — `.psybsp` v2 emits `BspPortal`
  records (4-vertex square winding on splitter plane) for each
  splitter node joining two non-solid leaves.
- **`lm_bake` multi-bounce indirect** — 2-4 bounces of cosine-weighted
  stratified hemisphere gather, energy-conservative recurrence
  L_(k+1) = direct + albedo·mean(L_k). Deterministic per-texel jitter.
  CLI gets `--bounces N` and `--samples N`. Direct-only path
  byte-identical when `bounces=0`.
- **stb_image proper** — vendored stb_image v2.30 + stb_image_write
  under `tools/lm_cook/third_party/`. Cooked .lmt textures decode in
  any image viewer.
- **`lm_mapimport` curve brushes** — `@cylinder` and `@sphere`
  directives in `.map` expand to convex face lists. Cylinder →
  N-gonal prism. Sphere → icosahedron with optional Loop subdivision.

### Test + demo status

- **477/478 Catch2 cases passing** (single flake: the known IPC
  port-acquisition race under parallel ctest; passes on retry).
- All three demos (sample_00, sample_01, sample_02) still smoke-pass.
- New unit tests added by this batch: 13 (core) + 21 (scene) + 18
  (render-rt) + 24 (physics) + 9 (tools) = 85 new cases.

### Wave B remaining (17 lanes)

02 math, 03 simd, 04 jobs (fibers + P/E pools), 05 asset (zstd cooker
+ round-trip), 09 render-post (motion blur, volumetric fog, rain),
10 world-bsp (portal culling), 11 world-outdoor (SIMD raymarch + CDLOD
morph), 12 audio (HRTF refine, FFT reverb, Doppler), 14 net, 15 script,
16 ui-imm (gizmos, brush previews, allocator heatmap), 17 ui-rml
(hot reload, Lua handlers, sample HUD), 19 editor-ipc (IDL regen,
state-delta), 20 editor-web (asset browser, profiler upgrade, prop
menu), 21 platform-win32 (WASAPI mixer, FFB wheel), 22 platform-linux
(PipeWire wiring, ALSA fallback, dmabuf), 23 platform-macos (CoreAudio
polish, IOSurface, HID FFB).

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
  `Co-Authored-By: AI tool` trailers per the repo's contributor-list
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
