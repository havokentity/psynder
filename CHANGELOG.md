# Changelog

## v1.0.7 — RT frame SDK, AO renderer, terrain scheduling, and console polish

This release turns the post-1.0 RT samples into reusable engine infrastructure,
adds CPU scheduling controls for heavier rendering paths, and polishes the
developer console workflow. CI was green on `main` before the release tag was
published.

### Engine RT / reusable frame renderer
- Added `render::rt::FrameRenderer`, a reusable CPU raytraced frame renderer
  that owns primary visibility, direct lighting, 8-wide shadow packets,
  ambient occlusion, denoising integration, optional mirror bounces, and
  low-resolution upsampling.
- Promoted common RT app/sample helpers into the engine RT API:
  `FrameCamera`, `FrameLight`, `make_frame_camera`, `primary_ray`, `reflect`,
  `upsample_bilinear_rgba8`, and row scheduling helpers for custom RT passes.
- Added tile-parallel and row-parallel scheduling paths with CPU-core hints,
  auto chunk selection, batch controls, and scheduler dump commands.
- Expanded RT ambient occlusion controls: AO enable/debug, sample count,
  radius, ambient strength, direct-light strength, and denoise toggle.
- Added `r_rt_frame_reflection_bounces` so demos/tools can switch the engine
  reflection path between 0 and 1 bounce; the renderer hard-skips reflection
  rays when the bounce count is zero or the material table has no reflectors.
- Optimized BVH/TLAS traversal and denoising code paths used by the heavier RT
  demos.

### RT samples
- Refactored `sample_12_rt_showcase` to hand TLAS, camera, lights, and
  instance materials to the engine `FrameRenderer` instead of carrying a local
  renderer implementation.
- Swept the RT-facing samples (`sample_05_rt_shadow_packets`, `sample_11_rt_spheres`,
  `sample_12_rt_showcase`, and `sample_13_rt_quake`) to use shared engine RT
  camera/light/helper APIs rather than duplicated sample-local structs and
  functions.
- Removed sample-local RT frame loops from samples 05, 11, and 13; the demos
  now demonstrate engine-owned shadow packets, AO plumbing, debug HUD history,
  and optional reflection bounces through `FrameRenderer`.
- Kept smoke-mode behavior deterministic for the RT samples while improving the
  runtime tuning surface for interactive/perf testing.

### Scene graph integration
- Added a lightweight `scene::SceneGraph` path for cached world transforms and
  analytic sphere instances.
- Taught the RT frame renderer to trace scene-graph analytic spheres even when
  no TLAS is supplied, giving tools/samples a simple SDK path for procedural RT
  content.
- Added unit coverage for scene graph parent/child transform propagation and
  analytic sphere gathering.

### Terrain raymarch scheduling
- Parallelized the outdoor terrain raymarch path through the job system.
- Added terrain scheduler CVars for CPU core hints, minimum rows per core, and
  explicit batch rows.
- Added `r_terrain_sched_dump` plus a developer-console terrain autotune command
  that evaluates candidate scheduler settings and restores previous values on
  stop.
- Added a SIMD-oriented terrain raymarch loop optimization pass.

### Developer console polish
- Added console config load/save support.
- Improved autocomplete behavior so Escape suppresses the popup until typing
  resumes.
- Added line wrapping, watermark rendering, and configurable visual scrollback.

### Tests and validation
- Added focused RT frame helper tests for camera construction, primary rays,
  reflection math, and row scheduler behavior.
- Added RT analytic-sphere renderer coverage.
- Applied the CI-required `clang-format-17` formatting pass after the first
  `main` CI run caught formatting drift.

## v1.0.0 — DESIGN.md M0–M8 all covered, 632/632 tests 🚀

Wave E closes the parallel-agent push. Psynder hits v1.0.

### Wave E / .lmpak format unification (#111)
- Lane 24's `lm_pak` writer and lane 05's `Vfs` reader spoke different
  `.lmpak` v1 dialects (flagged in Wave D's #108). Wave E **teaches the
  reader both dialects** — `parse_lmpak_canonical` for the original
  `LmpakWriter` layout (64-byte header + explicit entry/name-table
  offsets) and a new `parse_lmpak_tools` for the cooker's layout
  (56-byte header + combined-index region). Detection auto-selects.
- `PakMount` now owns optional `entries_owned` / `names_owned` buffers
  so the tools dialect materialises a canonical record set in memory;
  the rest of the reader (`find_in_pak`, `load_from_pak`) stays
  dialect-agnostic.
- Fixture test bumped from WARN to hard REQUIRE. Two new closed-loop
  Catch2 cases (`LmpakWriter → mount_pak → read` and `tools::pack_blobs
  → mount_pak → read`).

### Wave E / TerrainRaymarch::render wired to framebuffer (#112)
- `engine/world/outdoor/TerrainTarget.h` (new sibling header, frozen
  `Terrain.h` byte-identical) exposes `set_target(rm, fb*)`.
- `TerrainRaymarch::render(view, proj)` now actually paints into the
  bound framebuffer — derives eye/forward/right/up from the column-
  major `look_at_rh` + `perspective_rh` matrices via the orthonormal-
  transpose trick (no `Mat4::inverse` in core math), runs the per-
  column logarithmic march from sample_06 into pixels + packed depth.
- Wave-B SIMD kernels in `Raymarch_internal.h` still callable and
  unchanged; the new public path uses the same `detail::sample_bilinear`
  / `splat_at_texel` / `normal_at_texel` building blocks plus the
  sample_06 lighting palette so visual output is continuous.
- 3 Catch2 cases: identity-camera flat heightmap writes pixels +
  sub-far depth; null-target render is a no-op; re-clearing the bound
  target reverts to no-op.
- Sample_06 can drop its inlined raymarch + `detail::*` dip in a
  follow-up — the public API is now sufficient.

### Wave E / RmlUi vendor build + sample_04 cleanup (#113)
- **`PSYNDER_VENDOR_RMLUI=ON` now builds clean.** Root cause: RmlUi
  6.2 dropped the `"none"` backend alias and strictly validates
  `RMLUI_BACKEND` against an allow-list even when `RMLUI_SAMPLES=OFF`.
  Fixed by setting `RMLUI_BACKEND="auto"`. Vendor pull also routes
  via the default `_deps/` staging in the build dir so the source
  tree stays clean. Build detects either `rmlui_core` (6.x snake_case)
  or legacy `RmlCore`.
- **`engine/ui/rml/DataBind.{h,cpp}`** — public per-element setters:
  `set_element_text(doc, id, value)` and `set_element_attribute(doc,
  id, attr, value)`. Behind `#ifdef PSYNDER_HAS_RMLUI` the setters
  route through `Rml::Element::SetInnerRML` / `SetAttribute`;
  otherwise they walk the in-tree DOM and re-cascade. The `style`
  attribute is special-cased to re-parse `inline_style`, and `class`
  re-splits + re-cascades so `computed_style` reflects the new
  override before the next render.
- **`samples/04_nfs_track/main.cpp`** — `build_hud_rml` /
  `reload_with_source` per-frame source-rebuild replaced by
  `push_hud_telemetry` making 6 setter calls per frame (speed text,
  gear letter, rpm-fill width, thr/brk-fill top+height). The test-
  layer hack is gone.
- 4 Catch2 cases / 49 assertions on the new DataBind path.

### Wave E / Four contributor docs (a5d8c05)
- **`docs/01-getting-started.md`** — clone-to-running in 5 min, sample
  roster, smoke-lock pattern, editor panel prereqs, every renderer
  cvar.
- **`docs/02-rendering.md`** — pipeline overview, tile bin, Q24.8
  setup, attribute interpolation, full filter table, surface cache,
  Z-buffer / HiZ, performance budget.
- **`docs/03-lighting.md`** — `lm_bake` radiosity workflow, BVH8
  builder + collapse-to-8, TLAS over instances, refit + async rebuild
  trigger, packet traversal (AVX2 / NEON / scalar), heightmap shadow
  path, denoiser, volumetric / atmospheric.
- **`docs/04-world-formats.md`** — `.lmpak` / `.psybsp` / `.lmm` /
  `.lmt` / `.lma` / `.psylevel` / `.psyc` byte layouts, cook + pack
  flow, hot reload, determinism contract.

## DESIGN.md milestone coverage at v1.0

| Milestone | Status |
|---|---|
| **M0** bring-up (window + clear color, 3 OSes) | ✅ verified on Mac, code shipped for Win/Linux |
| **M1** first triangle | ✅ sample_01 ships a rotating textured triangle |
| **M2** tiled rasterizer + Z + bilinear | ✅ sample_02 ships 4 spinning cubes through the full pipe |
| **M3** BSP + lightmaps + editor v0 | ✅ sample_03 walks a 4-leaf BSP with PVS; React Inspector panel ships |
| **M4** outdoor track + vehicles + sculpt | ✅ sample_04 ships an auto-driven Pacejka vehicle lapping a banked spline track |
| **M5** hybrid raytracing | ✅ sample_05 ships raytraced dynamic-light shadows via 8-wide AVX2 packets |
| **M6** large outdoor FPS maps + sandbox | ✅ sample_06 ships a heightmap-raymarch flyover with helicopter |
| **M7** feature parity (HUD + networking + audio) | ✅ sample_04 HUD via RmlUi; rUDP + lockstep + snapshot shipped; HRIR HRTF + reverbs shipped |
| **M8** editor polish + 1.0 | ✅ Tracy zones, allocator heatmap, deterministic physics, golden-image harness, bench gates, contributor docs |

## Test + build status at v1.0

- **632/632 Catch2 cases passing** on macOS Apple Silicon (mac-release).
- All 7 sample binaries (M0–M6) smoke-pass cleanly.
- Build green on Mac. Win/Linux platform code shipped but unverified
  on real hardware.
- 9 release tags shipped across A→E: `v0.1.0-wave-a`,
  `v0.2.0-wave-b-partial`, `v0.3.0-wave-b-batch2`,
  `v0.4.0-wave-b-batch3`, `v0.5.0-wave-b-batch4`,
  `v0.6.0-wave-b-complete`, `v0.7.0-wave-c-complete`,
  `v0.8.0-wave-d-complete`, `v1.0.0`.

## How v1.0 was built — process notes

The engine was built via a **25-lane parallel-agent wave model**.
Each wave: orchestrator files Issues with strict file-ownership specs,
spawns N background agents (each in its own `git worktree`), merges
their PRs as they land. 5 waves total (A–E), ~70 PRs merged into
`main`, all squash-merged with no `Co-Authored-By` trailers.

- **Wave A** — 25/25 lanes scaffolded with real `.cpp` behind frozen
  public headers
- **Wave B** — 25/25 lanes shipped Wave-B feature pushes across 5
  sub-batches
- **Wave C** — sample_03 / sample_04 / sample_05 wired + editor F2/~
  toggle + DebugHud overlay
- **Wave D** — sample_06 + RmlUi HUD wired + Tracy zones + lm_pak
  fixture pipeline + EWA anisotropic filtering
- **Wave E** — .lmpak format unification + TerrainRaymarch wire-up +
  RmlUi vendor + 4 contributor docs

`AGENTS.md` documents the coordination protocol. `docs/waves-roadmap.md`
maps lanes to waves. `docs/wave-a-bar.md` is the shared deliverable
rubric.

## Known gaps + post-1.0 work

- **Win/Linux validation** — the platform lanes (21/22) shipped real
  Win32 + Wayland/X11 code that was never built on actual hardware.
  CI matrix builds an empty target on those OSes; the user verifies
  their boxes separately.
- **Copilot review sweep** — Copilot Code Review hasn't been enabled
  on the repo yet. Once toggled (Settings → Code & automation → Code
  review), the `copilot-review` skill can run across all merged PRs
  and address legitimate concerns in a `chore/copilot-sweep` PR.
- **Sample 06 cleanup** — Wave E's `TerrainRaymarch::render`
  wire-up lets sample_06 drop its inlined raymarch in a follow-up.
- **Full RmlUi vendor as default** — `PSYNDER_VENDOR_RMLUI=ON` now
  works but is opt-in; flipping the default needs a one-time review
  of the upstream dep cost.
- **Cross-platform validation harness** — CI runs the build matrix
  but not actual sample-run smoke tests on Win/Linux runners (no
  display server in the runner).

## v0.8.0-wave-d-complete (2026-05-19) — All 7 demos run, M6 + HUD + EWA aniso 🚁🏁

Wave D closes the polish push. **All seven sample binaries** (M0 through
M6) now smoke-pass on Mac. The Quake-style HUD is wired into a sample
for the first time. Tracy zones instrumented in core hot paths. EWA
anisotropic filtering completes the DESIGN.md §7.5 filter table.
End-to-end `lm_pak` fixture pipeline cooks real assets at build time.

### Wave D / sample_06 / Tactical map (M6) (#109)
- Project IGI / Delta Force-style aerial flyover. 512×288 internal
  framebuffer (per-column raymarch budget).
- 256×256 procedural heightmap (three octaves of value noise + a big
  Z-axis Gaussian ridge + a SE bump). Peaks reach ~30m.
- Direct drive of `world::outdoor::detail::*` per-column raymarch
  internals (Wave-A `TerrainRaymarch::render` is still a no-op pending
  lane-07 tile-queue integration — flagged for Wave E).
- Shared Z packing matches `TileRaster::pack_depth` so the helicopter
  and watchtowers Z-test correctly against the raymarched terrain
  (DESIGN.md §9.2 shared-Z invariant).
- Dawn skybox (warm orange near horizon → cool blue at zenith), 4-channel
  splat blend (grass/rock/sand/snow), Lambert against a low-east sun
  + distance-fog haze.
- Six watchtowers placed in a hex ring around the centre, each
  Y-snapped to the terrain. Scripted helicopter at 50m altitude
  (body box + spinning 4-blade rotor, yaw aligned to flight path).
- Half-circle camera flyover at ~30m altitude.

### Wave D / sample_04 HUD wiring (M7) (#107)
- `samples/04_nfs_track/assets/hud.rml` (62 lines) + `hud.rcss` (~125
  lines) — dark-on-grey racing dashboard: large KM/H readout, RPM bar
  with redline tick, gear letter, throttle/brake vertical indicators.
  Anchored to a 1280×720 viewport.
- `main.cpp` initializes `psynder::ui::rml`, mounts the assets dir,
  loads `hud.rml` + shows it, calls `update/render` per frame.
- Live telemetry: speed from chassis-position-derived velocity, RPM
  from wheel ω × gear ratio × final drive with gear band-selected
  from speed.
- Wired via per-frame source rebuild + `reload_with_source` (same
  path the unit tests use) — the in-tree RmlUi subset has no
  per-element setters yet. Pattern swap-compatible with upstream
  RmlUi data-binding when `PSYNDER_VENDOR_RMLUI=ON` lights up.

### Wave D / Tracy zones (#106)
- `engine/core/Tracy.h` extended with three new macros (existing
  `PSY_TRACE_ZONE` / `PSY_TRACE_ZONE_COLOR` left untouched):
  - `PSY_TRACE_FRAME(name)` → `FrameMarkNamed` / no-op
  - `PSY_TRACE_PLOT_F32(name, value)` → `TracyPlot` / no-op
  - `PSY_TRACE_MESSAGE(text)` → `TracyMessageL` / no-op
- Instrumentation: `Log::emit`, `Console::Execute`, `Console::Drain`
  wrapped in `PSY_TRACE_ZONE` (allocator hot paths were already
  instrumented in Wave B).

### Wave D / lm_pak fixture pipeline (#108)
- `assets/fixtures/crate.png` (32×32 RGBA8 — synthesized from
  `samples/01_triangle/assets/crate.ppm`) + `tone_440hz.wav` (1 s,
  mono, 48 kHz, int16 PCM).
- Build-time custom commands cook them via lane 24's `lm_cook`
  → `crate.lmt` + `tone_440hz.lma`, then pack via `lm_pak` →
  `build/${preset}/assets/demo.lmpak`. New target `psynder_fixture_pak`
  builds with `ALL`.
- Catch2 test loads the pack, verifies the FNV-1a-64 index (incl. case
  insensitivity + hash-sort for binary search), extracts payloads,
  confirms `LMT1`/`LMA1` magics survived the round-trip.
- **Found** during integration: lane 24's `lm_pak` producer and lane
  05's `Vfs` consumer speak different `.lmpak` v1 dialects. Documented
  in the test as a probe (WARN on mount failure) so future format
  unification surfaces here. Tracked for Wave E.

### Wave D / Raster anisotropic filtering (#110)
- **EWA** (elliptical-weighted-average) approximation per DESIGN.md
  §7.5 — `aniso_sample()` walks N taps along the major axis of the
  texture-space ellipse derived from per-quad UV gradients; each tap
  a bilinear lookup, blended with cosine-bell weights.
- `r_anisotropy` cvar (1/2/4/8/16, clamps out-of-range).
- `DrawCmd.aniso_max` plumbed (1 byte added to the cache-line-aligned
  `DrawCmd`; `TileBin.h` is lane-internal so no public-header
  change).
- Per-tile dispatch: `use_aniso = (cap ≥ 2) && (major² > 4·minor²)`
  const-bool gate, no per-pixel branching.

### Sample status (all 7 smoke-pass)

| Sample | Milestone | Status |
|---|---|---|
| sample_00_clear | M0 | ✅ smoke ✅ visual (animated clear) |
| sample_01_triangle | M1 | ✅ smoke ✅ visual (rotating crate triangle) |
| sample_02_textured_quad | M2 | ✅ smoke ✅ visual (4 spinning cubes) |
| sample_03_quake_room | M3 | ✅ smoke ✅ PVS branches verified |
| sample_04_nfs_track | M4 + M7 HUD | ✅ smoke (Pacejka lap + RmlUi dashboard) |
| sample_05_rt_shadow_packets | M5 | ✅ smoke (raytraced shadows + 3 lights) |
| sample_06_tactical_map | M6 | ✅ smoke (raymarch terrain + helicopter) |

### Test + demo status

- **623/623 Catch2 cases passing**, zero flakes.
- All M0–M6 demos smoke-pass on Mac.
- Tagged `v0.8.0-wave-d-complete`.

### What's left for v1.0 (Wave E)

1. **`TerrainRaymarch::render` → tile-queue integration** so sample_06
   uses the public API instead of `detail::*` internals.
2. **`.lmpak` format unification** — lane 24's producer and lane 05's
   consumer to agree on a single v1 wire format. Probably bump to v2
   and migrate.
3. **Per-element RmlUi setters** — eliminate the per-frame source
   rebuild in sample_04 (or vendor upstream RmlUi via
   `PSYNDER_VENDOR_RMLUI=ON`).
4. **Documentation pass** — `docs/01-getting-started.md`,
   `docs/02-rendering.md`, contributor guide.
5. **Win/Linux validation** on real hardware.
6. **Copilot review sweep** — once user toggles Copilot on, run
   `copilot-review` skill across all merged PRs.

## v0.7.0-wave-c-complete (2026-05-19) — M3/M4/M5 demos running, 616/616 tests 🚗⚡

Wave C made the engine's milestone roadmap **visible**. All five sample
binaries (M0 → M5) now smoke-pass on Mac, each exercising a meaningful
subsystem stack rather than rendering a stub clear-color.

### Wave C / sample_03 / Quake room (M3) (#104)
- Walking-POV BSP demo. Synthetic 4-leaf BSP map built in-memory
  (Room A + corridor + Room B + solid-outside) with a real 4-node
  tree splitting on Z then X. 3-cluster PVS bitmap wired so the
  walker actually culls.
- 18 quad faces with per-cluster colors (cool blue / green / warm
  orange) so the BSP topology reads visually as you walk.
- WASD + mouse-look camera via `platform::input()`. ESC quits.
- Sweep verifies all three PVS branches fire: Room A→11 draws,
  corridor→18 (sees both rooms), Room B→11.

### Wave C / sample_04 / NFS track lap (M4) (#105)
- Chase-cam vehicle lap of a closed oval built from 4 cubic-Bezier
  `SplineRoadSegment`s with banking on the turns.
- Track tessellated as an extruded strip — tarmac alternating per
  segment + kerb stripe.
- 1500 kg Pacejka vehicle (4 wheels, 450 N·m engine) auto-driven
  by a P-controller pair: heading-error → steer (±0.55 rad),
  speed-error → throttle/brake toward 20 m/s. 14 m look-ahead
  advances along the closed loop.
- Fixed-step 120 Hz physics with state interpolation. Chase camera
  ~5.5 m back / ~2.2 m up, look-target pushed 6 m ahead. Wheels are
  12-sided cylinders built at startup.

### Wave C / sample_05 / RT shadow packets (M5) (#102)
- Night scene with **raytraced dynamic-light shadows**. Ground plane
  + 5 colored cubes (red/green/blue/yellow/magenta). 3 orbiting RGB
  point lights at different radii / speeds.
- Per pixel: primary-ray TLAS intersection, then 8-wide
  `trace_shadow_packet` shadow rays per light, with Lambert +
  inverse-square attenuation. Dark-blue vertical-gradient skybox.
- Quarter-resolution (256×144) with bilinear upsample to 512×288
  to keep frame cost low.
- Smooth orbital camera tour.

### Wave C / editor mode toggle (#101)
- `engine/editor/core/HotKey.{h,cpp}` — `editor::handle_input_frame(...)`
  watches `Tilde` and `F2` via `key_pressed` (edge, not held).
- `engine/editor/core/SampleHook.h` — header-only
  `editor::sample_step(input, fb)` for samples to call once per frame:
  draws the PLAY/EDIT badge in the bottom-right, returns current mode.
  Samples can branch on it (freeze physics in Edit, etc.).

### Wave C / debug HUD overlay (#103)
- `engine/ui/imm/DebugHud.{h,cpp}` — `imm::draw_debug_hud(fb, stats)`
  with three modes (Off/Compact/Full) gated by `r_debug_hud` cvar.
- Full mode shows: frame time + FPS, 60-sample strip chart, lane-01
  allocator heatmap, 6-line `PSY_DIAG_TIER1` ring.
- Compact mode: header + strip chart only.

### Sample status (all smoke-pass)

| Sample | Milestone | Status |
|---|---|---|
| sample_00_clear | M0 | smoke ✓ visual ✓ |
| sample_01_triangle | M1 | smoke ✓ |
| sample_02_textured_quad | M2 | smoke ✓ visual ✓ (4 spinning cubes) |
| sample_03_quake_room | M3 | smoke ✓ (PVS branches verified) |
| sample_04_nfs_track | M4 | smoke ✓ (auto-driver laps the oval) |
| sample_05_rt_shadow_packets | M5 | smoke ✓ |

Sample_06 (tactical map / M6) and any wired HUD samples are Wave D scope.

### Test + demo status

- **616/616 Catch2 cases passing** (zero flakes).
- All M0/M1/M2/M3/M4/M5 demos smoke-pass.
- Tagged `v0.7.0-wave-c-complete`.

### What's left for v1.0

- **Wave D — Sample 06 (tactical map M6) + HUD sample wiring (M7) +
  Tracy zone instrumentation throughout hot paths (M8 polish) +
  DynamicLightList API for production raytraced lighting.**
- **Visual verification on Mac** — sample_03 / sample_04 / sample_05
  now have real geometry; please confirm they look correct.
- **Win/Linux validation** — still untested on actual hardware.
- **Copilot review sweep** — pending the user toggling Copilot Code
  Review on in repo settings.

## v0.6.0-wave-b-complete (2026-05-19) — Wave B 25/25 lanes, 612/612 tests pass 🎉

Final Wave B batch shipped. **All 25 lanes integrated.** Wave B done.

### Wave B / Lane 10 / world-bsp (#96)
- **Portal clipping** (`PortalClip.{h,cpp}`) — real portal-graph BFS walk
  replacing Wave A's PVS-only fallback. Clips view frustum at each portal
  via side planes generated from eye and CCW-convention portal edges.
  Falls back to PVS when `BspPortalSet.portals` is empty.
- **Lightmap atlas** (`LightmapAtlas.{h,cpp}`) — level-scope, slab-allocated
  from a bound `LinearArena`. 96 KiB pages (128×128 RGB16F). 256-page
  resident cap with LRU eviction. `atlas_page_for_surface(face_id)` is
  idempotent — same id → same `LightmapPage*` until eviction.

### Wave B / Lane 14 / net (#98)
- **Configurable sliding-window depth** — `WindowSize` enum (`Bits32`/
  `Bits64`/`Bits128`). `AckTracker` tracks 128 bits internally; send-ring
  256 slots. `apply_ack_wide()`/`snapshot_wide()` carry up to 128 bits
  via four u32 words. Legacy 32-bit form still on the wire by default;
  bits above the operational width masked off so a narrow host can't
  leak wide bits.
- **AOI per-channel priorities** — `set_channel_priority(channel, prio)`,
  `bytes_for_channel(channel, base)`. Priority defaults to 2; bytes =
  `base * priority / 2`. Tracked for the four reserved channel ids
  (default/lockstep/snapshot/reserved).

### Wave B / Lane 19 / editor-ipc (#100)
- **State-delta push** — `Server::push_scene_delta(slice, payload)`
  wraps payload in a `SceneDeltaFrame` (opcode 20) tagged with the
  slice; delivered only to connections subscribed to that channel.
- **Schema versioning** — `protocol.psy` bumped to v2. `Welcome::server_ver`
  carries the new value; v1 decoders see `server_ver == 2`. Forward-compat
  fallback for unknown opcodes already in `client_loop` default arm.
- **REPL hook wired** — `Server::start()` calls `install_repl_backend()`,
  `pump()` routes `ConsoleCmd` text through `script::dispatch_repl(...)`
  and ships back a `ConsoleReplyFrame` (opcode 21). Custom backends
  installed via `script::set_repl_backend(...)` are honoured.
- Drive-by: fixed `read_exact` overflow bug in `editor_ipc_session.cpp`
  that silently dropped bytes when `recv()` returned more than wanted.

### Wave B / Lane 20 / editor-web (#94)
- **AssetBrowser panel** (`src/panels/AssetBrowser.tsx`) — collapsible
  per-category groups, filter-as-you-type, visible/total counter,
  sorted tables. Mock data via `mock_assets()` until the real
  `assets` channel comes online.
- **PropSpawn panel** (`src/panels/PropSpawn.tsx`) — thumbnail grid
  with category filter chips, click-to-spawn sending
  `selection.spawn_prop` via the IPC client. Recent-spawn highlight.
- **Profiler upgrade** — second canvas2d stacked-bar strip-chart
  showing per-subsystem CPU time per frame (render, physics, audio,
  etc.) on top of the existing total-frame chart. Stable hashed
  colors per subsystem name.

### Wave B / Lane 21 / platform-win32 (#95)
- **WASAPI mixer wired** — strong overrides for
  `audio::backend_init_wasapi`/`backend_shutdown_wasapi` take precedence
  over lane 12's `[[gnu::weak]]` no-op fallbacks. `WasapiBridge` holds
  lane-12 stereo callback + scratch buffer reserved up-front. Stereo
  memcpy / mono downmix / 5.1+7.1 front-L/R upmix paths.
- **DirectInput FFB wheel** (`Win32Ffb.{h,cpp}`) — enumerates
  FF-capable wheels via `IDirectInput8::EnumDevices` with
  `DIEDFL_FORCEFEEDBACK`. Constant-force + spring (`DICONDITION`) effects
  created on demand via `IDirectInputDevice8::CreateEffect`, patched
  with `SetParameters` afterwards. Exclusive-foreground cooperative
  level when an HWND is available.

### Wave B / Lane 22 / platform-linux (#99)
- **PipeWire mixer wired** — `dlopen` libpipewire-0.3, `pw_thread_loop_new`
  + `pw_stream_new_simple` with `on_process` thunk. Dedicated pump
  thread drives lane-12's `MixerCallback` at `buffer_frames/sample_rate`
  cadence.
- **ALSA fallback wired** — `dlopen` libasound.so.2, `snd_pcm_set_params`
  (S16_LE/2ch/48k), writer thread float→int16 with `snd_pcm_recover`
  on -EPIPE/-ESTRPIPE.
- **dmabuf zero-copy** — `WaylandWindow::try_dmabuf_present(fb)` —
  zwp_linux_dmabuf_v1 v3 binding, lazy `/dev/dma_heap/system` open with
  `linux,cma` fallback. `DMA_HEAP_IOCTL_ALLOC` linear buffer +
  `zwp_linux_buffer_params_v1::create_immed` with XRGB8888. Sticky-disabled
  on any failure so we don't keep paying syscall cost. Falls back to
  wl_shm path automatically.

### Wave B / Lane 23 / platform-macos (#97)
- **CoreAudio mixer wired** — strong overrides for
  `audio::backend_init_coreaudio`/`backend_shutdown_coreaudio` calling
  `audio_start` with `coreaudio_render_thunk` that forwards into lane
  12's interleaved-stereo `MixerCallback`. Handles 1/2/N-channel device
  layouts.
- **IOSurface zero-copy upload** — first-frame allocates an `IOSurfaceRef`
  (BGRA / 4-bpp / IOSurface-aligned pitch); `id<MTLTexture>` bound via
  `newTextureWithDescriptor:iosurface:plane:`. Per-frame upload becomes
  `IOSurfaceLock` + `memcpy` + `IOSurfaceUnlock` — no
  `replaceRegion:withBytes:` staging copy. Falls back cleanly if
  IOSurface allocation refuses.
- **HID FFB** — `IOServiceMatching(kIOHIDDeviceKey)` walk, filtered with
  `FFIsForceFeedback`, opened via `FFCreateDevice`. Cartesian-direction
  `FFEFFECT` with `FFCONSTANTFORCE` type-specific params, submitted via
  `FFDeviceCreateEffect` + `FFEffectStart`. Updates via
  `FFEffectSetParameters`.

### Cumulative Wave B accomplishments (25/25 lanes)

By DESIGN.md §13 milestone:
- **M0 (clear color)** ✓ verified visually on Mac
- **M1 (textured triangle)** ✓ verified visually on Mac
- **M2 (tiled raster + Z + bilinear + crate room)** ✓ M2 demo renders
  spinning textured cubes
- **M3 (BSP + lightmaps + editor v0)** ✓ BSP loader, lightmap atlas
  streaming, brush CSG + sculpt, physgun, editor IPC + React panels
  (Inspector / Console / Profiler / AssetBrowser / PropSpawn),
  `.psylevel` + `.psyc` save/load
- **M4 (outdoor track + vehicles)** ✓ in-code: CDLOD watertight mesh,
  SIMD raymarcher backend, full Pacejka tires + 6+R drivetrain + aero,
  spline track editor ops. Needs sample_04 wiring.
- **M5 (hybrid raytracing + headlights)** ✓ in-code: BVH8 SAH +
  8-wide AVX2 packet + 4-wide NEON + à-trous denoiser + heightmap
  shadow path. Needs sample_05 wiring.
- **M6 (tactical map + sandbox editor)** ✓ in-code: heightmap raymarcher
  SIMD, full editor constraints + physgun + save/load.
- **M7 (RmlUi HUD + networking + audio)** ✓ in-code: RmlUi hot reload +
  Lua handlers + Quake-style HUD assets, rUDP with configurable window
  + AOI priorities + lockstep + snapshot, HRIR HRTF + partitioned FFT
  reverb + modulated FDN + Doppler.
- **M8 polish** — Tracy zones in core, allocator heatmap viz, deterministic
  physics with `-fno-fast-math`, 612 unit tests + golden harness +
  bench gate.

### Test + demo status

- **612/612 Catch2 cases passing** (zero flakes — the previously known
  IPC port-race is gone).
- **M0/M1/M2 demos all smoke-pass** on Mac.
- Repo is **~60-65K LOC across engine/tools/tests/samples**.
- All Wave B PRs (#76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88,
  89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100) merged into main.

### What's left for v1.0

- **Sample wiring (M4-M7 demos)** — the engine code is there; samples
  04/05/06 just need their `main.cpp` calling the right APIs.
- **Cross-platform validation** — Win32 + Linux PRs landed but were never
  built on actual hardware. The Mac orchestrator can't verify; needs
  PC/Linux smoke runs from the user.
- **Copilot review sweep** — Copilot Code Review hasn't been enabled on
  the repo yet. Once the user toggles it (Settings → Code & automation
  → Code review), I run the `copilot-review` skill across all 50
  merged PRs and address legitimate concerns in a `chore/copilot-sweep`
  PR.

## v0.5.0-wave-b-batch4 (2026-05-19) — Wave B 18/25 lanes, 602/602 tests pass

Five more Wave B lanes shipped. Wave B is now **18/25 integrated**.
7 lanes remain (Issues #59, 63, 68, 69, 70, 71, 72). Notably **the IPC
flake (test #75) is now passing reliably** — likely a side-effect of
lane 04's hetero job-pool work changing scheduling order.

### Wave B / Lane 02 / math (#90)
- **`BigCoordWorld`** — `Vec3 origin`, `f32 trigger_radius=1024`,
  `snap_to_camera(Vec3) -> Vec3` per-axis quantized snap to nearest cell
  (cell = 2× radius). Zero offset when inside radius. Per ADR-005.
- **Pacejka helpers** — `pacejka_slip_ratio(ω, r, v)` (SAE convention,
  0.1 m/s floor), `pacejka_combined_force(sx, sy, load, μ)` with B=10,
  C=1.9, D=μ·load, E=0.97 + friction-circle projection.
- **Batched transforms** — `transform_points` + `transform_dirs` scalar
  homogeneous transform; Wave C can vectorize behind the same signature.

### Wave B / Lane 03 / simd (#89)
- **Prefetch helpers** — `prefetch_t0/t1/t2/nta` wrapping
  `__builtin_prefetch` / `_mm_prefetch`. Plus `prefetch_range(p, bytes,
  stride=64)` for sequential prefetch with null/zero guards.
- **Streaming stores** — `stream_store_f32x4` using `_mm_stream_ps` on
  x86 / `vst1q_f32` non-temporal on aarch64. `stream_fence` via
  `_mm_sfence` / `dmb ishst` / atomic_thread_fence fallback.
- **AVX-512 forward decl** — `f32x16` struct gated on `__AVX512F__`;
  `has_avx512f()` runtime probe through `hardware::detect()`. Full op
  set deferred to Wave C.

### Wave B / Lane 04 / jobs (#93)
- **Fibers** — Win32 `CreateFiberEx`/`SwitchToFiber` + POSIX
  `ucontext` behind one primitive (create/resume/yield/destroy + host
  conversion). Caught a macOS-specific gotcha: Darwin's `ucontext_t`
  has `uc_mcontext` as a pointer (vs embedded on glibc), so each
  context needs an `_STRUCT_MCONTEXT` allocated alongside before
  `getcontext` — without it, `swapcontext` segfaults.
- **P/E heterogeneous pools** — detection via
  `sysctl hw.perflevel*.physicalcpu` (macOS),
  `GetLogicalProcessorInformationEx + EfficiencyClass` (Win32),
  `/sys cpufreq` bucketing (Linux). Per-worker class hints via
  `pthread_set_qos_class_self_np` /
  `SetThreadInformation ThreadPowerThrottling`. Three inboxes
  (unified/latency/throughput) with class-aware work stealing —
  same-class first, cross-class fallback. New `submit_latency` /
  `submit_throughput` in `JobSystemHetero.h`.

### Wave B / Lane 15 / script (#91)
- **REPL hook** — `set_repl_backend(fn)` / `repl_backend()` /
  `dispatch_repl(line, out)` with atomic-swapped function pointer.
  Default forwards to `Vm::Get().execute_repl`. Lane 19 editor-ipc
  installs its own evaluator without rebuilding script.
- **Lua spawn binding** — `world:spawn(archetype_name, kv_table)`
  allocates a real `scene::EcsRegistry::Get().create()` entity, records
  the per-component bag, returns the integer `Entity::raw` handle.
- PSYNDER_COMPONENT auto-bind deferred to Wave C per the agent's call.

### Wave B / Lane 16 / ui-imm (#92)
- **Full gizmos** — `gizmo_translate`, `gizmo_rotate` (Y-axis Wave B
  scope), `gizmo_scale` taking world `Vec3& pos`, `view_proj`, mouse
  state; world→screen projection + arm hit-testing via `imm::line`.
- **Brush previews** — `brush_preview_box` (12 cube edges),
  `..._cylinder` (two 16-segment rings + 4 struts), `..._sphere`
  (3 orthogonal 24-segment wire rings). Projects world verts via
  `view_proj` and emits `imm::line` segments.
- **Allocator heatmap viz** — `alloc_heatmap(Vec2 origin, Vec2 size)`
  reads `mem::tag_stats()` (lane 01 Wave-B) and draws per-tag bars
  coloured green/yellow/red by `current/budget` ratio with peak
  watermark line.

### Test + demo status

- **602/602 Catch2 cases passing** (the previously-known IPC port-race
  flake now passes reliably under parallel ctest, presumably because
  lane 04's hetero scheduler changed test execution order).
- All M0/M1/M2 demos still smoke-pass.
- New unit tests this batch: 3 (math) + 3 (simd) + 5 (jobs) + 2 (script)
  + 3 (ui-imm) = 16 new cases. (Batch deliberately tight-scoped per
  retry strategy after the original batch hit stream timeouts.)

### Process note: stream-timeout recovery

4 of 5 agents in the original batch-4 attempt hit automation service stream
timeouts (~10 min idle) and lost their in-flight work. Re-spawned with
**tighter single-focus prompts** ("scope must finish in <8 min, skip
ctest, skip nice-to-haves"); all 4 retries succeeded. Lane 04 (jobs)
completed on its original spawn (~24 min — bigger problem, kept the
stream warm via frequent tool calls).

### Wave B remaining (7 lanes)

10 world-bsp (portal culling, lightmap atlas streaming), 14 net (full
sliding window, AOI priorities), 19 editor-ipc (IDL regen, state-delta
push, schema versioning), 20 editor-web (asset browser, profiler upgrade,
prop menu), 21 platform-win32 (WASAPI mixer wiring, FFB wheel), 22
platform-linux (PipeWire wiring, ALSA fallback, dmabuf), 23 platform-macos
(CoreAudio polish, IOSurface, HID FFB).

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

- `engine/scene/EcsRegistry.h` added `query<reads<>, writes<>>()`,
  `set_structural_deferred()`, `apply_structural_changes()` —
  additive only.
- `cmake/Goldens.cmake` adapter from dmonte's `pt_add_golden_cell`.
- `cmake/missing_golden.cmake` driver for absent-baseline cells.

### Next: Wave B (M2 + M3)

See [docs/waves-roadmap.md](docs/waves-roadmap.md) for the lane-by-lane
Wave B focus. M2 = tiled raster polish + crate-room demo. M3 = BSP +
lightmaps + first React editor session + brush CSG v0.
