# ADR-009: Three-surface UI architecture — React + Chrome / immediate-mode / RmlUi

- **Status:** Accepted
- **Date:** 2026-05-19

## Context

Three distinct UI surfaces in the engine, each with different
requirements:

1. **Editor panels** — inspector, hierarchy, asset browser, console,
   profiler, REPL. Need: multi-monitor docking, real DOM/CSS, hot-reload
   at web speeds, designer-friendly forms, third-party widget libraries.
2. **In-viewport overlays** — perf graphs over the game, hitbox viz,
   manipulator gizmos, brush previews. Need: draw directly into the
   game framebuffer, compose with rendered scene, work on Wayland
   (where transparent OS overlay windows are fragile).
3. **Player HUD + menus** — health, ammo, minimap, main menu, options,
   mission briefings, scoreboards, cockpits. Need: ship in retail
   without dev dependencies (no Chrome required), designer-authored,
   hot-reload via asset VFS.

A single UI framework can't cover all three. Forcing them into one
framework means compromises in every dimension.

## Decision

**Three surfaces, three approaches:**

### Editor panels — React + TypeScript, in Chrome app windows

Each editor panel is its own React + TypeScript app running in a
separate Chrome app window (`chrome --app=http://127.0.0.1:7654/panels/<name>`).

- Multi-monitor docking native to the OS — drag panels across monitors
- Real Chrome DevTools for debugging
- Designer-friendly component libraries (shadcn/ui, Radix, TanStack
  Table)
- Hot-reload at web speeds via Vite HMR
- Cross-platform free

### In-viewport overlays — minimal immediate-mode UI (~few hundred LoC)

Dear-ImGui-style, our own implementation. Lives in `engine/ui/imm/`.
Draws directly into the game framebuffer. Used for:

- Perf graphs over the game
- Hitbox / wireframe / BVH viz
- Manipulator gizmos (translate / rotate / scale axes)
- Selection highlights
- Brush previews

This stays *small* — not a general-purpose UI replacement, just the
~20 widgets needed for in-viewport overlays.

### Player HUD + menus — RmlUi (HTML/CSS subset)

[RmlUi](https://github.com/mikke89/RmlUi) is MIT-licensed and a small,
self-contained HTML/CSS-subset renderer. Vendored under
`third_party/rmlui/`. FreeType for font glyph rasterization. Used for
player-facing UI:

- Health, ammo, minimap, scoreboards
- Main menu, options, mission briefings
- Cockpits, racing dashboards

Authored by designers in `.rml` + `.rcss`. Hot-reload via the asset
VFS. Lua bindings for handlers.

## Communication

- **Editor panels ↔ engine:** local-only HTTP + WebSocket server on
  `127.0.0.1:7654` (session-token gated). msgpack over WS.
- **Immediate-mode overlays:** in-process, read engine state directly
  each frame.
- **RmlUi:** ECS systems write a small "UI state" struct each frame; a
  binding adapter copies it into RmlUi's data model.

In all three cases, **data flow is one-directional** (engine → UI). No
UI code looks up game state ad-hoc. DOTS contract preserved.

## Consequences

- Lane 16 (`engine/ui/imm/`) writes the immediate-mode UI.
- Lane 17 (`engine/ui/rml/`) wires RmlUi + FreeType + `RenderInterface`.
- Lane 19 (`engine/editor/ipc/`) hosts the WS+HTTP server.
- Lane 20 (`engine/editor/web/`) is the React + TypeScript panel suite.
- Shipping games **don't need Chrome installed** — RmlUi handles all
  player UI.
- The editor mode requires Chrome / Chromium on PATH; the dev environment
  prereqs are documented in DESIGN.md §19.8.

## References

- DESIGN.md §10.6 (UI — three surfaces, three approaches), §10.8
  (editor)
- RmlUi — https://github.com/mikke89/RmlUi
- Dear ImGui — https://github.com/ocornut/imgui (reference / shape
  inspiration; we don't link it)
