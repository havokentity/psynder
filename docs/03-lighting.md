# Lighting — hybrid baked + software raytraced

Psynder's lighting is **hybrid**: the static world gets path-traced
radiosity baked into lightmap atlases offline, and a tight real-time
software raytracer handles dynamic shadows and a few bounces during
runtime. No GPU is involved.

The runtime is **not a path tracer**. It traces a small, bounded
number of rays per pixel for shadows from dynamic lights. We can
afford that on a modern CPU. We absolutely cannot afford a full path
tracer per frame, which is why static lighting is baked.

This doc is a quick tour. The authoritative reference is
[DESIGN.md §8](../DESIGN.md).

## Baked radiosity — `lm_bake`

`tools/lm_bake/` is an offline path-traced lightmap baker.

```bash
build/mac-release/bin/lm_bake my_scene.psylevel --bounces 4 --samples 256 --out my_scene.lmlight
```

Algorithm:

1. **Atlas packing** — xatlas-style chart packing produces a unique
   UV layout per surface. Surface resolution scales with importance
   (large flat walls get more lumels; small props less).
2. **Direct lighting** from emissive surfaces and static lights
   (point, spot, directional, area).
3. **Indirect** — up to 4 cosine-weighted-hemisphere bounces gathered
   per lumel via energy-conservative recurrence
   `L_(k+1) = direct + albedo · mean(L_k)`. Deterministic per-texel
   jitter — no shared RNG state, perfectly reproducible.
4. **Output** — 16-bit half-float RGB lightmap; per-surface
   resolution; packed `.lmlight` file shipped alongside the BSP.

For raymarched heightmap terrain (DESIGN.md §9.2 backend B), there
are no UVs in the polygon-surface sense, so `lm_bake` writes a
**parallel "light channel"** of the colormap — each terrain texel
stores both color and baked illumination.

Multi-bounce was a Wave-B follow-up; Wave-A direct-only output is
byte-identical when `--bounces 0`.

## Runtime raytracing — `engine/render/rt/`

### BVH8

8-wide nodes. SAH-binned top-down build with 12 buckets, max leaf 4.
We collapse three binary levels into one wide node with SoA AABB
layout (six 8-float arrays — min_x, max_x, min_y, max_y, min_z,
max_z) so a single ray-vs-8-children slab test fits in a packed
`_mm256_*` pass per axis.

Build is fast enough to run at level load. **No Embree.** ADR-007.

### TLAS over instances

Each BLAS is a `Bvh8` over its mesh's triangles. The TLAS is a wide
BVH over instances, each instance carrying an affine transform.
Object-space ray dispatch (instance inverse transform applied to the
ray) keeps the BLAS in its build frame, no per-instance retransform
of triangles.

### Refit + async rebuild

Dynamic actors refit per frame — bottom-up walk, O(n) in node count.
We track refit cost vs as-built cost; when the ratio exceeds **1.3×**
we kick off an async rebuild on the worker pool. When ready, the new
tree atomically replaces the old. The frame using the stale tree
pays a small quality cost (looser bounds → more node tests), never
a correctness cost.

### Packet traversal

| Arch | Width | Path |
|---|---|---|
| AVX2 | 8-wide | `trace_shadow_packet` — coherent DFS, per-node 8-way slab test against all 8 packet rays, lanes terminate on any-hit |
| NEON | 4-wide | Two 4-wide halves over the same SoA layout (Wave B replaced the per-lane scalar fallback) |
| Fallback | scalar | Single-ray Möller-Trumbore + 8-way slab |

One shadow ray per pixel-per-active-light per frame. Pixels with no
nearby dynamic lights skip the trace entirely.

### Heightmap shadow path

For maps using the raymarched terrain backend, polygon meshes still
cast shadows via the BVH, but the terrain itself uses a **second
shadow path** — direct raymarch through the heightmap (uniform 2D
grid, cache-friendly, no tree traversal). Logarithmic-distance steps,
bilinear sub-cell sampling, Y-slab pre-cull. The two occlusion
results are MAX-combined per pixel.

### Soft shadows

PCSS-style: the distance to the occluder informs the filter radius.
We use a low-discrepancy 4-sample stratified pattern across frames
plus temporal accumulation. Cheap and stable.

### Denoiser

Edge-aware à-trous filter, 2 passes, guided by depth + normal. The
bilateral convex combination guarantees `filtered_visibility ≥
unfiltered_visibility` for in-shadow pixels (monotonicity), so we
never get spurious bright pixels in a fully-shadowed region.

## Dynamic light support

Up to **8 dynamic lights per pixel** via a **tiled light list**
(forward+ style, on CPU): per-64×64 tile holds a culled list of
light indices; the shading stage loops only over those.

| Light | Use case |
|---|---|
| Spot with cookie | Headlights, flashlights |
| Point | Brake lights (emissive + small point), muzzle flashes (2-frame), compound lighting |
| Directional | Sun, moon |
| Area (offline only) | Lightmap bake |

## Volumetric / atmospheric

- **Volumetric fog froxel grid** — 160×90×64 cells. CPU-side in-
  scatter computed per froxel from the active light list, ray-marched
  at resolve. ~1.5 ms cost. Optional shadowed in-scatter via a
  callback that binds to `Tlas::occluded` at the call site.
- **Rain** — instanced streaks as alpha-blended quads, lit by the
  dynamic-light list — gives NFS:HS its night-rain look. 4096-streak
  pool, gravity-driven sim.
- **Long-distance haze** for outdoor maps — cheap analytic
  Rayleigh-only approximation, no precomputed LUTs needed at our
  scale.

## What the `samples/05_hybrid_night/` demo exercises

The M5 demo wires the runtime raytracer end-to-end:

- Ground plane + 5 colored cubes in a TLAS (each cube is a BLAS over
  12 triangles)
- 3 orbiting RGB point lights at different radii / speeds
- Per pixel: primary-ray intersect → 8-wide `trace_shadow_packet`
  shadow rays per light → Lambert + inverse-square attenuation
- Quarter-resolution (256×144) traced, bilinear upsample to 512×288
- Dark-blue vertical-gradient skybox

It's the closest the engine gets to "look, ma, real raytracing on a
CPU."
