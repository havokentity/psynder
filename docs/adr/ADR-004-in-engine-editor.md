# ADR-004: Bespoke in-engine editor (Garry's Mod-style), no TrenchBroom dependency

- **Status:** Accepted
- **Date:** 2026-05-19

## Context

Two reasonable choices for level authoring + sandbox play:

1. **Vendor TrenchBroom** as the level editor; the engine just loads
   `.map` files at runtime. Lightweight to integrate.
2. **Bake a full editor into the engine** — Garry's Mod-style — that
   reuses the engine's renderer, physics, scripting, networking, and
   ECS. Press `~` mid-game to flip from "play" to "edit"; everything
   that worked in play continues to work in edit.

Option 1 means the editor lives outside the engine, with a serialization
boundary. The engine can't ship features like physgun, weld, runtime
constraint authoring, or multi-user co-op editing because those need
the live engine state.

Option 2 means more upfront work but unlocks the Garry's Mod-style
sandbox playground that drives a big chunk of the game's appeal.

## Decision

**Option 2.** Psynder ships a **bespoke in-engine editor** (`engine/editor/`)
layered on top of the same renderer / physics / scripting / networking
/ ECS that the game uses. Press `~` or F2 to flip the running session
from play to edit — nothing reloads.

Two intertwined use cases, one toolset:

1. **Sandbox / play-mode editing:** spawn props, grab them with a
   physgun, weld / constrain / unfreeze, glue rockets to crates, set up
   scenarios. Save as a contraption (`.psyc`).
2. **Level authoring:** brush-based CSG primitives sculpt indoor BSP
   geometry; terrain painting tools raise / lower / smooth / material-
   paint outdoor heightmaps (both backends); entity placement with a
   property inspector; light authoring; lightmap bake trigger. Saves
   to `.psylevel` (the same format the runtime loads).

Both modes share the same primitives. "Spawn a crate" in sandbox is the
same op as "place a crate prop" in authoring — only the surrounding UI
chrome and the physics-simulation-paused flag differ.

A `.map` (TrenchBroom format) **importer** ships as a courtesy one-way
bridge under `tools/lm_mapimport/`. It is **not** a dependency — just
a porting tool for users with existing Quake-style maps.

## Consequences

- Lane 18 (`engine/editor/core/`) implements the brush CSG, sculpt,
  physgun, constraints, undo, save/load.
- Lane 19 (`engine/editor/ipc/`) hosts the local WS + HTTP server at
  127.0.0.1:7654 for the React editor panels.
- Lane 20 (`engine/editor/web/`) is the React + TypeScript panel suite.
- Multi-user co-op editing falls out of using the engine's networking
  layer — multiple developers can edit the same level simultaneously,
  same way Gmod's multiplayer sandbox works.
- Editor-only logic is fenced behind `PSYNDER_EDITOR=1`. Retail builds
  with `PSYNDER_EDITOR=0` strip the editor cleanly.

## References

- DESIGN.md §10.8 (editor / sandbox mode), §10.6 (UI three-surface)
- Garry's Mod
