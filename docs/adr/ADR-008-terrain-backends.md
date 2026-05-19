# ADR-008: Two outdoor terrain backends — polygon CDLOD mesh + heightmap raymarcher

- **Status:** Accepted
- **Date:** 2026-05-19

## Context

Psynder targets two distinct outdoor game styles per the reference
games table in DESIGN.md §1:

- **NFS-style racing tracks** — detailed local geometry, spline-defined
  road centerlines with banking, sharp curbs, modeled rocks, asphalt
  detail textures. Camera tends to be low-altitude, close to the
  surface. Polygon CDLOD is the right fit.
- **Project IGI / Delta Force-style tactical FPS maps** — kilometre-
  scale view distances, sparse population, scattered structures
  (compounds, watchtowers, vehicles), draw-distance fog as a design
  choice. Long views over rolling hills with no detail meshes at
  distance. The 1999-era voxel-style heightmap renderers (NovaLogic's
  "Voxel Space" engine) handle this much better than CDLOD —
  per-column ray-marching has effectively-free view distance and no
  LOD seams.

A single backend can't optimise for both. CDLOD is wrong for tactical
FPS maps (chunk-management overhead grows with view distance); per-
column raymarch is wrong for racing tracks (no native support for
spline road detail or sharp curbs).

## Decision

**Both backends ship, selectable per-map.**

**Backend A — polygon CDLOD mesh:** 16-bit heightmap chunked at 64×64,
CDLOD seamless LOD, material per-vertex (4-weight splatmap). Default
for racing tracks. Integrates cleanly with spline road geometry and
detail textures.

**Backend B — heightmap raymarcher:** the same 16-bit heightmap, but
rendered by per-column ray-marching through the height field instead
of being tessellated into triangles. NovaLogic Voxel Space-inspired.
Each pixel column casts a ray, steps forward (logarithmic-distance
steps), looks up the heightmap at each step, and paints vertical
strips into the framebuffer wherever the height projects above the
column's running horizon. Default for tactical FPS maps.

**Composition.** Both backends write the **same tile framebuffer and
Z-buffer**. The Z-buffer is the glue:

```
For each tile (in parallel):
  1. Terrain pass — either:
       (a) CDLOD mesh: standard rasterizer, terrain triangles, depth-tested
       (b) Heightmap raymarch: per-column march, fills color + depth
  2. Polygon pass: rasterize meshes (soldiers, vehicles, buildings,
     props, debris) with depth-test — respects terrain Z values
  3. Hybrid shadow rays: BVH for polygon meshes; for backend (b) maps,
     a second shadow path raymarches the heightmap directly (cheaper)
  4. Resolve, post, present
```

Polygon meshes land on terrain seamlessly in both backends because they
share depth space.

**Per-map config.** `terrain_backend = mesh | raymarch` fixed at map
load. No mid-map switching — different LOD pipelines, different lightmap
layout.

## Consequences

- Lane 11 (`engine/world/outdoor/`) implements both backends.
- Lightmapping convention differs: CDLOD uses standard UV-mapped
  lightmap atlas; raymarch stores baked illumination as a parallel
  "light channel" of the colormap.
- Backend B limits: **no overhangs or caves** (single Y per (X, Z)).
  Cave / tunnel sections must be modeled as polygon meshes and
  composited via Z-buffer, or as BSP indoor segments transitioning to
  outdoor.
- Lane 08 (`engine/render/rt/`) implements the heightmap shadow ray
  path separately from BVH traversal.

## References

- DESIGN.md §9.2 (terrain backends), §8.2 (heightmap shadow path)
- NovaLogic "Voxel Space" engine — Comanche, Delta Force, Armored Fist
- CDLOD — Filip Strugar
