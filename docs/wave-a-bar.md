# Wave A — shared deliverable bar

Every lane PR opened against Wave A is graded by this rubric. Lane-specific
goals are in each Issue body; this doc is the shared expectations layer.

## Goal of Wave A

Drive Psynder from the bootstrap commit (frozen public APIs + scaffolded
build) toward **M0 + M1 working on Mac**, with every subsystem's surface
expanded enough that Wave B can land M2 (tiled rasterizer with bilinear +
mipmaps + Z) without further scaffolding work.

This means every lane MUST:

1. **Implement against the frozen public header**, not modify it. If your
   lane needs the public header changed, file an Issue against the
   orchestrator (don't change it yourself).
2. **Add real `.cpp` and internal headers under your owned directory.**
   Replace the auto-generated stub TU with real source files; the build
   helper will pick them up via the `*.cpp` glob.
3. **Land at least one unit test** covering the most-important invariant
   of your lane. The test goes under `tests/unit/<lane>_*.cpp` — that
   directory is shared but each file is yours.
4. **Keep the bench gate green.** If your change moves the per-tile raster
   or per-island physics bench by > 2%, justify it in the PR body.
5. **No `virtual` in `render/`, `scene/ecs/`, `physics/`, `audio/mixer/`
   hot paths.** No `shared_ptr` in `engine/` runtime. No per-frame
   `new`/`delete` outside `engine/core/alloc/`. (DESIGN.md §3.4 forbidden
   patterns — clang-tidy enforces.)
6. **Strict file ownership.** If your lane needs to touch a file outside
   your owned set, STOP and file an Issue. The orchestrator mediates.
7. **No `Co-Authored-By: AI tool` trailer** on commits. The repo's
   contributor list stays clean.

## Header contracts (read these before coding)

| Lane | Public header(s) you implement behind |
|---|---|
| 01-core | `engine/core/Types.h`, `Log.h`, `Diag.h`, `alloc/Allocator.h`, `console/Console.h`, `hardware/CpuFeatures.h` |
| 02-math | `engine/math/Math.h`, `Rng.h` |
| 03-simd | `engine/simd/Simd.h` |
| 04-jobs | `engine/jobs/JobSystem.h` |
| 05-asset | `engine/asset/Vfs.h` |
| 06-scene | `engine/scene/EcsRegistry.h` |
| 07-render-raster | `engine/render/raster/Raster.h`, `engine/render/Framebuffer.h` |
| 08-render-rt | `engine/render/rt/Bvh.h` |
| 09-render-post | `engine/render/post/Post.h` |
| 10-world-bsp | `engine/world/bsp/Bsp.h` |
| 11-world-outdoor | `engine/world/outdoor/Terrain.h` |
| 12-audio | `engine/audio/Audio.h` |
| 13-physics | `engine/physics/Physics.h` |
| 14-net | `engine/net/Net.h` |
| 15-script | `engine/script/Script.h` |
| 16-ui-imm | `engine/ui/imm/Imm.h` |
| 17-ui-rml | `engine/ui/rml/Rml.h` |
| 18-editor-core | `engine/editor/core/Editor.h` |
| 19-editor-ipc | `engine/editor/ipc/Ipc.h` |
| 20-editor-web | `engine/editor/web/` (Vite scaffold already in place) |
| 21-platform-win32 | `engine/platform/Platform.h` (Win32 impl) |
| 22-platform-linux | `engine/platform/Platform.h` (Linux impl) |
| 23-platform-macos | `engine/platform/Platform.h` (macOS impl) |
| 24-tools | `tools/lm_pak/`, `lm_cook/`, `lm_qbsp/`, `lm_bake/`, `lm_mapimport/` |
| 25-samples-tests | `samples/`, `tests/` |

## Branching + PR

- Branch name: `lane/NN-short-name` (matches `AGENTS.md` lane → directory map).
- PR title: short, references the lane Issue (e.g. "core: real LinearArena + log ring (#1)").
- PR body: explain the WHY, list any cross-lane public-header asks (which require pre-approval).
- Squash-merge into `main`.

## Definition of done for Wave A (per lane)

| Criterion | Required |
|---|---|
| Lane's subdirectory `CMakeLists.txt` reflects added sources | ✅ |
| Real `.cpp` files replacing the auto-stub | ✅ |
| At least one unit test under `tests/unit/<lane>_*.cpp` | ✅ |
| `cmake --build --preset mac-release` succeeds | ✅ |
| If the lane touches platform-specific code, builds on the host platform | ✅ |
| No public-header changes (file Issue if you need one) | ✅ |
| No `Co-Authored-By: AI tool` trailer | ✅ |
| PR opened against `main` | ✅ |

Mac smoke-test serialization (when running sample binaries):

```bash
LOCK=/tmp/psynder_smoke.lockdir
while ! mkdir "$LOCK" 2>/dev/null; do sleep 0.5; done
trap 'rmdir "$LOCK"' EXIT
build/mac-release/bin/sample_00_clear
```

## Build commands

```bash
# Mac
cmake --preset mac-release && cmake --build --preset mac-release

# Linux
cmake --preset linux-release && cmake --build --preset linux-release

# Windows
cmake --preset win-release && cmake --build --preset win-release
```

## What "stub" means in lane briefs

In each lane Issue, "stub" means the Phase-0 placeholder that the lane
agent replaces with real code. Functions like `Console::SaveArchivedCvars`
returning -1, `JobSystem::submit` running jobs synchronously, the
platform `Window` returning `should_close()=true` after one frame —
these are all stubs. Your job in Wave A is to turn them into the real
thing per DESIGN.md.
