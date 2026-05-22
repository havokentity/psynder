# Hybrid Material + Shadows Coordination

This is a living handoff note for the current hybrid renderer/material/shadow
architecture work. Keep it updated before spawning workers, after large edits,
and before ending a session so crash recovery has a concrete resume point.

## Current Branch

- Orchestrator branch: `codex/hybrid-material-shadows`
- Shared integration branch: `integration/wave-hybrid-material-shadows`
- Current base commit: `00bde99 render: add hybrid material shadow policy`
- Base intent: define the shared contracts for material policy, static/dynamic
  renderables, raster/RT/bake shadow queues, and first sample migration.
- Integration flow for future worker PRs: workers should target a shared
  `integration/wave-*` branch first. Test that integration branch, then merge
  the tested integration result to `main`.

## Update Cadence

- During active multi-agent work, this file should be updated after every
  meaningful state change and should not be stale for more than 60 seconds.
- User-facing progress updates should land roughly every 10-60 seconds while
  work is active.
- If workers are spawned, give each worker a disjoint branch/worktree and file
  ownership slice. The orchestrator updates this file from the main working
  branch as reports come in.
- One agent may be assigned as a scribe only if it owns this file exclusively.
  Otherwise, the orchestrator updates it directly to avoid merge churn.

## Active Agents

- Code-writing worktrees prepared from `origin/integration/wave-hybrid-material-shadows`:
  - `codex/agent-asset-async-texture`
    - agent: `019e4f6d-dbbb-7850-b66f-676828e5eac8`
    - `/Volumes/XTRM 5 Media/More MyRepos/Psynder.worktrees/agent-asset-async-texture`
    - owns `engine/asset/`
    - PR: https://github.com/havokentity/psynder/pull/136
    - status: complete; async texture/PPM/.lmt foundation; tested asset target/unit
  - `codex/agent-rt-material-bounces`
    - agent: `019e4f5e-2a6d-7130-aabc-560b806db969`
    - `/Volumes/XTRM 5 Media/More MyRepos/Psynder.worktrees/agent-rt-material-bounces`
    - owns `engine/render/rt/`
  - `codex/agent-raster-projected-shadows`
    - agent: `019e4f5e-4446-7c92-b0b5-bda7eeb67060`
    - `/Volumes/XTRM 5 Media/More MyRepos/Psynder.worktrees/agent-raster-projected-shadows`
    - owns `engine/render/raster/`
  - `codex/agent-lmbake-material-policy`
    - agent: `019e4f5e-5378-7773-8125-bc9f82121a1e`
    - `/Volumes/XTRM 5 Media/More MyRepos/Psynder.worktrees/agent-lmbake-material-policy`
    - owns `tools/`
  - `codex/agent-samples-hybrid-migration`
    - agent: `019e4f5e-63c9-7ba2-9016-75ac61719901`
    - `/Volumes/XTRM 5 Media/More MyRepos/Psynder.worktrees/agent-samples-hybrid-migration`
    - owns `samples/` and `tests/`
  - `codex/agent-scene-validation`
    - agent: `019e4f5e-10ad-73e0-8494-8ae43823cec7`
    - `/Volumes/XTRM 5 Media/More MyRepos/Psynder.worktrees/agent-scene-validation`
    - owns `engine/scene/`
    - PR: https://github.com/havokentity/psynder/pull/135
    - status: complete; commit `184cb06`; tested mac-debug scene/render-scene
- The earlier read-only scouts have completed:
  - scene/ECS layout
  - render/material queues
  - raster shadow/decal fit
  - RT/baker material policy
  - sample/test migration targets

## Monitor Log

- Current monitor pass: six workers dispatched; no worker completed in the
  first 60-second wait. Worktrees are present and clean from the orchestrator
  view.
- Second monitor pass: no completions yet, but all six workers have lane-local
  edits in their assigned worktrees. Ownership split is still clean.
- Scene worker complete with PR #135 targeting the integration branch.
- Asset worker complete with PR #136 targeting the integration branch.

## Next Multi-Agent Step

1. Current shared contract branch is committed and pushed.
2. `integration/wave-hybrid-material-shadows` is created and pushed from that
   contract commit.
3. Resume/reuse prior agents as code-writing workers where useful, each from a
   dedicated worktree and branch.
4. Worker PRs target `integration/wave-hybrid-material-shadows`, not `main`.
5. The integration branch gets tested as a whole before merging onward to
   `main`.

## Decisions Captured

- `SceneRenderer` / `HybridRenderer` is the common renderer path.
- Scene submission stays ECS/DOTS-oriented: renderables are ECS components,
  material identity is a handle, and queues hold compact item indices.
- Material policy is surface-level data. Object mobility is per renderable.
- Static objects may participate in baked lighting/shadows. Dynamic objects
  should default to runtime-only behavior; bake flags on dynamic objects are
  rejected from bake queues and surfaced as policy issues/warnings.
- Raster shadow support starts with a cheap projected/decal path: generate
  small geometry with precomputed UVs, then use multiplicative draw blending.
  No per-pixel material lookup or projector matrix work in the tile loop.
- Baked raster shadows should flow through bake/lightmap policy and the
  existing surface/lightmap payload path.
- RT shadow packets must respect material shadow policy.
- Material editor UI/WebSocket live editing is a later PR, but current material
  data should be editor-friendly.

## Implemented In This Branch

- `engine/render/Material.h`
  - Added `MaterialRasterShadowMode`.
  - Added `MaterialShadowAlphaMode`.
  - Added baked-lighting flags:
    - `Material_BakeVisible`
    - `Material_CastsBakedShadow`
    - `Material_ReceivesBakedShadow`
    - `Material_EmissiveBakes`
  - Added `Material_BakedLightingMask`.
  - Added SoA columns for raster shadow mode, shadow alpha, shadow opacity,
    and shadow softness.

- `engine/scene/SceneEcs.h`
  - Added `ObjectMobility { Static, Dynamic }`.
  - Added mobility to `RenderableComponent` and `SceneRenderItem`.
  - Hardened scene render gathering against parallel ECS query callbacks using
    thread-local chunk scratch plus a short append lock.

- `engine/render/SceneRenderer.h`
  - Added queues for:
    - `raster_shadow_casters`
    - `raster_shadow_receivers`
    - `bake_static`
    - `bake_shadow_casters`
    - `bake_shadow_receivers`
  - Added `dynamic_bake_rejected` stats.
  - Added policy issue collection and warning helpers for dynamic+bake misuse.
  - Added AlphaTest mapping into raster draw flags.

- `engine/render/raster/*`
  - Added `DrawBlendMode::Multiply`.
  - Added per-draw blend mode and opacity.
  - Tile raster path can multiplicatively darken existing framebuffer pixels
    without writing depth, suitable for projected blob/decal shadows.

- `engine/render/rt/*`
  - Added masked shadow-packet tracing by TLAS instance.
  - `FrameRenderer` builds a per-frame instance shadow mask from shared
    material IDs and `Material_CastsRtShadow`.

- `samples/05_rt_shadow_packets/main.cpp`
  - Migrated instance color policy from raw color arrays to `MaterialLibrary`
    plus `instance_materials`.

- `tests/`
  - Added static/dynamic bake queue tests.
  - Added material SoA coverage for new shadow policy columns.
  - Added raster multiplicative decal test.
  - Added RT material shadow packet test.
  - Added `sample_05_rt_shadow_packets_smoke` to CTest.

## Validation Already Run

- `cmake --build --preset mac-release`
  - Passed.
  - Existing warning noise remains from unrelated areas and linker duplicate /
    newer macOS library warnings.

- `build/mac-release/bin/psynder_unit`
  - Passed: 682 test cases, 248139 assertions.

- Manual smoke with runtime lock:
  - `build/mac-release/bin/sample_05_rt_shadow_packets --smoke-frames=3`
  - Passed.

- CTest smoke:
  - `ctest --test-dir build/mac-release -R sample_05_rt_shadow_packets_smoke --output-on-failure`
  - Passed.

- `git diff --check`
  - Passed.

## Known Follow-Ups

- Promote sample-local PPM/texture loading into engine asset/texture code.
- Make asset loaders async by design, and keep samples on async loading paths.
- Continue migrating samples to `SceneRenderer` / `HybridRenderer`.
- Split base texture policy from baked lightmap payload policy.
- Make the lightmap baker consume shared scene/material policy instead of
  sample-local bake structures.
- Add material editor/live-edit architecture in a later PR.
- Consider worker branches for disjoint sample migrations after this contract
  branch stabilizes.

## Current Modified Files

- `engine/render/Material.h`
- `engine/render/SceneRenderer.h`
- `engine/render/raster/Raster.cpp`
- `engine/render/raster/Raster.h`
- `engine/render/raster/TileBin.h`
- `engine/render/raster/TileRaster.cpp`
- `engine/render/rt/Bvh.h`
- `engine/render/rt/FrameRenderer.cpp`
- `engine/render/rt/FrameRenderer.h`
- `engine/render/rt/Intersect.cpp`
- `engine/scene/SceneEcs.h`
- `samples/05_rt_shadow_packets/main.cpp`
- `tests/CMakeLists.txt`
- `tests/unit/render_raster_tile.cpp`
- `tests/unit/render_rt_frame_helpers.cpp`
- `tests/unit/render_scene_renderer.cpp`
- `tests/unit/scene_render_submission.cpp`
- `docs/HYBRID_MATERIAL_SHADOWS_STATUS.md`
