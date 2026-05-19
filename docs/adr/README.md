# Architecture Decision Records

ADRs live in this directory, one file per decision (`ADR-NNN-slug.md`).
The DESIGN.md §16 ADR log is the source of truth at the time of v1.0
handoff; individual ADRs here expand on the reasoning for posterity and
host any subsequent supersession history.

## Status of each ADR (from DESIGN.md §16)

| ADR | Title | Status |
|---|---|---|
| ADR-001 | Pixel-shading kernel default + auto surface cache | ✅ Decided |
| ADR-002 | Tile size (64×64 default, templated specializations) | ✅ Decided |
| ADR-003 | Hybrid skinning (chassis skinned, wheels rigid) | ✅ Decided |
| ADR-004 | Bespoke in-engine editor (Garry's Mod-style) | ✅ Decided |
| ADR-005 | Open-world coords | ⛔ Not applicable (16 km × 16 km cap; float32 + re-centering) |
| ADR-006 | Planet tile data source | ⛔ Not applicable (flat heightmaps only) |
| ADR-007 | No Embree in engine runtime; roll our own BVH8 | ✅ Decided |
| ADR-008 | Two outdoor terrain backends (CDLOD mesh + raymarcher) | ✅ Decided |
| ADR-009 | Three-surface UI (React+Chrome, ImGui-style, RmlUi) | ✅ Decided |

Cross-system changes require a new ADR per DESIGN.md §15. New singletons
require an ADR per DESIGN.md §3.4. ADR file additions land via PR like
any other change.
