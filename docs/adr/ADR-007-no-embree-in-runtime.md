# ADR-007: No Embree dependency in the engine runtime; roll our own BVH8

- **Status:** Accepted
- **Date:** 2026-05-19

## Context

[Intel Embree](https://www.embree.org/) is the gold standard for CPU
ray tracing. It ships SAH builders, packet traversal kernels (4-, 8-,
16-wide), refit, instancing — battle-tested over 15 years and used by
every major film renderer.

The author's prior project (dmonte path tracer) is built on Embree at
runtime. Reusing the same approach for Psynder's hybrid raytracing
(shadow rays + offline lightmap bake) would be tempting.

But Embree is a **bad fit for the Psynder engine runtime** for several
reasons:

1. **Build complexity.** Embree's CMake is opaque; pinning a version,
   vendoring, and getting it to link cleanly on three OSes (esp. Apple
   Silicon) is an ongoing pain.
2. **Weak Apple Silicon NEON support.** Embree's AVX-512 / AVX2 paths
   are excellent; its arm64 / NEON paths are markedly less optimised
   (and were absent for several Embree versions). Psynder treats Apple
   Silicon as first-class, not an afterthought.
3. **Incompatibility with our DOTS + own-allocator stance.** Embree
   manages its own memory internally (its own arena allocator); cross-
   referencing into our scene graph + own-allocator-tracked geometry
   needs awkward bridge code.
4. **Long binary size.** Embree adds ~5-10 MB to the engine binary just
   in static code; we want a tight binary.
5. **The hybrid raytracer is shadow-and-AO only**, not a full path
   tracer. We trace a small, bounded number of rays per pixel for
   dynamic shadows + AO. We don't need most of what Embree does
   (bidirectional path tracing primitives, photon mapping kernels,
   user-defined geometry callbacks, ray statistics, etc.).

## Decision

**No Embree anywhere in the engine runtime.** Psynder rolls its own
software raytracing core:

- BVH8 (8-wide) builder, SAH heuristic, top-down
- 8-wide packet traversal (AVX2 path; NEON falls back to 4-wide)
- Refit for animated meshes; SAH-cost-driven async rebuild (1.3×
  threshold) per DESIGN.md §9.4
- TLAS over instances
- Edge-aware à-trous denoiser, 2 passes guided by depth + normal
- Tile-pipeline integration: shadow rays fuse with shade in one
  per-tile job so the framebuffer slice stays in L2

All of the above is **fresh code**, written for real-time CPU use and
informed by the open literature (Wald, Havran, Hapala/Havran, pbrt-v4,
the original Larrabee papers).

**Exception:** the optional `tools/dmonte_pt/` reference path tracer
**retains** its Embree dependency. It is opt-in via
`PSYNDER_BUILD_DMONTE_PT=ON`, lives outside the engine library, and
exists only for golden-image cross-validation and offline-still
rendering. If `dmonte_pt` bit-rots we cut it without touching the
engine.

## Consequences

- Lane 08 (`engine/render/rt/`) writes the full BVH8 builder, intersect,
  refit, and denoiser. The shape of the API is borrowed from pbrt-v4
  conventions, not Embree.
- Performance vs Embree on x86-64 will be slightly behind in absolute
  numbers (Embree has 15 years of micro-optimisation we don't). The gap
  closes on Apple Silicon where Psynder's NEON paths win.
- Cross-platform debugging is simpler — when the BVH walk misbehaves,
  it's our code and our build, not a vendored opaque blob.

## References

- DESIGN.md §5 (relationship to dmonte), §8 (lighting), §16 ADR log
- Wald — "On fast Construction of SAH-based Bounding Volume Hierarchies"
  (2007)
- Wald, Boulos — "Ray Tracing Deformable Scenes using Dynamic Bounding
  Volume Hierarchies"
- pbrt-v4 — Physically Based Rendering (4th ed)
- Embree — https://www.embree.org/ (rejected for the engine runtime)
