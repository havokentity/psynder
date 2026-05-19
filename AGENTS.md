# Psynder — agent coordination

When multiple agents work on Psynder in parallel (the standard mode of operation — see DESIGN.md §19 and the 25-lane carve-up), strict ownership prevents merge hell. This file is the load-bearing reference.

## File ownership per lane

Every parallel agent owns ONE directory plus its subdirectory `CMakeLists.txt`. **The agent never touches files outside its owned set.** Cross-cutting edits (top-level `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, root `cmake/*.cmake`, `.github/`, `.clang-*`, `LICENSE`, `README.md`, `DESIGN.md`) are reserved for the project's build-system owner and are not in any lane.

If a lane agent finds it needs a file outside its ownership, **STOP** and document in the PR body. The orchestrator mediates.

## Lane → directory map (Wave A)

| Lane | Directory | Branch |
|---|---|---|
| 01-core           | `engine/core/`            | `lane/01-core` |
| 02-math           | `engine/math/`            | `lane/02-math` |
| 03-simd           | `engine/simd/`            | `lane/03-simd` |
| 04-jobs           | `engine/jobs/`            | `lane/04-jobs` |
| 05-asset          | `engine/asset/`           | `lane/05-asset` |
| 06-scene          | `engine/scene/`           | `lane/06-scene` |
| 07-render-raster  | `engine/render/raster/`   | `lane/07-render-raster` |
| 08-render-rt      | `engine/render/rt/`       | `lane/08-render-rt` |
| 09-render-post    | `engine/render/post/`     | `lane/09-render-post` |
| 10-world-bsp      | `engine/world/bsp/`       | `lane/10-world-bsp` |
| 11-world-outdoor  | `engine/world/outdoor/`   | `lane/11-world-outdoor` |
| 12-audio          | `engine/audio/`           | `lane/12-audio` |
| 13-physics        | `engine/physics/`         | `lane/13-physics` |
| 14-net            | `engine/net/`             | `lane/14-net` |
| 15-script         | `engine/script/`          | `lane/15-script` |
| 16-ui-imm         | `engine/ui/imm/`          | `lane/16-ui-imm` |
| 17-ui-rml         | `engine/ui/rml/`          | `lane/17-ui-rml` |
| 18-editor-core    | `engine/editor/core/`     | `lane/18-editor-core` |
| 19-editor-ipc     | `engine/editor/ipc/`      | `lane/19-editor-ipc` |
| 20-editor-web     | `engine/editor/web/`      | `lane/20-editor-web` |
| 21-platform-win32 | `engine/platform/win32/`  | `lane/21-platform-win32` |
| 22-platform-linux | `engine/platform/linux/`  | `lane/22-platform-linux` |
| 23-platform-macos | `engine/platform/macos/`  | `lane/23-platform-macos` |
| 24-tools          | `tools/`                  | `lane/24-tools` |
| 25-samples-tests  | `samples/`, `tests/`      | `lane/25-samples-tests` |

## Commit-message rules

- Subject line: imperative, < 70 chars. Reference the lane Issue (e.g. "raster: tile-bin draw commands (#7)").
- Body: explain the *why*, not the *what*. Mention any cross-lane API changes (which require pre-approval).
- **Do NOT add `Co-Authored-By: Claude Opus 4.7 …` trailers** on Psynder commits. The contributor list stays clean.

## Mac smoke-test serialization

Multiple agents running the same Mac binary in parallel will saturate the platform layer (window, audio device, framebuffer present). Acquire an atomic lock via `mkdir` before any sample-binary or smoke invocation:

```bash
LOCK=/tmp/psynder_smoke.lockdir
while ! mkdir "$LOCK" 2>/dev/null; do sleep 0.5; done
trap 'rmdir "$LOCK"' EXIT

# Run sample / smoke
build/mac-release/bin/sample_00_clear --smoke-frames=10 || true
```

Concurrent BUILDS (`cmake --build`) are fine — only serialize the runtime invocation.

## CMake ownership

The top-level `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, and `cmake/*.cmake` are owned by the build-system maintainer (the orchestrator). Lane agents own *only their subdirectory's* `CMakeLists.txt`. If your lane needs the top-level build wiring changed (new option, new dep), file an Issue against the orchestrator rather than editing the root files yourself.

## Public-header contracts

Wave 0 froze the public API of every subsystem. Each `engine/<lane>/<Public*.h>` file is the **contract** other lanes code against. **Do not change public headers without filing an Issue against the orchestrator** — a public-header change cascades to every dependent lane and must be coordinated.

You may freely edit internal headers (anything `_internal.h`, `Impl/*.h`, or `.cpp` files in your lane).

## Integration branch

The orchestrator maintains `integration/wave-N` as a periodically-rebased branch carrying every in-flight lane PR, for live user testing. Lane PRs target `main`; the orchestrator merges your branch into the integration branch as you push. You don't need to interact with it.

## Mac vs Win/Linux validation

The orchestrator runs on macOS. PRs that touch Win32 or Linux paths land with a clear "needs PC validation" note rather than blocking on testing the agent can't perform. The user verifies their Windows / Linux boxes separately.
