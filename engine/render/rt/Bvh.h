// SPDX-License-Identifier: MIT
// Psynder — software raytracing public API. BVH8 builder + intersect kernel
// + refit + denoiser, all written fresh (no Embree dependency per ADR-007).
// Lane 08 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

namespace psynder::render::rt {

struct Ray {
    math::Vec3 origin;
    math::Vec3 direction;
    f32        t_min = 1e-4f;
    f32        t_max = 1e30f;
};

struct Hit {
    bool       hit       = false;
    f32        t         = 0;
    math::Vec3 normal;
    u32        primitive = 0;
    u32        instance  = 0;
};

struct Triangle {
    math::Vec3 v0, v1, v2;
};

class Bvh8 {
public:
    void build(const Triangle* tris, u32 count);
    void refit();                                   // for dynamic meshes
    Hit  intersect(const Ray& ray) const;
    bool occluded(const Ray& ray) const;            // shadow rays
    u32  node_count() const noexcept;
};

// Top-level acceleration structure: array of BLAS instances + transforms.
class Tlas {
public:
    struct InstanceDesc {
        const Bvh8* blas;
        math::Mat4  transform;
    };
    void build(const InstanceDesc* instances, u32 count);
    void refit();
    Hit  intersect(const Ray& ray) const;
    bool occluded(const Ray& ray) const;
};

// Shadow-ray packet driver (lane 08 implements 8-wide AVX2 + 4-wide NEON).
struct ShadowPacket8 {
    Ray  rays[8];
    bool occluded[8];
};
void trace_shadow_packet(const Tlas& tlas, ShadowPacket8& pkt);

// Denoiser — edge-aware à-trous, 2 passes guided by depth + normal.
struct DenoiseInput {
    const f32* shadow_visibility;   // width*height
    const f32* depth;
    const f32* normals;             // width*height*3
    u32        width;
    u32        height;
};
void denoise_shadows(const DenoiseInput& in, f32* output_visibility);

}  // namespace psynder::render::rt
