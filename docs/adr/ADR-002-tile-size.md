# ADR-002: 64×64 is the default tile size; templated specializations runtime-changeable

- **Status:** Accepted
- **Date:** 2026-05-19

## Context

The tiled rasterizer needs to pick a tile size that balances:

- **Cache residency** — each tile's framebuffer slice + Z-buffer + draw
  command list should fit in L2 (and ideally L1 for the hottest portion)
- **Job-grain** — too-small tiles → scheduling overhead; too-large tiles
  → underutilised workers when only a few tiles cover the screen
- **HiZ coarseness** — HiZ at 8×8 per tile needs the tile size to be a
  multiple of 8

64×64 is the conventional sweet spot for 1080p on a modern desktop CPU.
32×32 wins on very-low-resolution retro presets; 128×128 may win on
Apple Silicon's larger L1 (128–192 KB on M-series P-cores).

## Decision

**64×64 is the shipped default everywhere.** The rasterizer is
**templated over `<TILE_W, TILE_H>`** and ships three specializations —
32×32, 64×64, 128×128. The compiler bakes tile dimensions as constants
into the inner loop, so there is zero runtime cost vs hard-coding.

Tile size is selectable via the `r_tile_size` console var. **Mid-run
changes use the same drain-and-reallocate machinery as a resolution
change**: drain in-flight frames, reallocate the tile pool from the
level-scope allocator, swap the specialization function pointer, resume.
One visible hitch, ~30–50 ms.

## Consequences

- CI benchmarks all three sizes on every commit. Per-platform defaults
  may diverge once we have data: Apple Silicon's larger L1 may justify
  128 there, but we ship 64 everywhere until measurements say otherwise.
- The bench gate (>2% per-tile cost regression must justify) applies
  to all three specializations.
- `r_tile_size` is exposed for power users + perf-investigation; it is
  **not** a shipping toggle for end-users (no menu entry).

## References

- DESIGN.md §7.3 (binning), §7.9 (resolution model)
- Larrabee papers — Abrash, Forsyth, Seiler (sort-middle tile
  rasterization on CPU)
- Fabian Giesen — Optimizing the basic rasterizer
