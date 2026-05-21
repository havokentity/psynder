// SPDX-License-Identifier: MIT
// Psynder — Bvh8 / Tlas implementation state, internal to lane 08.
//
// The public Bvh.h declares Bvh8 and Tlas as empty classes (the public
// header is FROZEN per the Wave-A bar — we cannot add fields). To carry
// runtime state without breaking that contract, we keep state in static
// hash maps keyed by `this`. Build/refit paths may touch the maps; hot
// intersect/occluded paths use per-thread lookup caches and TLAS-cached BLAS
// state pointers so parallel ray traversal does not serialize on the registry.

#pragma once

#include "Bvh.h"
#include "Bvh_internal.h"

#include <vector>

namespace psynder::render::rt::detail {

// ─── BLAS state ──────────────────────────────────────────────────────────
struct Bvh8State {
    std::vector<Triangle> triangles;
    std::vector<u32> prim_indices;         // permuted leaf order
    std::vector<BinaryNode> binary_nodes;  // build-time tree, kept for refit
    std::vector<Bvh8Node> wide_nodes;      // 8-wide runtime tree
    f32 as_built_cost = 0.0f;
    f32 refit_cost = 0.0f;
};

// ─── TLAS state ──────────────────────────────────────────────────────────
struct TlasState {
    std::vector<Tlas::InstanceDesc> instances;
    std::vector<const Bvh8State*> blas_states;  // mirrors instances; cached at build/refit
    std::vector<Aabb> world_bounds;         // per-instance world AABB
    std::vector<math::Mat4> inv_transform;  // for object-space ray xform
    std::vector<u32> prim_indices;
    std::vector<BinaryNode> binary_nodes;
    std::vector<Bvh8Node> wide_nodes;
    f32 as_built_cost = 0.0f;
    f32 refit_cost = 0.0f;
};

// Defined in Bvh.cpp. Looks up (or lazily creates) state keyed by `this`.
Bvh8State& state_of(Bvh8& b) noexcept;
const Bvh8State& state_of(const Bvh8& b) noexcept;
TlasState& state_of(Tlas& t) noexcept;
const TlasState& state_of(const Tlas& t) noexcept;

// Heuristic: refit_cost > 1.3× as_built_cost → kick async rebuild (§9.4).
bool bvh_should_async_rebuild(const Bvh8State& s) noexcept;
bool tlas_should_async_rebuild(const TlasState& s) noexcept;

// ─── Helpers exposed for Tlas world-space xform of BLAS bounds ──────────
Aabb transform_aabb(const Aabb& b, const math::Mat4& m) noexcept;
math::Mat4 affine_inverse(const math::Mat4& m) noexcept;

// ─── Scalar intersect kernel for one ray vs a Bvh8State ─────────────────
// Tlas dispatches into per-instance object-space rays; the Bvh8 path stays
// scalar (correctness first); Intersect.cpp adds the AVX2 packet driver.
struct LocalHit {
    bool hit = false;
    f32 t = 0.0f;
    math::Vec3 normal{0, 0, 0};
    u32 primitive = 0;
};
LocalHit traverse_scalar(const Bvh8State& s, const Ray& ray) noexcept;
bool occluded_scalar(const Bvh8State& s, const Ray& ray) noexcept;
Hit traverse_tlas_scalar(const TlasState& s, const Ray& ray) noexcept;
bool occluded_tlas_scalar(const TlasState& s, const Ray& ray) noexcept;

}  // namespace psynder::render::rt::detail
