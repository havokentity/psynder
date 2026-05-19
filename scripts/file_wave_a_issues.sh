#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Psynder — file remaining Wave-A Issues #2..#25. Issue #1 (Lane 01 / core)
# already filed. Each issue body is small and references the shared bar at
# docs/wave-a-bar.md plus DESIGN.md sections. Run from the repo root.

set -euo pipefail

issue() {
    local title="$1"
    local body="$2"
    gh issue create --title "$title" --body "$body" | tail -1
}

BAR="See [docs/wave-a-bar.md](../blob/main/docs/wave-a-bar.md) for the shared Wave-A bar (no \`Co-Authored-By\` trailer, strict file ownership, public headers frozen, mac smoke-lock pattern)."

# ─── Lane 02 / math ──────────────────────────────────────────────────────
issue "Lane 02 / math: implement vec/mat/quat ops + fixed-point Q24.8 + Aabb/Sphere helpers" "$(cat <<EOF
**Lane:** 02-math · **Branch:** \`lane/02-math\` · **Owned paths:** \`engine/math/\`
**Files NOT touched:** anything outside \`engine/math/\`.

$BAR

## Wave-A deliverables
1. **Math.{h,cpp}** — flesh out the rest of the Mat4 ops (inverse, transpose, RH/LH variants), Mat3 ops, full quaternion suite (slerp, lerp, from-Euler, to-Euler).
2. **Fixed-point Q24.8** — \`struct FxQ24_8 { i32 raw; ... };\` with add/sub/mul/div, plus a constexpr converter from/to \`f32\`. Used by the rasterizer's edge functions (DESIGN.md §7.3 — "Q24.8 for exact coverage, 1/256 sub-pixel").
3. **Aabb / Sphere helpers** — union, contains, intersects, expand, transform-by-Mat4.
4. **Big-coord re-centering** — \`origin_recenter(Vec3& world_origin, Vec3 camera)\` snapping the world origin to the camera's region per ADR-005 + §9.2 (the 16 km × 16 km cap).
5. **Rng.h** — PCG already ripped; add a stratified sampler helper (\`StratifiedSampler2D\`).
6. **Tests** — \`tests/unit/math_*.cpp\` covering: Mat4 inverse roundtrip, quaternion slerp endpoints, AABB transform.
EOF
)"

# ─── Lane 03 / simd ──────────────────────────────────────────────────────
issue "Lane 03 / simd: SSE4.2 / AVX2 / NEON intrinsic wrappers + runtime dispatch" "$(cat <<EOF
**Lane:** 03-simd · **Branch:** \`lane/03-simd\` · **Owned paths:** \`engine/simd/\`
**Files NOT touched:** anything outside \`engine/simd/\`.

$BAR

## Wave-A deliverables
1. **Simd.{h,cpp}** — replace scalar fallback with real intrinsics:
   - SSE4.2 baseline (\`_mm_*\`), AVX2 fast path (\`_mm256_*\` + FMA), NEON on aarch64 (\`vaddq_*\`, etc.). Runtime dispatcher chooses widest available kernel at startup.
2. **Operator set** — add/sub/mul/div/fma, cmp_eq/lt/le/gt/ge, blend, min/max, abs, rsqrt/sqrt, gather, load_aligned/unaligned, store_aligned/unaligned, broadcast, reduce_add/min/max, mask creation.
3. **Mask types** — \`mask4\`, \`mask8\` with logical ops and \`mask_to_int\` for tile coverage emission.
4. **AVX-512 opportunistic** — \`f32x16\` for AVX-512 hosts, gated by feature detect. Fallback to two AVX2 halves.
5. **Tests** — \`tests/unit/simd_*.cpp\` covering: pack-load round-trip, FMA precision vs scalar, blend correctness.
6. **Bench** — basic bench under \`tests/bench/simd_*.cpp\` measuring add/mul/fma throughput.
EOF
)"

# ─── Lane 04 / jobs ──────────────────────────────────────────────────────
issue "Lane 04 / jobs: Chase-Lev work-stealing deque + job graph + parallel_for" "$(cat <<EOF
**Lane:** 04-jobs · **Branch:** \`lane/04-jobs\` · **Owned paths:** \`engine/jobs/\`
**Files NOT touched:** anything outside \`engine/jobs/\`.

$BAR

## Wave-A deliverables
1. **JobSystem.{h,cpp}** — replace the synchronous stub with a real lock-free work-stealing scheduler:
   - Chase-Lev deque per worker (pick a known-good public-domain reference impl)
   - One worker per physical core (\`psynder::hardware::detect().cores_physical\`)
   - Main thread reserved for OS/input
   - Job submission wait-free; work-stealing handles imbalance
2. **Job graph** — declarative dependency support via \`submit(desc, dep)\`. Walk the DAG, dispatch ready nodes. Reuse node memory across frames (DESIGN.md §2.4).
3. **parallel_for** — split [begin, end) into chunks of \`grain\`; run on the pool; sync return. Backbone for tile raster, vertex transform, narrowphase batches.
4. **Per-worker scratch hookup** — at worker init, call the core lane's \`mem::worker_scratch_init(worker_id)\` (declare the hook in \`Allocator.h\`; lane 01 implements).
5. **Tests** — \`tests/unit/jobs_*.cpp\` covering: parallel_for sum correctness, dep order, no leak across frames.
6. Fibers deferred to Wave B unless time permits — explicitly note in PR.
EOF
)"

# ─── Lane 05 / asset ─────────────────────────────────────────────────────
issue "Lane 05 / asset: VFS + .lmpak archive reader + hot-reload watcher" "$(cat <<EOF
**Lane:** 05-asset · **Branch:** \`lane/05-asset\` · **Owned paths:** \`engine/asset/\`
**Files NOT touched:** anything outside \`engine/asset/\`.

$BAR

## Wave-A deliverables
1. **Vfs.cpp** — implement \`mount_pak\`, \`mount_directory\`, sync \`read\`. Mount order = search order; later mounts shadow earlier ones.
2. **.lmpak format** — FNV-1a-indexed archive, optional zstd compression (DESIGN.md §10.7). Define a v1 header in \`LmpakFormat.h\`, write a reader that mmaps the archive and serves \`Blob\` views into it (no copy).
3. **Cooker format defs** — \`.lmm\` mesh, \`.lmt\` texture (paletted/16/32-bit + mipchain), \`.lma\` audio — header structs only in \`engine/asset/Formats.h\`. Actual cookers live in lane 24 (tools/).
4. **Async loader** — \`read_async\` submits a job to the worker pool; on_loaded fires on a worker. zstd decompress on the same job.
5. **Hot-reload watcher** — dev builds only. Platform-specific stat polling or kqueue/inotify; fire \`on_changed\` between frames.
6. **Tests** — \`tests/unit/asset_*.cpp\` covering: mount + read round-trip, .lmpak format roundtrip with a synthetic archive.
EOF
)"

# ─── Lane 06 / scene ─────────────────────────────────────────────────────
issue "Lane 06 / scene: archetype-chunked ECS + queries + structural batching" "$(cat <<EOF
**Lane:** 06-scene · **Branch:** \`lane/06-scene\` · **Owned paths:** \`engine/scene/\`
**Files NOT touched:** anything outside \`engine/scene/\`.

$BAR

## Wave-A deliverables
1. **World.{h,cpp}** — replace the stub with the real archetype-chunked ECS (DESIGN.md §3.5, §4.4):
   - 16 KB chunks, page-aligned, allocated from \`TypedPool<Chunk>\`
   - SoA columns per component, 64-byte aligned within the chunk
   - Chunk header: archetype id, row count, version stamps, dirty mask
   - Iteration column-at-a-time (prefetcher-friendly)
2. **Component registry** — \`PSYNDER_COMPONENT(Name)\` macro registers POD types at static init. Assert \`is_trivially_copyable\` at compile time. Lane 06 provides the registry; the macro is already in the public header.
3. **Queries** — \`world.query<reads<A>, writes<B>>([](auto a, auto b){ ... })\`. Cache matched archetype list; only re-walk on structural change. Iteration parallelizes across chunks via lane 04's \`parallel_for\`.
4. **Structural change batching** — defer add/remove component to frame boundaries by default. Provide \`apply_structural_changes()\` invoked by the engine main loop.
5. **Spatial index hooks** — leave the SAP / BVH / hashed-grid impls to Wave B; declare the routing API surface in an internal header for now.
6. **Tests** — \`tests/unit/scene_*.cpp\` covering: create / destroy / alive, add/remove component, query iteration over a few hundred entities.
EOF
)"

# ─── Lane 07 / render-raster ─────────────────────────────────────────────
issue "Lane 07 / render-raster: tiled rasterizer (Q24.8 edges + perspective-correct + bilinear)" "$(cat <<EOF
**Lane:** 07-render-raster · **Branch:** \`lane/07-render-raster\` · **Owned paths:** \`engine/render/raster/\`, \`engine/render/Framebuffer.h\` (for adding helpers, not breaking the layout)
**Files NOT touched:** anything outside \`engine/render/raster/\` (with the explicit Framebuffer exception above for additive-only changes).

$BAR

## Wave-A deliverables
1. **Tiled sort-middle rasterizer** (DESIGN.md §7):
   - Vertex transform in SIMD lanes (use lane 03's f32x4/x8)
   - Triangle setup with Q24.8 edge equations
   - Tile binner @ 64×64 default; templated over \`<TILE_W,TILE_H>\` with 32/64/128 specializations (ADR-002)
   - Per-tile coverage walk; 2×2 quad rasterizer for free LOD finite diffs
   - 24-bit float Z + 8-bit stencil; early-Z when alpha test is off
   - Perspective-correct attribute interpolation (1/w, u/w, v/w then divide at quad)
2. **Bilinear texture sampler** — minimum for M1. Trilinear / aniso can wait for Wave B.
3. **DrawItem submission** — wire to lane 06's ECS-driven scene walk: query world for visible meshes, build DrawItems into a per-frame arena.
4. **Affine mode opt-in** — cvar \`r_affine\` controls perspective-correct vs affine (retro look).
5. **Sample 01 / triangle** — wire so a rotating textured triangle renders into the framebuffer; lane 25 owns the sample's main.cpp but you provide the test mesh helper.
6. **Tests** — \`tests/unit/render_raster_*.cpp\` covering: edge-equation sign correctness, perspective-correct vs affine difference, single-tile coverage bitmap.
7. **Bench** — per-tile raster bench under \`tests/bench/raster_*.cpp\`. CI bench gate (>2% regression must justify).
EOF
)"

# ─── Lane 08 / render-rt ─────────────────────────────────────────────────
issue "Lane 08 / render-rt: BVH8 builder (SAH) + 8-wide packet traversal skeleton" "$(cat <<EOF
**Lane:** 08-render-rt · **Branch:** \`lane/08-render-rt\` · **Owned paths:** \`engine/render/rt/\`
**Files NOT touched:** anything outside \`engine/render/rt/\`.

$BAR

## Wave-A deliverables
1. **Bvh8 builder** — SAH heuristic top-down build (DESIGN.md §8.2). Phase-0 stub is empty; Wave A lands the real build. Reference algorithm: Wald + pbrt-v4. Borrow only the SAH skeleton from dmonte (which is built on Embree, so the actual build code is new).
2. **8-wide packet traversal** — AVX2 path; NEON falls back to two 4-wide packets. Phase-0 stub uses no SIMD. Wave A implements the AVX2 path; NEON-real can slip to Wave B with a stub note in the PR.
3. **TLAS over instances** — \`Tlas::build(InstanceDesc*, count)\`; per-instance transform. Refit on dynamic instances.
4. **Refit** — for animated meshes, walk BLAS bottom-up. Measure refit SAH cost vs as-built; trigger async rebuild at 1.3× per DESIGN.md §9.4.
5. **Shadow ray packets** — \`trace_shadow_packet(tlas, pkt)\` driver. Used by the dynamic-light hybrid pass.
6. **Tests** — \`tests/unit/render_rt_*.cpp\` covering: single-triangle hit, BVH visits N nodes for a known scene, packet 8 hits match scalar.
7. **No Embree** — this lane explicitly ships from scratch per ADR-007. \`tools/dmonte_pt/\` is the only place Embree is allowed and it's opt-in.
EOF
)"

# ─── Lane 09 / render-post ───────────────────────────────────────────────
issue "Lane 09 / render-post: resolve + tonemap + dither + bloom + scanline filter" "$(cat <<EOF
**Lane:** 09-render-post · **Branch:** \`lane/09-render-post\` · **Owned paths:** \`engine/render/post/\`
**Files NOT touched:** anything outside \`engine/render/post/\`.

$BAR

## Wave-A deliverables
1. **resolve()** — HDR linear → LDR sRGB with Reinhard tonemap, optional dither (DESIGN.md §7.7). SIMD via lane 03.
2. **apply_bloom()** — separable Gaussian blur, 4-pass downsample-upsample. Operates on the HDR framebuffer before resolve.
3. **apply_scanline()** — retro scanline darkening, strength cvar.
4. **Dither** — ordered (Bayer) and blue-noise variants; selectable via cvar \`r_dither\`.
5. **Tests** — \`tests/unit/render_post_*.cpp\` covering: tonemap monotonic, scanline preserves average luminance, bloom kernel separability.
6. Streaming-store the LDR writes (\`_mm_stream_*\` / \`stnp\` on arm64) per DESIGN.md §4.3.
EOF
)"

# ─── Lane 10 / world-bsp ─────────────────────────────────────────────────
issue "Lane 10 / world-bsp: BSP loader + PVS bit-vector + leaf walk" "$(cat <<EOF
**Lane:** 10-world-bsp · **Branch:** \`lane/10-world-bsp\` · **Owned paths:** \`engine/world/bsp/\`
**Files NOT touched:** anything outside \`engine/world/bsp/\`.

$BAR

## Wave-A deliverables
1. **Bsp.h/cpp** — \`load()\` reads a .psybsp produced by lane 24's \`lm_qbsp\`. Header format: nodes / leaves / faces / PVS bit vectors per cluster.
2. **locate()** — point-in-tree walk down to leaf.
3. **walk_visible_leaves()** — given camera position, find its leaf, read PVS bit vector, emit visible leaves to a callback.
4. **Portal culling** — declare API; impl can slip to Wave B if time-pressed.
5. **Integration with the scene graph** — each BSP face becomes a \`DrawItem\` source consumed by lane 07. Wire a converter pass.
6. **Tests** — \`tests/unit/world_bsp_*.cpp\` with a synthetic 4-leaf BSP confirming PVS visibility.
EOF
)"

# ─── Lane 11 / world-outdoor ─────────────────────────────────────────────
issue "Lane 11 / world-outdoor: heightmap CDLOD mesh (backend A) + raymarch skeleton (backend B)" "$(cat <<EOF
**Lane:** 11-world-outdoor · **Branch:** \`lane/11-world-outdoor\` · **Owned paths:** \`engine/world/outdoor/\`
**Files NOT touched:** anything outside \`engine/world/outdoor/\`.

$BAR

## Wave-A deliverables
1. **Backend A — polygon CDLOD mesh** (DESIGN.md §9.2 + ADR-008):
   - 16-bit heightmap, chunked at 64×64
   - CDLOD seamless LOD
   - Material per-vertex (4-weight splatmap)
   - Emits DrawItems into lane 07's queue
2. **Backend B — heightmap raymarcher skeleton** — per-column ray-march declared, with scalar reference. SIMD (8 columns at once on AVX2) can slip to Wave B.
3. **Spline tracks** — cubic Bezier road segments → extruded textured strip; drivable surface flagged for physics.
4. **Scatter** — instanced billboards from density maps; deterministic placement for lockstep.
5. **Per-map backend config** — \`terrain_backend = mesh | raymarch\` fixed at map load.
6. **Tests** — \`tests/unit/world_outdoor_*.cpp\` covering: CDLOD stitching produces a watertight mesh, raymarch hits the heightmap surface correctly.
EOF
)"

# ─── Lane 12 / audio ─────────────────────────────────────────────────────
issue "Lane 12 / audio: 32-channel software mixer + platform backends" "$(cat <<EOF
**Lane:** 12-audio · **Branch:** \`lane/12-audio\` · **Owned paths:** \`engine/audio/\`
**Files NOT touched:** anything outside \`engine/audio/\`.

$BAR

## Wave-A deliverables
1. **Engine::start / stop** — open the chosen platform device (WASAPI / CoreAudio / PipeWire-ALSA), set up callback.
2. **32-channel mixer** — SIMD-merged voice sum (lane 03), one voice per job (lane 04 \`parallel_for\` over active voices).
3. **HRTF (minimal HRIR set)** for FPS; positional stereo+EQ for racing chase camera. HRIR set can be a vendored minimal IRC dataset.
4. **Reverb** — FDN algorithmic outdoors; FFT convolution indoor (FFT lib via FetchContent or simple hand-coded Cooley-Tukey).
5. **Platform glue** — header-only declarations of \`audio::backend_init_wasapi()\`, \`_coreaudio()\`, \`_pipewire()\`, \`_alsa()\`; the platform lanes (21/22/23) wire them up.
6. **Tests** — \`tests/unit/audio_*.cpp\` covering: voice acquire/release, mixer doesn't clip with N quiet voices, HRTF azimuth produces expected ITD.
EOF
)"

# ─── Lane 13 / physics ───────────────────────────────────────────────────
issue "Lane 13 / physics: rigid bodies + SAP + GJK/EPA + parallel island solver" "$(cat <<EOF
**Lane:** 13-physics · **Branch:** \`lane/13-physics\` · **Owned paths:** \`engine/physics/\`
**Files NOT touched:** anything outside \`engine/physics/\`.

$BAR

## Wave-A deliverables
1. **\`psynder_phys\` v0** (DESIGN.md §10.1):
   - Fixed 120 Hz tick; render interpolates between sim states
   - Rigid body integrator (forces → velocities → positions)
   - Sweep-and-prune broadphase, 3 axis-pass jobs in parallel
   - GJK + EPA narrowphase for convex pairs; special kernels for sphere-sphere, sphere-capsule, capsule-capsule, AABB-AABB. SIMD-vectorised (lane 03)
   - Island detection via union-find; one solver job per island; sequential-impulse / projected Gauss-Seidel inside
2. **Vehicle module skeleton** (\`vehicle::\`) — declare APIs only; full Pacejka + drivetrain slips to Wave B.
3. **Character controller** — capsule kinematic, sweep-step-slide algorithm with crouch + prone + ladder + water states.
4. **Determinism** — \`-fno-fast-math\` on physics TUs; pin rounding to round-to-nearest. Add a CMake property in this lane's CMakeLists.
5. **Tests** — \`tests/unit/physics_*.cpp\` covering: sphere-sphere collision energy conservation under bounded restitution, island isolation, deterministic replay.
6. **Bench** — \`tests/bench/physics_*.cpp\` measuring per-island solver cost (CI bench gate).
EOF
)"

# ─── Lane 14 / net ───────────────────────────────────────────────────────
issue "Lane 14 / net: reliable UDP transport + sliding window + selective ACK" "$(cat <<EOF
**Lane:** 14-net · **Branch:** \`lane/14-net\` · **Owned paths:** \`engine/net/\`
**Files NOT touched:** anything outside \`engine/net/\`.

$BAR

## Wave-A deliverables
1. **rUDP layer** — sequence numbers, sliding window, selective ACKs (DESIGN.md §10.4).
2. **Frame header** + cipher-suite ID slot reserved (not implemented Wave A; we just leave room).
3. **Lockstep** mode for racing (8 players, perfect determinism — depends on lane 13's deterministic physics).
4. **Snapshot interpolation** mode for FPS / tactical FPS (16-32 players, AOI filter for large maps).
5. **Local-loopback test harness** — two Hosts on the same process; exchange bytes deterministically.
6. **Tests** — \`tests/unit/net_*.cpp\` covering: reliable send with simulated 5% drop, ACK pipelining, AOI filter inclusion/exclusion at boundary.
EOF
)"

# ─── Lane 15 / script ────────────────────────────────────────────────────
issue "Lane 15 / script: Lua 5.4 binding + system/component registration + REPL" "$(cat <<EOF
**Lane:** 15-script · **Branch:** \`lane/15-script\` · **Owned paths:** \`engine/script/\`
**Files NOT touched:** anything outside \`engine/script/\`.

$BAR

## Wave-A deliverables
1. **Lua 5.4 wired** — via vcpkg or FetchContent (Lane owns the Dependencies tweak via a Dependencies.cmake comment if needed; do NOT edit root cmake/Dependencies.cmake — file an Issue instead).
2. **VM lifecycle** — \`Vm::start\` / \`shutdown\` / \`execute_string\` / \`execute_file\`.
3. **Engine-side binding** — expose \`World*\` query API, \`PSYNDER_COMPONENT(...)\` registration mirror for Lua, system registration with reads/writes:
   \`\`\`lua
   world:register_system({reads={'Position'}, writes={'Velocity'}},
                        function(positions, velocities) ... end)
   \`\`\`
4. **REPL** — \`Vm::execute_repl(line, out)\` returns formatted output. Wire to the editor IPC layer (lane 19's responsibility to hook).
5. **DOTS contract** — no \`entity:tick()\` style. Per DESIGN.md §3.3, users cannot write per-entity OOP code via the Lua API.
6. **Tests** — \`tests/unit/script_*.cpp\` covering: register Lua system, execute over a few entities, REPL roundtrip.
EOF
)"

# ─── Lane 16 / ui-imm ────────────────────────────────────────────────────
issue "Lane 16 / ui-imm: immediate-mode UI for in-viewport overlays" "$(cat <<EOF
**Lane:** 16-ui-imm · **Branch:** \`lane/16-ui-imm\` · **Owned paths:** \`engine/ui/imm/\`
**Files NOT touched:** anything outside \`engine/ui/imm/\`.

$BAR

## Wave-A deliverables
1. **Immediate-mode core** — minimal Dear-ImGui-style, our own impl (~few hundred LoC, DESIGN.md §10.6).
2. **Widget set (minimum):** \`button\`, \`label\`, \`line\`, \`rect_outline\`, \`filled_rect\`, \`graph\` (perf graph plot).
3. **Manipulator gizmo** skeleton — translate/rotate axes drawn into the framebuffer. Lane 18 (editor-core) wires actual entity transforms.
4. **Selection highlight + brush preview** primitives, used by the editor.
5. **No Chrome dependency** — this lane draws directly into the software framebuffer. Stays in-engine even when the editor is closed (for the perf HUD).
6. **Tests** — \`tests/unit/ui_imm_*.cpp\` covering: button hit-test, rect_outline pixel correctness.
EOF
)"

# ─── Lane 17 / ui-rml ────────────────────────────────────────────────────
issue "Lane 17 / ui-rml: RmlUi integration + FreeType glyphs + asset-VFS hot reload" "$(cat <<EOF
**Lane:** 17-ui-rml · **Branch:** \`lane/17-ui-rml\` · **Owned paths:** \`engine/ui/rml/\`
**Files NOT touched:** anything outside \`engine/ui/rml/\`.

$BAR

## Wave-A deliverables
1. **Vendor RmlUi** under \`third_party/rmlui/\` (submodule or FetchContent; pick what works locally).
2. **Vendor FreeType** under \`third_party/freetype/\` (or system find_package).
3. **RenderInterface impl** — RmlUi → lane 07's rasterizer. Triangles flow through the same path as game geometry.
4. **load_document / show / hide** — load \`.rml\` + \`.rcss\` via lane 05 VFS so they hot-reload between frames.
5. **Lua binding** for handlers via lane 15.
6. **Tests** — \`tests/unit/ui_rml_*.cpp\` covering: load a tiny .rml + .rcss, verify document parses.
EOF
)"

# ─── Lane 18 / editor-core ───────────────────────────────────────────────
issue "Lane 18 / editor-core: mode toggle + brush CSG + sculpt + physgun + constraints + undo" "$(cat <<EOF
**Lane:** 18-editor-core · **Branch:** \`lane/18-editor-core\` · **Owned paths:** \`engine/editor/core/\`
**Files NOT touched:** anything outside \`engine/editor/core/\`.

$BAR

## Wave-A deliverables
1. **Mode toggle** — \`~\` or F2 flips play ↔ edit; nothing reloads (DESIGN.md §10.8). Hooked into lane 21/22/23's input.
2. **Entity spawn / move / delete** via lane 06 ECS + lane 13 physics.
3. **Brush CSG** primitives (\`box\`, \`wedge\`, \`cylinder\`, \`prism\`); boolean combine, snap-to-grid. Output → BSP via lane 24's \`lm_qbsp\`.
4. **Heightmap sculpt** — raise / lower / smooth / flatten + 4-weight splat paint. Works for both terrain backends.
5. **Physgun** — pick body, drag through 3D space, freeze, rotate, scale, weld to another body.
6. **Constraints** — weld, axis, slider, ball-socket, rope, elastic.
7. **Undo / redo** — O(1) per step, delta-encoded.
8. **save/load** \`.psylevel\` (binary, zstd) + \`.psyc\` (contraption: entity graph + constraint set).
9. **Tests** — \`tests/unit/editor_core_*.cpp\` covering: undo of single move, brush CSG of two boxes producing the expected BSP.
EOF
)"

# ─── Lane 19 / editor-ipc ────────────────────────────────────────────────
issue "Lane 19 / editor-ipc: local HTTP + WebSocket server + msgpack protocol + IDL stubs" "$(cat <<EOF
**Lane:** 19-editor-ipc · **Branch:** \`lane/19-editor-ipc\` · **Owned paths:** \`engine/editor/ipc/\`
**Files NOT touched:** anything outside \`engine/editor/ipc/\`.

$BAR

## Wave-A deliverables
1. **WS server** at 127.0.0.1:7654 (pick a small C++20-capable WebSocket lib via FetchContent — uWebSockets or websocketpp; document choice in PR).
2. **HTTP** served on the same port for the React panel bootstrap.
3. **Session-token auth** via URL fragment; validated against the engine's per-startup token.
4. **msgpack frames** — pick a lib (\`msgpack-c\`); schema-versioned, defined in \`protocol.psy\` IDL.
5. **IDL stub generator** — small Python or C++ tool under this lane that emits C++ + TypeScript stubs from \`protocol.psy\`. Wire as a CMake custom-command so build-time changes regenerate.
6. **One-directional state push** — engine publishes deltas per-frame; panels subscribe to slices.
7. **Tests** — \`tests/unit/editor_ipc_*.cpp\` covering: msgpack encode/decode roundtrip for a sample component, session-token rejection of bad tokens.
EOF
)"

# ─── Lane 20 / editor-web ────────────────────────────────────────────────
issue "Lane 20 / editor-web: React + TypeScript editor panels (Inspector, Console, Profiler)" "$(cat <<EOF
**Lane:** 20-editor-web · **Branch:** \`lane/20-editor-web\` · **Owned paths:** \`engine/editor/web/\`
**Files NOT touched:** anything outside \`engine/editor/web/\`.

$BAR

## Wave-A deliverables
1. **Vite + React scaffold** (already in place) — install deps with \`pnpm i\` and verify \`pnpm dev\` boots the placeholder.
2. **Inspector panel** — connects to the WS at 127.0.0.1:7654, subscribes to the selected-entity component schemas, auto-renders form fields from \`PSYNDER_COMPONENT(...)\` schemas.
3. **Console panel** — bidirectional REPL hooked to lane 19 IPC.
4. **Profiler panel** — perf graph stream from the engine.
5. **Component-schema auto-binding** — define the JSON-Schema mapping rule; field types → form widgets (number → input[type=number], enum → select, bool → checkbox, color → color picker).
6. **Build artifact** — \`pnpm build\` produces \`dist/\`; the C++ build embeds it as static assets (lane 19 wires the embed via cmake/EmbedResource.cmake).
7. **No PSYNDER_EDITOR=0 in shipping** — gate the whole bundle behind the cmake option.
EOF
)"

# ─── Lane 21 / platform-win32 ────────────────────────────────────────────
issue "Lane 21 / platform-win32: Win32 window + DXGI flip-model present + WASAPI + XInput" "$(cat <<EOF
**Lane:** 21-platform-win32 · **Branch:** \`lane/21-platform-win32\` · **Owned paths:** \`engine/platform/win32/\`
**Files NOT touched:** anything outside \`engine/platform/win32/\`.

$BAR

**Important:** the orchestrator runs on macOS. PRs for this lane land with a "needs PC validation" note; the user verifies on their Windows box.

## Wave-A deliverables
1. **Win32 window** — CreateWindowEx, WM_PAINT, WM_CLOSE, WM_KEYDOWN/UP, WM_MOUSEMOVE, WM_INPUT (raw mouse).
2. **DXGI flip-model swap chain** — upload the CPU framebuffer to a fixed-size texture each frame; passthrough quad with bilinear / nearest / integer scale per DESIGN.md §11.1 + §7.9.
3. **WASAPI shared mode** — event-driven, hooks lane 12's mixer.
4. **XInput** for gamepads. Wheel force-feedback can wait for Wave B.
5. **Replace the Phase-0 stub** — sample_00_clear should open a real window with the animated clear color.
6. **Tests** — host-only; skip if not on Windows. \`tests/unit/platform_win32_*.cpp\` if any.
EOF
)"

# ─── Lane 22 / platform-linux ────────────────────────────────────────────
issue "Lane 22 / platform-linux: Wayland xdg-shell present + evdev + PipeWire/ALSA" "$(cat <<EOF
**Lane:** 22-platform-linux · **Branch:** \`lane/22-platform-linux\` · **Owned paths:** \`engine/platform/linux/\`
**Files NOT touched:** anything outside \`engine/platform/linux/\`.

$BAR

**Important:** the orchestrator runs on macOS. PRs for this lane land with a "needs Linux validation" note; the user verifies on their Linux box.

## Wave-A deliverables
1. **Wayland primary** — xdg-shell + \`wp_viewporter\` for the scaled present; dmabuf where the compositor supports it; otherwise wl_shm.
2. **X11 fallback** — XShm + X Render for the scaling blit.
3. **evdev** for keyboard / mouse / gamepad.
4. **PipeWire-first audio**, ALSA fallback.
5. **Replace the Phase-0 stub** — sample_00_clear should open a real window on Wayland with the animated clear color.
6. **Tests** — host-only.
EOF
)"

# ─── Lane 23 / platform-macos ────────────────────────────────────────────
issue "Lane 23 / platform-macos: AppKit window + CAMetalLayer scanout + CoreAudio + GameController" "$(cat <<EOF
**Lane:** 23-platform-macos · **Branch:** \`lane/23-platform-macos\` · **Owned paths:** \`engine/platform/macos/\`
**Files NOT touched:** anything outside \`engine/platform/macos/\`.

$BAR

**This is the orchestrator's host — the M0 demo must run end-to-end here.**

## Wave-A deliverables
1. **AppKit window** — NSWindow with NSView backed by CAMetalLayer (use \`MacPlatform.mm\` Objective-C++ — the .mm files are already accepted by the Lane cmake helper).
2. **CAMetalLayer scanout** — single passthrough textured quad per DESIGN.md §11.3 + §7.9 (Metal used ONLY for present, not for engine rendering). Upload the CPU framebuffer each frame via \`replaceRegion:\` or \`MTLBuffer\` + blit; bilinear / nearest / integer scale.
3. **GameController.framework** for gamepad. \`IOKit\` for keyboard / mouse.
4. **CoreAudio AUHAL** hookup for lane 12's mixer.
5. **HiDPI / Retina** — use physical pixel size for the present surface; framebuffer never changes (§7.9).
6. **Replace the Phase-0 stub** — \`sample_00_clear\` opens a real 1280×720 window, animates the clear color, runs until ESC or close-button.
7. **Tests** — \`tests/unit/platform_macos_*.cpp\` covering: Clock::ticks_per_second sane, executable_path returns absolute path.

Confidence-check: this is the lane whose Wave-A success unblocks the M0 demo verification. Aim for stability over feature breadth.
EOF
)"

# ─── Lane 24 / tools ─────────────────────────────────────────────────────
issue "Lane 24 / tools: lm_pak + lm_cook + lm_qbsp + lm_bake + lm_mapimport" "$(cat <<EOF
**Lane:** 24-tools · **Branch:** \`lane/24-tools\` · **Owned paths:** \`tools/\`
**Files NOT touched:** anything outside \`tools/\`.

$BAR

## Wave-A deliverables
1. **lm_pak** — pack a directory tree into a .lmpak archive, unpack the reverse. FNV-1a index. Optional zstd via lane 05's library helpers.
2. **lm_cook** — convert .obj / .gltf / .png / .wav → .lmm / .lmt (mipchain) / .lma. Vendor stb_image for PNG, dr_wav for WAV; pick a small obj parser or hand-roll. glTF can use cgltf vendored.
3. **lm_qbsp** — BSP compiler. id-inspired. Accept a brush-list .map; emit a .psybsp.
4. **lm_bake** — offline radiosity / path-traced lightmap baker. Direct lighting first; multi-bounce can slip to Wave B with a note. Uses lane 08 rt for the trace.
5. **lm_mapimport** — one-way TrenchBroom .map → .psylevel importer (DESIGN.md §10.8).
6. **CLI UX** — each tool prints a short \`--help\`; all read paths via lane 05 VFS (or stdin/stdout for streaming use).
7. **Tests** — \`tests/unit/tools_*.cpp\` smoke-call each tool's library entrypoint with synthetic input.
EOF
)"

# ─── Lane 25 / samples + tests ───────────────────────────────────────────
issue "Lane 25 / samples + tests: M0/M1 demos + bench + golden harness" "$(cat <<EOF
**Lane:** 25-samples-tests · **Branch:** \`lane/25-samples-tests\` · **Owned paths:** \`samples/\`, \`tests/\` (except per-lane unit-test files those lanes own)
**Files NOT touched:** anything outside \`samples/\` and \`tests/\` (and within tests/, do NOT touch \`tests/unit/<lane>_*.cpp\` files owned by other lanes).

$BAR

## Wave-A deliverables
1. **Sample 00** — already has \`main.cpp\`. Verify it builds + runs after lane 23 lands its real window impl. Add an \`--smoke-frames=N\` flag for headless CI.
2. **Sample 01 (M1 demo)** — \`main.cpp\` writes a rotating textured triangle. Pulls a tiny crate texture from a vendored asset under \`samples/01_triangle/assets/\`. Drives the rasterizer (lane 07).
3. **Sample 02 (M2 demo)** — \`main.cpp\` for the crate room (forward-looking — fine to land it returning early if lane 07 hasn't shipped full bilinear yet).
4. **Golden harness** — \`tests/golden/runner.cpp\` walks \`tests/golden/cases/*.psyscene\`, renders each, compares to PNG reference with 0.5% per-pixel tolerance. Ship 2-3 trivial reference images (clear color, single tri).
5. **Bench harness** — \`tests/bench/runner.cpp\` runs per-tile raster and per-island physics microbenchmarks; outputs JSON; CI parses for >2% regression vs baseline.
6. **CI integration** — \`.github/workflows/ci.yml\` is owned by orchestrator; if you need a workflow change, file an Issue. You CAN add the smoke-run step to your sample binaries' \`add_test()\` calls.
EOF
)"

echo "All 24 Wave-A Issues filed (#2..#25). Lane 01 was filed earlier as #1."
