# Getting started with Psynder

This guide gets you from a clean clone to running the M0-M6 demos in
under five minutes on Apple Silicon (and most of the way there on Win/
Linux). The engine has zero GPU code — every pixel is the CPU's problem
— so all you need is a modern compiler and a working window system.

## Prerequisites

| | Minimum | Notes |
|---|---|---|
| Compiler | Clang ≥ 17 (primary), GCC ≥ 13 (Linux), MSVC 19.40+ (Windows) | Apple Clang on macOS |
| CMake | ≥ 3.28 | |
| Ninja | latest | The presets all use Ninja |
| vcpkg | latest | Manifest mode; `vcpkg.json` is at repo root |
| Node + pnpm/npm | Node 20+, pnpm 8+ | **Only** if building the editor (`PSYNDER_EDITOR=ON`, default) |
| Chrome / Chromium | latest stable | **Only** if running editor panels (the engine launches `chrome --app=...`) |

Shipping a game **built on Psynder doesn't need any of the editor
prereqs** — only the engine + your game executable + assets. The
editor toolchain is dev-side.

## Clone + build

```bash
git clone https://github.com/havokentity/psynder.git
cd psynder

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

That builds **~160 targets** including 7 sample binaries, 5 offline
tools, the `psynder_unit` test runner, and all engine static libs.
First-time configure takes ~30 s as CMake fetches `fmt`, `Catch2`,
`Lua 5.4`, and `Tracy` via `FetchContent`.

## Run the samples

The seven sample binaries demonstrate one milestone each from
DESIGN.md §13:

```bash
build/mac-release/bin/sample_00_clear              # M0: animated clear
build/mac-release/bin/sample_01_triangle           # M1: textured triangle
build/mac-release/bin/sample_02_textured_quad      # M2: 4 spinning cubes
build/mac-release/bin/sample_03_quake_room         # M3: walking POV through BSP
build/mac-release/bin/sample_04_nfs_track          # M4 + M7: NFS-style lap with HUD
build/mac-release/bin/sample_05_hybrid_night       # M5: raytraced shadows
build/mac-release/bin/sample_06_tactical_map       # M6: heightmap flyover
```

Every sample supports `--smoke-frames=N` for headless CI (exits cleanly
after N frames) and `--smoke-capture-out PATH` to write the last frame
as a PNG (or PPM in some samples).

Press **ESC** to quit, or **`~` / F2** to toggle the editor mode
overlay (it draws a PLAY/EDIT badge bottom-right and freezes physics
in EDIT).

## Concurrent agents — smoke-test serialization

When multiple agents share one Mac, runs collide on the platform layer.
Acquire the smoke lock before invoking a sample binary:

```bash
LOCK=/tmp/psynder_smoke.lockdir
while ! mkdir "$LOCK" 2>/dev/null; do sleep 0.5; done
trap 'rmdir "$LOCK"' EXIT

build/mac-release/bin/sample_03_quake_room --smoke-frames=30
```

Concurrent **builds** (`cmake --build`) are fine — only serialize the
runtime invocation. See [AGENTS.md](../AGENTS.md) for the full
coordination protocol.

## Run the tests + bench

```bash
ctest --preset mac-release                    # ~600 unit + golden + bench tests
ctest --preset mac-release --output-on-failure
```

623+ Catch2 cases ship with the engine. Golden-image cells live under
`tests/golden/` with per-host PNG baselines (Darwin/Linux/Windows).
Bench gates live under `tests/bench/` — the CI per-tile raster + per-
island physics gates fail any PR with >2% regression.

## Editor panels (optional)

Build with `PSYNDER_EDITOR=ON` (the default). Start a sample, press
`~` to flip into edit mode, then open Chrome at
`http://127.0.0.1:7654/panels/inspector?token=<token>`. The session
token prints to stderr on engine start. Other panels:
`/panels/console`, `/panels/profiler`, `/panels/assets`,
`/panels/props`.

To skip the editor stack entirely for a shipping build, set
`PSYNDER_EDITOR=OFF` — the WebSocket server, IDL, and React panels
are fenced out, the binary shrinks, and no Chrome / Node dependency
remains.

## Next steps

- [02-rendering.md](02-rendering.md) — the tiled sort-middle rasterizer.
- [03-lighting.md](03-lighting.md) — baked lightmaps + raytraced
  dynamic shadows.
- [04-world-formats.md](04-world-formats.md) — `.lmpak`, `.psybsp`,
  `.lmm`, `.lmt`, `.lma` file formats.
- [adr/](adr/) — the seven decided architecture decisions.
- [waves-roadmap.md](waves-roadmap.md) — the parallel-agent wave model
  used to build the engine.

## Reporting issues

Use GitHub Issues at https://github.com/havokentity/psynder/issues.
For build problems, please include: OS + version, compiler + version
(`clang --version` / `cl /version`), CMake version, and the full
`cmake --preset ... 2>&1 | tail -40` output.
