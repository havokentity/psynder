# ADR-001: On-the-fly is the default pixel-shading kernel; surface cache auto-engages

- **Status:** Accepted
- **Date:** 2026-05-19
- **Deciders:** Project owner (Rajesh D'Monte)

## Context

A textured + lightmapped surface rendered with the modern feature set
(filtering, dynamic lights, normal maps, env cubemaps, HDR lightmaps)
benefits from an **on-the-fly** pixel-shading kernel that samples each
input on every pixel. A surface rendered with the *retro* look (nearest-
neighbour filtering, no dynamic lights, no normal maps) benefits from a
**surface cache** that pre-multiplies base texture × lightmap into a
single combined chunk sampled once per pixel — the trick Quake II used.

Earlier drafts of DESIGN.md proposed exposing this as a user toggle
(`r_shading_kernel = on_the_fly | surface_cached`). That was rejected.

## Decision

**On-the-fly is the only default.** The renderer also ships a surface-
cache kernel but **auto-engages it per surface per frame** when all of
the following hold:

- Lightmap filter mode is nearest
- No dynamic lights overlap the surface bounds
- No normal map / specular / env cubemap on the surface's material
- Surface is at a stable mip level (no transition this frame)
- Lightmap is LDR

A surface that's been ineligible must be eligible for 4 consecutive
frames before re-classification (hysteresis), so a passing muzzle flash
doesn't thrash 200 cache entries.

A debug cvar `r_force_shading_path` is available for A/B comparison and
profiling but is **not** exposed in shipping settings.

## Consequences

- Retro look is reached by turning off filtering and modern features in
  settings — the engine auto-engages surface cache for the resulting
  eligible surfaces. **Single lever, both ends.** No separate "retro
  mode" pipeline; visual preset and pipeline path are the same dial,
  pulled from one end.
- Cache size (4–8 MB slab in the level-scope allocator) is bounded; LRU
  evicts when full.
- A `ShadingPath` tag (one byte on the draw command) selects the kernel
  per draw, never per pixel — SIMD lanes stay coherent within a tile.

## References

- DESIGN.md §7.6 (surface cache)
- Quake II software rendering — Michael Abrash, Graphics Programming
  Black Book
