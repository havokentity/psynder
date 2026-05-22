// SPDX-License-Identifier: MIT
// Psynder — lm_bake: offline lightmap baker (Lane 24 / tools).
//
// Wave-A scope (per Issue #24 and DESIGN.md §8.1):
//   - Direct lighting only (single hop from light to surface).
//   - Per-triangle planar atlas: each triangle gets a kxk texel grid in
//     its own surface-aligned UV chart. xatlas-style chart packing slips
//     to Wave-B; the grid layout still produces a valid streamable atlas.
//   - 16-bit half-float RGB output stored in an .lmlight blob.
//
// Wave-B scope (this revision):
//   - Multi-bounce path-traced diffuse indirect. Configurable via
//     `BakeOptions::max_indirect_bounces` (CLI `--bounces N`). Each bounce
//     gathers cosine-weighted hemisphere samples (deterministic stratified
//     pattern, no RNG state across runs) against the lane 08 BVH8 — the
//     adapter lives in `Bake.cpp` and falls back to brute-force when the
//     scene fits in a single triangle batch.
//   - Energy-conservative Lambertian: each BakeTriangle now carries an
//     albedo (defaults to 0.5 grey). Specular indirect slips to Wave-C.
//   - Direct-vs-indirect cross-bounce energy compare available via
//     `BakeOptions::max_indirect_bounces=0` (regression baseline) vs
//     `>0`.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::tools::bake {

inline constexpr u32 BakeMaterial_CastBakedShadow = 1u << 0;
inline constexpr u32 BakeMaterial_ReceiveBakedLight = 1u << 1;
inline constexpr u32 BakeMaterial_EmitBakedLight = 1u << 2;
inline constexpr u32 BakeMaterial_DefaultFlags =
    BakeMaterial_CastBakedShadow | BakeMaterial_ReceiveBakedLight;
inline constexpr u32 kInvalidBakeMaterialId = 0xFFFFFFFFu;

struct BakeMaterial {
    math::Vec3 albedo{0.5f, 0.5f, 0.5f};  // diffuse albedo in [0,1]
    math::Vec3 emissive_color{1.0f, 1.0f, 1.0f};
    f32 emissive = 0.0f;  // radiance scale when EmitBakedLight is set
    f32 roughness = 1.0f;
    u32 flags = BakeMaterial_DefaultFlags;
};

struct BakeTriangle {
    math::Vec3 v0;
    math::Vec3 v1;
    math::Vec3 v2;
    math::Vec3 normal;                            // shading normal (we'll renormalize)
    math::Vec3 albedo{0.5f, 0.5f, 0.5f};          // fallback when material_id is unresolved
    math::Vec3 emissive_color{1.0f, 1.0f, 1.0f};  // fallback when material_id is unresolved
    f32 emissive = 0.0f;                          // fallback radiance scale
    f32 roughness = 1.0f;
    u32 material_id = kInvalidBakeMaterialId;  // index into BakeScene::materials when present
    u32 material_flags = BakeMaterial_DefaultFlags;
};

enum class LightKind : u32 {
    kPoint = 0,
    kDirectional = 1,
};

struct BakeLight {
    LightKind kind = LightKind::kPoint;
    math::Vec3 position{0, 0, 0};    // point light: world pos
    math::Vec3 direction{0, -1, 0};  // directional light: direction TO the surface
    math::Vec3 color{1, 1, 1};       // linear RGB radiance multiplier
    f32 intensity = 1.0f;            // watts per steradian for points
    f32 radius = 0.0f;               // 0 = point; > 0 = soft (Wave-B uses)
};

struct BakeScene {
    std::vector<BakeMaterial> materials;
    std::vector<BakeTriangle> triangles;
    std::vector<BakeLight> lights;
};

struct BakeOptions {
    u32 lightmap_resolution = 8;           // texels per triangle edge
    u32 max_indirect_bounces = 0;          // 0 = direct only (Wave-A default; Wave-B accepts 2-4)
    u32 samples_per_texel = 1;             // Wave-A: 1 (jittered grid in Wave-B)
    u32 indirect_samples_per_bounce = 16;  // hemisphere gather samples per texel per bounce (Wave-B)
    f32 ray_epsilon = 1e-3f;               // shadow ray origin nudge
};

// One bake output per triangle. The kxk grid is laid out row-major in
// `pixels`; pixels are linear RGB float-32 triples.
struct BakedSurface {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> pixels;  // size = width*height*3
};

struct BakedAtlas {
    std::vector<BakedSurface> surfaces;  // one per triangle
};

// Bake the scene. The CPU path tracer used here is a small brute-force
// kernel; perf is fine for Wave-A unit tests (handful of triangles).
BakedAtlas bake(const BakeScene& scene, const BakeOptions& opt = {});

// Direct-light only entrypoint exposed for tests.
BakedSurface bake_triangle_direct(const BakeScene& scene, u32 triangle_index, const BakeOptions& opt);

// ─── .lmlight on-disk format ─────────────────────────────────────────────
//
//   char  magic[4]   = "LMLT"
//   u32   version    = 1
//   u32   surface_count
//   u32   reserved
//   ── per surface:
//        u32 width; u32 height
//        u16 half_rgb[width*height*3]   (IEEE 754 binary16)
//
inline constexpr u32 kLmlMagic = 0x544C4D4Cu;  // 'LMLT'
inline constexpr u32 kLmlVersion = 1u;

void write_lmlight(const BakedAtlas& atlas, std::vector<u8>& out);
bool read_lmlight(std::span<const u8> bytes, BakedAtlas& out, std::string* err = nullptr);

// Half-float helpers used by the .lmlight encoder.
u16 f32_to_f16(f32 v) noexcept;
f32 f16_to_f32(u16 h) noexcept;

int cli_main(int argc, char** argv);
void print_help();

}  // namespace psynder::tools::bake
