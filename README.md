# Psynder

> *"Everything the GPU can do, the CPU can do — just lovingly, in cache, with SIMD."*

**Psynder** — **P**synder **C**ache **S**canline **R**enderer (PCSR) — is an open-source, pure-CPU game engine and software renderer. Tiled sort-middle rasterizer, software raytracer for dynamic shadows, late-90s software-era aesthetic pushed forward with modern CPU horsepower: wide SIMD, lock-free job graphs, cache-coherent DOTS, gigabytes of RAM, real raytraced lighting on a perspective-correct rasterizer.

There is **zero GPU code** in the engine runtime. The platform layer hands us a window and a framebuffer; everything else is the CPU's problem.

- **Language:** C++23
- **Targets:** Windows (x86-64), Linux (x86-64), macOS (Apple Silicon arm64)
- **License:** MIT
- **Status:** **v1.0.0** — DESIGN.md M0–M8 all covered. All 7 sample binaries smoke-pass on Mac. 632 Catch2 unit tests + golden + bench gates green. [Quick start](docs/01-getting-started.md). See [CHANGELOG](CHANGELOG.md) for the per-wave story.

## Design pillars

1. **Pure CPU.** No D3D / Vulkan / Metal / OpenGL in the engine runtime. The framebuffer is presented via the cheapest possible scaled blit.
2. **Hardcore performance.** Explicit SIMD (SSE4.2 / AVX2 / AVX-512 on x86-64, NEON on Apple Silicon), data-oriented design, lock-free job system, cache-line-aware data layouts.
3. **Authentic + modern.** Software-era look-and-feel as opt-ins (affine option, dithered fog, paletted textures) **plus** mipmapping, filtering, baked radiosity lightmaps, and raytraced dynamic shadows.
4. **Two genre presets, one engine.** Indoor FPS (Quake-style BSP+PVS) and outdoor heightmap (NFS-style racing tracks *and* Project IGI / Delta Force-style large tactical FPS).
5. **Hackable & teachable.** Every subsystem documented, every magic constant explained.
6. **Three platforms, first-class.** Apple Silicon NEON paths are hand-written, not an afterthought.

## What's working today (v0.1.0-wave-a)

- **M0 demo** — `sample_00_clear` opens a 1280×720 window on macOS with `CAMetalLayer`-backed CPU scanout, animates a sin-driven clear color (lane 23). Win32 + Linux platform skeletons in place (need PC/Linux validation).
- **All 25 subsystems scaffolded with real `.cpp` impls** (not stubs):
  - **core:** lock-free RCU log, page allocator with hugepage hints, full dmonte console port (cvar archive, undo/redo, favourites, history, smart-resolve, bracket-batch, per-value platform tags), CPU feature probe with cpuid + sysctl
  - **math:** vec/mat/quat/Q24.8, big-coord re-centering, stratified sampler, PCG RNG
  - **simd:** SSE4.2 / AVX2+FMA / NEON intrinsic wrappers with runtime dispatch, AVX-512 opportunistic, full op set (add/sub/mul/fma/cmp/blend/min/max/abs/rsqrt/sqrt/gather/load/store/broadcast/reduce/mask)
  - **jobs:** Chase-Lev work-stealing scheduler (one worker per physical core), job graph with deps, `parallel_for`
  - **asset:** mmap'd `.lmpak` reader with FNV-1a-64 index, optional zstd, async loader, hot-reload watcher, `.lmm/.lmt/.lma` cooker formats
  - **scene:** archetype-chunked ECS (16 KB chunks, SoA columns, structural-change batching, query iteration), component registration via `PSYNDER_COMPONENT(...)`
  - **render-raster:** tiled sort-middle rasterizer (Q24.8 edges, perspective-correct attrs, bilinear sampling, 32/64/128 tile specializations, `r_affine` opt-in, ~600 Mpix/s on M4 Max)
  - **render-rt:** BVH8 SAH builder + collapse-to-8, TLAS+BLAS, scalar + 8-wide AVX2 packet traversal, à-trous denoiser, refit + async-rebuild trigger (1.3× SAH threshold)
  - **render-post:** Reinhard tonemap, Bayer/blue-noise dither, separable Gaussian bloom, scanline filter, streaming stores
  - **world-bsp:** `.psybsp` loader, point-in-tree leaf walk, PVS bit-vec visibility emit, portal-clipping API
  - **world-outdoor:** heightmap loader, polygon-CDLOD watertight chunks, raymarcher scalar reference, spline tracks (Bezier), deterministic scatter
  - **audio:** 32-channel software mixer with SIMD voice sum, Woodworth-ITD HRTF, FDN reverb + FFT convolution reverb (Cooley-Tukey)
  - **physics (`psynder_phys`):** fixed 120 Hz tick with accumulator, symplectic Euler integrator, parallel SAP broadphase, GJK+EPA narrowphase + special closed-form kernels, union-find island detection, projected Gauss-Seidel solver, vehicle + character skeletons, deterministic FP (`-fno-fast-math` + `FpGuard`)
  - **net:** rUDP frame layer, sliding-window + selective-ACK reliability, lockstep + snapshot interpolation, AOI filter, in-process loopback bus
  - **script:** Lua 5.4 (FetchContent-built), sandboxed stdlib, DOTS-shaped binding (`world:register_system({reads=, writes=}, fn)` — no `entity:tick`), REPL
  - **ui-imm:** Dear-ImGui-style overlay widgets (button/label/line/rect/graph), perf graph + gizmos + brush preview + selection
  - **ui-rml:** in-tree RML/RCSS subset parser + cascade + layout + RenderInterface into the rasterizer (vendor RmlUi opt-in via `PSYNDER_VENDOR_RMLUI`)
  - **editor-core:** mode toggle, brush CSG (box/wedge/cylinder/prism) → BSP, heightmap sculpt (raise/lower/smooth/flatten + 4-weight splat), physgun, constraints (weld/axis/slider/ball-socket/rope/elastic), O(1) undo/redo, `.psylevel` + `.psyc` save/load
  - **editor-ipc:** hand-rolled WS+HTTP server bound to 127.0.0.1:7654, session-token auth, msgpack codec, RFC 6455 handshake, `protocol.psy` IDL with Python-generated C++ + TypeScript stubs
  - **editor-web:** Vite + React + TypeScript app — Inspector / Console / Profiler panels, mock-mode for offline dev, msgpack-over-WS client, schema → form-widget binding
  - **platform-macos:** AppKit window + `CAMetalLayer` single-quad scanout (Metal used ONLY for present, never engine rendering), CoreAudio AUHAL hook, GameController + IOKit input
  - **platform-win32:** Win32 + DXGI flip-model swap chain + WASAPI shared-mode + XInput (needs PC validation)
  - **platform-linux:** Wayland xdg-shell + viewporter + dmabuf detection + X11 fallback (XShm + XRender) + evdev + PipeWire/ALSA (needs Linux validation)
  - **tools:** `lm_pak` (archive packer), `lm_cook` (.obj/.gltf/.png/.wav → engine formats), `lm_qbsp` (BSP compiler), `lm_bake` (direct-lighting baker), `lm_mapimport` (TrenchBroom bridge)
  - **samples:** sample_00 (clear color, M0 verified), sample_01 (rotating textured triangle, M1 verified), sample_02-06 scaffolded
  - **tests:** ~350 Catch2 cases passing; golden-image harness with imgdiff (`tests/golden/`); per-tile + per-island microbenches (`tests/bench/`); CI bench gate enforces ≥2% regression must justify

Known wrinkles: the in-process IPC live-server WS test occasionally races on port acquisition under parallel ctest execution (test #75); a single retry passes. Tracked as a Wave B follow-up.

## Building

```bash
# Mac (Apple Silicon)
cmake --preset mac-release
cmake --build --preset mac-release

# Linux (x86-64)
cmake --preset linux-release
cmake --build --preset linux-release

# Windows (MSVC)
cmake --preset win-release
cmake --build --preset win-release
```

Prerequisites: Clang ≥ 17 (Apple Clang on macOS), CMake ≥ 3.28, Ninja, vcpkg. Editor mode additionally needs Node.js ≥ 20 + pnpm + Chrome/Chromium.

## Reading order for contributors

1. [DESIGN.md](DESIGN.md) §1 (Vision) — design pillars and non-goals.
2. [DESIGN.md](DESIGN.md) §3 (DOTS mandate) — the architectural contract.
3. [DESIGN.md](DESIGN.md) §16 (ADR log) — the seven decided + two N/A ADRs.
4. [DESIGN.md](DESIGN.md) §13 (Milestone roadmap) — implementation order.
5. [DESIGN.md](DESIGN.md) §19 (Implementation handoff) — what to read first, what to defer, hard rules.

## Repository layout

See [DESIGN.md](DESIGN.md) §12. Engine code lives under `engine/`; offline tools under `tools/`; samples under `samples/`; tests under `tests/`.

## Contributing

Each milestone is broken into 25 parallel ownership lanes. Each lane has a tracking Issue with strict file-ownership specs ("files OWNED" / "files NOT touched"). PRs target `main`, squash-merged.

See `docs/adr/` for architecture decision records; cross-system changes require an ADR.

## License

MIT. See [LICENSE](LICENSE). Authored by Rajesh D'Monte.

A handful of low-level kernels (PCG RNG, console code, log, hardware-detect, cmake helpers) are direct lifts from the author's prior project [dmonte path tracer](https://github.com/havokentity/demont-engine); the runtime raytracing core is independently written (no Embree dependency, see ADR-007 in DESIGN.md).
