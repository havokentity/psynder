# Rendering — the tiled software rasterizer

Psynder's renderer is a **tiled sort-middle rasterizer with a separate
hybrid lighting stage**. It runs entirely on the CPU. The GPU is used
only as a passthrough scanout — DXGI flip-model blit on Windows,
Wayland viewporter / X Render on Linux, and a single CAMetalLayer
quad on macOS. No engine shader runs on the GPU.

This doc summarises the renderer. The authoritative reference is
[DESIGN.md §7](../DESIGN.md).

## Pipeline at a glance

```
For each frame:
  1. Cull (frustum + PVS indoor / tile-band outdoor)
  2. Vertex transform + light eval (SIMD lanes)
  3. Triangle setup — Q24.8 edge equations, gradients
  4. Bin: assign each tri to overlapping 64×64 tiles
  5. Per-tile rasterize (parallel): coverage + depth + attributes
  6. Per-tile shade: texture sample + lightmap + fog + fx
  7. Hybrid: trace dynamic shadow rays (BVH8 packets)
  8. Resolve: tile → backbuffer, tone map + dither
  9. Post: bloom, motion blur, volumetric fog, scanline filter
 10. Present (platform passthrough quad)
```

Steps 4-7 fuse into **one job per tile** so the framebuffer slice
stays hot in L2.

## Tile binning

Tiles default to **64×64** pixels (ADR-002). The rasterizer is
templated over `<TILE_W, TILE_H>` and ships three specializations —
32×32, 64×64, 128×128 — selectable at runtime via the `r_tile_size`
console variable. The compiler bakes tile dimensions as constants into
the inner loop, so changing tile size is **zero cost** at runtime once
the function pointer flips.

Mid-run changes use the same drain-and-reallocate pattern as
resolution / present-mode changes — one visible 30-50 ms hitch, then
the new tile pool is live. CI benchmarks all three specializations on
every commit.

Morton-ordered tile-to-worker assignment keeps neighbouring tiles on
neighbouring cores so the L2 line traffic is local.

## Triangle setup — Q24.8 fixed-point

Edge functions are evaluated in Q24.8 fixed-point (24 integer + 8
fractional bits). Sub-pixel precision is 1/256 px — enough to kill the
texture swimming common in old software renderers without inflating
the per-triangle setup cost.

A `DrawItem` carries 3 transformed vertices + a gradient pack +
material id + lightmap id + flags. It's exactly **128 bytes**, one
cache line aligned, so binning a triangle is one cache-line worth of
work.

## Attribute interpolation

Perspective-correct by default: 1/w, u/w, v/w are linearly interpolated,
then divided at each 2×2 quad. The 2×2 quad rasterizer gives mip LOD
finite-diffs for free.

`r_affine` opts into the retro affine path — same geometry, no division,
the warping that made software-era games look like software-era games.

## Texture sampling — full filter table

| Filter | Cost | Notes |
|---|---|---|
| Nearest (point) | 1 fetch | The retro path |
| Bilinear | 4 fetches + 3 lerps | Default for color/lightmap |
| Trilinear | 8 fetches | Across mip levels |
| Anisotropic 2×/4×/8×/16× | up to N × bilinear | EWA approximation |
| Bicubic | 16 fetches | UI / HUD upscale |

Mipmaps are generated offline by `lm_cook` (Kaiser filter). Textures
ship in `.lmt` — paletted (8 bpp + palette), 16-bit (RGB565 / RGBA4444),
or 32-bit (RGBA8) — with optional BC1/BC3-style block compression
decoded into a SLAB cache at load.

## Surface cache — automatic, per-surface, per-frame

Psynder's pixel-shading inner loop is **on-the-fly by default**:
sample base texture × sample lightmap × multiply. Plays cleanly with
every modern feature in the table above.

A second kernel — the classic Quake-style **surface cache** — is
applied **automatically per surface, per frame, when eligible**
(ADR-001). Eligibility:

- Lightmap filter mode is nearest
- No dynamic lights overlap the surface bounds
- No normal map / specular / env cubemap on the material
- Surface is at a stable mip level this frame
- Lightmap is LDR

When eligible, the surface's base × lightmap is pre-multiplied into a
single 4-8 MB LRU slab in the level-scope allocator. The pixel inner
loop samples once instead of twice. 4-frame hysteresis prevents
muzzle-flash thrash.

`r_force_shading_path` (dev cvar) overrides per-surface dispatch for
A/B profiling. Not exposed in shipping settings.

The retro look is reached by **turning off filtering and modern
features in settings** — the engine then auto-engages surface cache
for the eligible surfaces. Single lever, both ends.

## Z-buffer

24-bit float Z + 8-bit stencil interleaved, one `u32` per pixel.
Early-Z when alpha test is off. HiZ at 8×8 per tile rebuilt
incrementally from the depth slice; conservative max-Z early-reject
before the inner pixel loop.

## Hybrid lighting

Dynamic shadows trace **8 rays per packet** through a CPU BVH8
(AVX2 path; 4-wide NEON on Apple Silicon). See
[03-lighting.md](03-lighting.md) for the full lighting story.

## Performance budget (1080p60 desktop)

| Stage | Budget (ms) |
|---|---|
| Cull + setup | 1.5 |
| Vertex transform | 1.5 |
| Bin + raster + shade | 9 |
| Hybrid raytrace shadows | 3 |
| Resolve + post | 1 |
| Slack | 0.66 |
| **Total** | **16.66 (60 FPS)** |

Lower-end target: 720p60 internal with bilinear + baked-only lighting
on an M1 Air or a 4-core Ryzen 3.

## Resolution model — fixed internal, scaled present

Psynder renders at a **fixed internal resolution** chosen at startup
(`render_width`/`render_height` cvars or command-line). The CPU
framebuffer stays at that resolution for the life of the run. **Window
resizing — including maximize and fullscreen — never changes the
render resolution.** The platform present blits and scales to whatever
the window currently is. See DESIGN.md §7.9 for the why.

## Console cvars (renderer)

| Cvar | Default | Meaning |
|---|---|---|
| `r_tile_size` | 64 | 32 / 64 / 128 |
| `r_affine` | 0 | 1 = affine interpolation (retro) |
| `r_anisotropy` | 1 | 1 / 2 / 4 / 8 / 16 |
| `r_force_shading_path` | auto | dev only: `on_the_fly` / `surface_cached` |
| `r_motion_blur_strength` | 0 | 0 = off |
| `r_fog_enable` | 1 | volumetric fog |
| `r_fog_density` | 0.02 | |
| `r_rain_enable` | 0 | |
| `r_rain_intensity` | 0.5 | |
| `r_dither` | bayer | `off` / `bayer` / `blue_noise` |
| `r_scanline` | 0 | 1 = retro scanline filter |
| `r_scanline_strength` | 0.2 | |
| `r_debug_hud` | off | `off` / `compact` / `full` |
