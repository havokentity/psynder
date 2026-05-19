# Psynder

> *"Everything the GPU can do, the CPU can do — just lovingly, in cache, with SIMD."*

**Psynder** — **P**synder **C**ache **S**canline **R**enderer (PCSR) — is an open-source, pure-CPU game engine and software renderer. Tiled sort-middle rasterizer, software raytracer for dynamic shadows, late-90s software-era aesthetic pushed forward with modern CPU horsepower: wide SIMD, lock-free job graphs, cache-coherent DOTS, gigabytes of RAM, real raytraced lighting on a perspective-correct rasterizer.

There is **zero GPU code** in the engine runtime. The platform layer hands us a window and a framebuffer; everything else is the CPU's problem.

- **Language:** C++23
- **Targets:** Windows (x86-64), Linux (x86-64), macOS (Apple Silicon arm64)
- **License:** MIT
- **Status:** v0.x — early kickoff, see [DESIGN.md](DESIGN.md) for the full architectural mandate

## Design pillars

1. **Pure CPU.** No D3D / Vulkan / Metal / OpenGL in the engine runtime. The framebuffer is presented via the cheapest possible scaled blit.
2. **Hardcore performance.** Explicit SIMD (SSE4.2 / AVX2 / AVX-512 on x86-64, NEON on Apple Silicon), data-oriented design, lock-free job system, cache-line-aware data layouts.
3. **Authentic + modern.** Software-era look-and-feel as opt-ins (affine option, dithered fog, paletted textures) **plus** mipmapping, filtering, baked radiosity lightmaps, and raytraced dynamic shadows.
4. **Two genre presets, one engine.** Indoor FPS (Quake-style BSP+PVS) and outdoor heightmap (NFS-style racing tracks *and* Project IGI / Delta Force-style large tactical FPS).
5. **Hackable & teachable.** Every subsystem documented, every magic constant explained.
6. **Three platforms, first-class.** Apple Silicon NEON paths are hand-written, not an afterthought.

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
