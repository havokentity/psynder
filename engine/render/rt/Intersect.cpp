// SPDX-License-Identifier: MIT
// Psynder — ray / packet intersection kernels. Lane 08 owns.
//
// Three paths:
//   * Scalar traversal — single ray, walks the 8-wide BVH testing one
//     ray against eight child slabs per node. On AVX2 (x86-64) the slab
//     test is a true 8-way SIMD intersect: all eight child AABBs are
//     loaded from the SoA `Bvh8Node` and tested in parallel in one
//     batch (Hapala/Havran style). On arm64 NEON we do two 4-wide
//     batches. Otherwise scalar fallback.
//   * AVX2 8-wide packet — `trace_shadow_packet` driver. A coherent DFS:
//     for every visited node, all 8 rays test against each of the 8
//     children. At leaves, each primitive is tested per-lane via scalar
//     Möller–Trumbore.
//   * NEON 4-wide packet (arm64) — two 4-wide halves over the same 8-ray
//     packet. Real SIMD slab tests via `float32x4_t`.
//
// On platforms without AVX2 or NEON we fall back to scalar per lane.
//
// Triangle test: Möller–Trumbore, single-precision.
// Slab test: branchless safe-divide (no div-by-zero on axis-aligned rays).

#include "Bvh.h"
#include "Bvh_internal.h"
#include "Bvh_impl.h"

#include <cmath>
#include <limits>

#if defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#include <arm_neon.h>
#define PSYNDER_RT_HAVE_NEON 1
#else
#define PSYNDER_RT_HAVE_NEON 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PSYNDER_RT_PREFETCH_RO(ptr) __builtin_prefetch((ptr), 0, 1)
#else
#define PSYNDER_RT_PREFETCH_RO(ptr) ((void)0)
#endif

namespace psynder::render::rt::detail {

namespace {

// Möller–Trumbore single-ray vs triangle. Writes hit_t / normal on success.
PSY_FORCEINLINE
bool ray_triangle_mt(const Ray& r, const Triangle& tri, f32& hit_t, math::Vec3& hit_n) noexcept {
    const math::Vec3 e1 = math::sub(tri.v1, tri.v0);
    const math::Vec3 e2 = math::sub(tri.v2, tri.v0);
    const math::Vec3 pv = math::cross(r.direction, e2);
    const f32 det = math::dot(e1, pv);
    constexpr f32 kEps = 1e-8f;
    if (std::fabs(det) < kEps)
        return false;
    const f32 inv_det = 1.0f / det;

    const math::Vec3 tv = math::sub(r.origin, tri.v0);
    const f32 u = math::dot(tv, pv) * inv_det;
    if (u < 0.0f || u > 1.0f)
        return false;

    const math::Vec3 qv = math::cross(tv, e1);
    const f32 v = math::dot(r.direction, qv) * inv_det;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    const f32 t = math::dot(e2, qv) * inv_det;
    if (t < r.t_min || t > r.t_max)
        return false;

    hit_t = t;
    hit_n = math::normalize(math::cross(e1, e2));
    return true;
}

PSY_FORCEINLINE
bool ray_triangle_occluded(const Ray& r, const Triangle& tri) noexcept {
    f32 t;
    math::Vec3 n;
    return ray_triangle_mt(r, tri, t, n);
}

// Scalar slab test on one of the 8 child AABBs of a wide node. Returns
// `true` and writes `t_entry` if the ray enters the box within the current
// [t_min, t_max] window. Used by the portable fallback in
// `ray_vs_8_children_slab` when neither AVX2 nor NEON is available.
[[maybe_unused]] PSY_FORCEINLINE bool node_slab_test(const Bvh8Node& node,
                                                     u32 child,
                                                     math::Vec3 inv_dir,
                                                     math::Vec3 origin,
                                                     f32 t_min,
                                                     f32 t_max,
                                                     f32& t_entry) noexcept {
    const f32 tx1 = (node.min_x[child] - origin.x) * inv_dir.x;
    const f32 tx2 = (node.max_x[child] - origin.x) * inv_dir.x;
    f32 tmin = std::fmin(tx1, tx2);
    f32 tmax = std::fmax(tx1, tx2);

    const f32 ty1 = (node.min_y[child] - origin.y) * inv_dir.y;
    const f32 ty2 = (node.max_y[child] - origin.y) * inv_dir.y;
    tmin = std::fmax(tmin, std::fmin(ty1, ty2));
    tmax = std::fmin(tmax, std::fmax(ty1, ty2));

    const f32 tz1 = (node.min_z[child] - origin.z) * inv_dir.z;
    const f32 tz2 = (node.max_z[child] - origin.z) * inv_dir.z;
    tmin = std::fmax(tmin, std::fmin(tz1, tz2));
    tmax = std::fmin(tmax, std::fmax(tz1, tz2));

    if (tmax < std::fmax(tmin, t_min))
        return false;
    if (tmin > t_max)
        return false;
    t_entry = std::fmax(tmin, t_min);
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// Single-ray vs 8 children — true SIMD slab batch (Hapala/Havran).
//
// On AVX2 we load the 8-element SoA min/max arrays in one shot per axis
// and run the standard branchless slab test on all 8 children at once.
// Returns a bit mask: bit i set ⇔ child i is hit within [t_min, t_max].
// On NEON we do two 4-wide halves.
// ──────────────────────────────────────────────────────────────────────────

PSY_FORCEINLINE
u8 ray_vs_8_children_slab(
    const Bvh8Node& n, math::Vec3 inv_dir, math::Vec3 origin, f32 t_min, f32 t_max) noexcept {
#if defined(__AVX2__)
    const __m256 ox = _mm256_set1_ps(origin.x);
    const __m256 oy = _mm256_set1_ps(origin.y);
    const __m256 oz = _mm256_set1_ps(origin.z);
    const __m256 ix = _mm256_set1_ps(inv_dir.x);
    const __m256 iy = _mm256_set1_ps(inv_dir.y);
    const __m256 iz = _mm256_set1_ps(inv_dir.z);
    const __m256 vtmin_b = _mm256_set1_ps(t_min);
    const __m256 vtmax_b = _mm256_set1_ps(t_max);

    const __m256 mnx = _mm256_load_ps(n.min_x);
    const __m256 mny = _mm256_load_ps(n.min_y);
    const __m256 mnz = _mm256_load_ps(n.min_z);
    const __m256 mxx = _mm256_load_ps(n.max_x);
    const __m256 mxy = _mm256_load_ps(n.max_y);
    const __m256 mxz = _mm256_load_ps(n.max_z);

    const __m256 tx1 = _mm256_mul_ps(_mm256_sub_ps(mnx, ox), ix);
    const __m256 tx2 = _mm256_mul_ps(_mm256_sub_ps(mxx, ox), ix);
    const __m256 ty1 = _mm256_mul_ps(_mm256_sub_ps(mny, oy), iy);
    const __m256 ty2 = _mm256_mul_ps(_mm256_sub_ps(mxy, oy), iy);
    const __m256 tz1 = _mm256_mul_ps(_mm256_sub_ps(mnz, oz), iz);
    const __m256 tz2 = _mm256_mul_ps(_mm256_sub_ps(mxz, oz), iz);

    const __m256 tmin =
        _mm256_max_ps(vtmin_b,
                      _mm256_max_ps(_mm256_min_ps(tx1, tx2),
                                    _mm256_max_ps(_mm256_min_ps(ty1, ty2), _mm256_min_ps(tz1, tz2))));
    const __m256 tmax =
        _mm256_min_ps(vtmax_b,
                      _mm256_min_ps(_mm256_max_ps(tx1, tx2),
                                    _mm256_min_ps(_mm256_max_ps(ty1, ty2), _mm256_max_ps(tz1, tz2))));
    const __m256 ok = _mm256_cmp_ps(tmin, tmax, _CMP_LE_OQ);
    const u32 bits = static_cast<u32>(_mm256_movemask_ps(ok));
    return static_cast<u8>(bits & n.child_mask);
#elif PSYNDER_RT_HAVE_NEON
    // Two 4-wide halves over the 8 SoA children.
    const float32x4_t ox = vdupq_n_f32(origin.x);
    const float32x4_t oy = vdupq_n_f32(origin.y);
    const float32x4_t oz = vdupq_n_f32(origin.z);
    const float32x4_t ix = vdupq_n_f32(inv_dir.x);
    const float32x4_t iy = vdupq_n_f32(inv_dir.y);
    const float32x4_t iz = vdupq_n_f32(inv_dir.z);
    const float32x4_t vtmin_b = vdupq_n_f32(t_min);
    const float32x4_t vtmax_b = vdupq_n_f32(t_max);

    u8 out_mask = 0;
    for (u32 half = 0; half < 2; ++half) {
        const u32 off = half * 4;
        const float32x4_t mnx = vld1q_f32(&n.min_x[off]);
        const float32x4_t mny = vld1q_f32(&n.min_y[off]);
        const float32x4_t mnz = vld1q_f32(&n.min_z[off]);
        const float32x4_t mxx = vld1q_f32(&n.max_x[off]);
        const float32x4_t mxy = vld1q_f32(&n.max_y[off]);
        const float32x4_t mxz = vld1q_f32(&n.max_z[off]);

        const float32x4_t tx1 = vmulq_f32(vsubq_f32(mnx, ox), ix);
        const float32x4_t tx2 = vmulq_f32(vsubq_f32(mxx, ox), ix);
        const float32x4_t ty1 = vmulq_f32(vsubq_f32(mny, oy), iy);
        const float32x4_t ty2 = vmulq_f32(vsubq_f32(mxy, oy), iy);
        const float32x4_t tz1 = vmulq_f32(vsubq_f32(mnz, oz), iz);
        const float32x4_t tz2 = vmulq_f32(vsubq_f32(mxz, oz), iz);

        const float32x4_t tmin = vmaxq_f32(
            vtmin_b,
            vmaxq_f32(vminq_f32(tx1, tx2), vmaxq_f32(vminq_f32(ty1, ty2), vminq_f32(tz1, tz2))));
        const float32x4_t tmax = vminq_f32(
            vtmax_b,
            vminq_f32(vmaxq_f32(tx1, tx2), vminq_f32(vmaxq_f32(ty1, ty2), vmaxq_f32(tz1, tz2))));
        const uint32x4_t ok = vcleq_f32(tmin, tmax);
        // Pack the 4 lane bits into the upper / lower nibble.
        alignas(16) u32 lanes[4];
        vst1q_u32(lanes, ok);
        u8 nib = 0;
        for (u32 k = 0; k < 4; ++k)
            if (lanes[k])
                nib = static_cast<u8>(nib | (1u << k));
        out_mask = static_cast<u8>(out_mask | (nib << (off)));
    }
    return static_cast<u8>(out_mask & n.child_mask);
#else
    u8 mask = 0;
    for (u32 c = 0; c < 8; ++c) {
        if (n.child_kind[c] == 2)
            continue;
        f32 t_entry;
        if (node_slab_test(n, c, inv_dir, origin, t_min, t_max, t_entry)) {
            mask = static_cast<u8>(mask | (1u << c));
        }
    }
    return mask;
#endif
}

// Safe-divide reciprocal of the ray direction.
PSY_FORCEINLINE
math::Vec3 safe_inv(math::Vec3 d) noexcept {
    constexpr f32 kBig = 1e30f;
    auto inv = [](f32 c) { return (std::fabs(c) < 1e-20f) ? kBig : (1.0f / c); };
    return {inv(d.x), inv(d.y), inv(d.z)};
}

struct ChildHit {
    u32 child = 0;
    f32 t_entry = 0.0f;
};

PSY_FORCEINLINE
u32 sorted_child_hits(const Bvh8Node& n,
                      u8 mask,
                      math::Vec3 inv_dir,
                      math::Vec3 origin,
                      f32 t_min,
                      f32 t_max,
                      ChildHit (&hits)[8]) noexcept {
    u32 count = 0;
    for (u32 c = 0; c < 8; ++c) {
        if ((mask & (1u << c)) == 0)
            continue;
        f32 t_entry = 0.0f;
        if (!node_slab_test(n, c, inv_dir, origin, t_min, t_max, t_entry))
            continue;

        u32 pos = count++;
        while (pos > 0 && hits[pos - 1].t_entry > t_entry) {
            hits[pos] = hits[pos - 1];
            --pos;
        }
        hits[pos] = ChildHit{c, t_entry};
    }
    return count;
}

PSY_FORCEINLINE
Ray transform_ray(const Ray& ray, const math::Mat4& inv, f32 t_max) noexcept {
    const math::Vec4 o4{ray.origin.x, ray.origin.y, ray.origin.z, 1.0f};
    const math::Vec4 d4{ray.direction.x, ray.direction.y, ray.direction.z, 0.0f};
    const math::Vec4 oo = math::mul(inv, o4);
    const math::Vec4 dd = math::mul(inv, d4);
    Ray local;
    local.origin = {oo.x, oo.y, oo.z};
    local.direction = {dd.x, dd.y, dd.z};
    local.t_min = ray.t_min;
    local.t_max = t_max;
    return local;
}

PSY_FORCEINLINE
math::Vec3 transform_normal_to_world(math::Vec3 n, const math::Mat4& tr) noexcept {
    return math::normalize(math::Vec3{
        tr.m[0] * n.x + tr.m[4] * n.y + tr.m[8] * n.z,
        tr.m[1] * n.x + tr.m[5] * n.y + tr.m[9] * n.z,
        tr.m[2] * n.x + tr.m[6] * n.y + tr.m[10] * n.z,
    });
}

}  // anonymous namespace

// ────────────────────────────────────────────────────────────────────────
// Scalar traversal (single ray) — uses the SIMD 8-way slab test above.
// ────────────────────────────────────────────────────────────────────────

LocalHit traverse_scalar(const Bvh8State& s, const Ray& ray_in) noexcept {
    LocalHit out;
    out.hit = false;
    out.t = ray_in.t_max;

    if (s.wide_nodes.empty() || s.triangles.empty()) {
        return out;
    }

    Ray ray = ray_in;
    const math::Vec3 inv_dir = safe_inv(ray.direction);

    constexpr u32 kStackCap = 128;
    u32 stack[kStackCap];
    u32 top = 0;
    stack[top++] = 0;  // root

    while (top > 0) {
        const u32 nid = stack[--top];
        const Bvh8Node& n = s.wide_nodes[nid];
        const u8 mask = ray_vs_8_children_slab(n, inv_dir, ray.origin, ray.t_min, ray.t_max);
        if (mask == 0)
            continue;
        ChildHit child_hits[8];
        const u32 child_count =
            sorted_child_hits(n, mask, inv_dir, ray.origin, ray.t_min, ray.t_max, child_hits);
        for (u32 hc = 0; hc < child_count; ++hc) {
            const u32 c = child_hits[hc].child;
            const u8 kind = n.child_kind[c];
            if (kind == 1) {
                const u32 first = n.child_index[c];
                const u32 cnt = n.child_count[c];
                for (u32 i = 0; i < cnt; ++i) {
                    const u32 pid = s.prim_indices[first + i];
                    if (pid >= s.triangles.size())
                        continue;
                    f32 hit_t;
                    math::Vec3 hit_n;
                    if (ray_triangle_mt(ray, s.triangles[pid], hit_t, hit_n)) {
                        if (hit_t < ray.t_max) {
                            ray.t_max = hit_t;
                            out.hit = true;
                            out.t = hit_t;
                            out.normal = hit_n;
                            out.primitive = pid;
                        }
                    }
                }
            }
        }
        for (u32 hc = child_count; hc > 0; --hc) {
            const u32 c = child_hits[hc - 1].child;
            if (n.child_kind[c] == 0) {
                if (top < kStackCap) {
                    const u32 child_node = n.child_index[c];
                    PSYNDER_RT_PREFETCH_RO(&s.wide_nodes[child_node]);
                    stack[top++] = child_node;
                }
            }
        }
    }

    return out;
}

bool occluded_scalar(const Bvh8State& s, const Ray& ray_in) noexcept {
    if (s.wide_nodes.empty() || s.triangles.empty())
        return false;
    Ray ray = ray_in;
    const math::Vec3 inv_dir = safe_inv(ray.direction);

    constexpr u32 kStackCap = 128;
    u32 stack[kStackCap];
    u32 top = 0;
    stack[top++] = 0;

    while (top > 0) {
        const u32 nid = stack[--top];
        const Bvh8Node& n = s.wide_nodes[nid];
        const u8 mask = ray_vs_8_children_slab(n, inv_dir, ray.origin, ray.t_min, ray.t_max);
        if (mask == 0)
            continue;
        for (u32 c = 0; c < 8; ++c) {
            if ((mask & (1u << c)) == 0)
                continue;
            const u8 kind = n.child_kind[c];
            if (kind == 1) {
                const u32 first = n.child_index[c];
                const u32 cnt = n.child_count[c];
                for (u32 i = 0; i < cnt; ++i) {
                    const u32 pid = s.prim_indices[first + i];
                    if (pid >= s.triangles.size())
                        continue;
                    if (ray_triangle_occluded(ray, s.triangles[pid])) {
                        return true;
                    }
                }
            } else if (kind == 0) {
                if (top < kStackCap) {
                    const u32 child_node = n.child_index[c];
                    PSYNDER_RT_PREFETCH_RO(&s.wide_nodes[child_node]);
                    stack[top++] = child_node;
                }
            }
        }
    }
    return false;
}

Hit traverse_tlas_scalar(const TlasState& s, const Ray& ray_in) noexcept {
    Hit best;
    best.hit = false;
    best.t = ray_in.t_max;

    if (s.instances.empty() || s.wide_nodes.empty())
        return best;

    const math::Vec3 inv_dir = safe_inv(ray_in.direction);
    constexpr u32 kStackCap = 128;
    u32 stack[kStackCap];
    u32 top = 0;
    stack[top++] = 0;

    while (top > 0) {
        const u32 nid = stack[--top];
        const Bvh8Node& n = s.wide_nodes[nid];
        const u8 mask = ray_vs_8_children_slab(n, inv_dir, ray_in.origin, ray_in.t_min, best.t);
        if (mask == 0)
            continue;

        ChildHit child_hits[8];
        const u32 child_count =
            sorted_child_hits(n, mask, inv_dir, ray_in.origin, ray_in.t_min, best.t, child_hits);
        for (u32 hc = 0; hc < child_count; ++hc) {
            const u32 c = child_hits[hc].child;
            const u8 kind = n.child_kind[c];
            if (kind == 1) {
                const u32 first = n.child_index[c];
                const u32 cnt = n.child_count[c];
                for (u32 k = 0; k < cnt; ++k) {
                    const u32 inst_i = s.prim_indices[first + k];
                    if (inst_i >= s.instances.size())
                        continue;
                    const Bvh8State* bs =
                        inst_i < s.blas_states.size() ? s.blas_states[inst_i] : nullptr;
                    if (!bs)
                        continue;

                    const Ray local = transform_ray(ray_in, s.inv_transform[inst_i], best.t);
                    const LocalHit lh = traverse_scalar(*bs, local);
                    if (lh.hit && lh.t < best.t) {
                        best.hit = true;
                        best.t = lh.t;
                        best.primitive = lh.primitive;
                        best.instance = inst_i;
                        best.normal =
                            transform_normal_to_world(lh.normal, s.instances[inst_i].transform);
                    }
                }
            }
        }
        for (u32 hc = child_count; hc > 0; --hc) {
            const u32 c = child_hits[hc - 1].child;
            if (n.child_kind[c] == 0) {
                if (top < kStackCap) {
                    const u32 child_node = n.child_index[c];
                    PSYNDER_RT_PREFETCH_RO(&s.wide_nodes[child_node]);
                    stack[top++] = child_node;
                }
            }
        }
    }

    return best;
}

bool occluded_tlas_scalar(const TlasState& s, const Ray& ray_in) noexcept {
    if (s.instances.empty() || s.wide_nodes.empty())
        return false;

    const math::Vec3 inv_dir = safe_inv(ray_in.direction);
    constexpr u32 kStackCap = 128;
    u32 stack[kStackCap];
    u32 top = 0;
    stack[top++] = 0;

    while (top > 0) {
        const u32 nid = stack[--top];
        const Bvh8Node& n = s.wide_nodes[nid];
        const u8 mask = ray_vs_8_children_slab(n, inv_dir, ray_in.origin, ray_in.t_min, ray_in.t_max);
        if (mask == 0)
            continue;
        for (u32 c = 0; c < 8; ++c) {
            if ((mask & (1u << c)) == 0)
                continue;
            const u8 kind = n.child_kind[c];
            if (kind == 1) {
                const u32 first = n.child_index[c];
                const u32 cnt = n.child_count[c];
                for (u32 k = 0; k < cnt; ++k) {
                    const u32 inst_i = s.prim_indices[first + k];
                    if (inst_i >= s.instances.size())
                        continue;
                    const Bvh8State* bs =
                        inst_i < s.blas_states.size() ? s.blas_states[inst_i] : nullptr;
                    if (!bs)
                        continue;
                    const Ray local = transform_ray(ray_in, s.inv_transform[inst_i], ray_in.t_max);
                    if (occluded_scalar(*bs, local))
                        return true;
                }
            } else if (kind == 0) {
                if (top < kStackCap) {
                    const u32 child_node = n.child_index[c];
                    PSYNDER_RT_PREFETCH_RO(&s.wide_nodes[child_node]);
                    stack[top++] = child_node;
                }
            }
        }
    }
    return false;
}

}  // namespace psynder::render::rt::detail

// ────────────────────────────────────────────────────────────────────────
// AVX2 8-wide packet shadow driver — `trace_shadow_packet`
// ────────────────────────────────────────────────────────────────────────

namespace psynder::render::rt {

#if defined(__AVX2__)

namespace {

// AVX2 8-wide slab test: tests *one* child AABB against all 8 packet rays.
// Returns the bit mask of lanes that hit this child.
PSY_FORCEINLINE
u8 packet8_slab_one_child(f32 cmnx,
                          f32 cmny,
                          f32 cmnz,
                          f32 cmxx,
                          f32 cmxy,
                          f32 cmxz,
                          __m256 ox,
                          __m256 oy,
                          __m256 oz,
                          __m256 ix,
                          __m256 iy,
                          __m256 iz,
                          __m256 tmin_b,
                          __m256 tmax_b) noexcept {
    const __m256 bmx = _mm256_set1_ps(cmnx);
    const __m256 bmy = _mm256_set1_ps(cmny);
    const __m256 bmz = _mm256_set1_ps(cmnz);
    const __m256 bMx = _mm256_set1_ps(cmxx);
    const __m256 bMy = _mm256_set1_ps(cmxy);
    const __m256 bMz = _mm256_set1_ps(cmxz);

    const __m256 tx1 = _mm256_mul_ps(_mm256_sub_ps(bmx, ox), ix);
    const __m256 tx2 = _mm256_mul_ps(_mm256_sub_ps(bMx, ox), ix);
    const __m256 ty1 = _mm256_mul_ps(_mm256_sub_ps(bmy, oy), iy);
    const __m256 ty2 = _mm256_mul_ps(_mm256_sub_ps(bMy, oy), iy);
    const __m256 tz1 = _mm256_mul_ps(_mm256_sub_ps(bmz, oz), iz);
    const __m256 tz2 = _mm256_mul_ps(_mm256_sub_ps(bMz, oz), iz);

    __m256 tmin =
        _mm256_max_ps(tmin_b,
                      _mm256_max_ps(_mm256_min_ps(tx1, tx2),
                                    _mm256_max_ps(_mm256_min_ps(ty1, ty2), _mm256_min_ps(tz1, tz2))));
    __m256 tmax =
        _mm256_min_ps(tmax_b,
                      _mm256_min_ps(_mm256_max_ps(tx1, tx2),
                                    _mm256_min_ps(_mm256_max_ps(ty1, ty2), _mm256_max_ps(tz1, tz2))));

    const __m256 ok = _mm256_cmp_ps(tmin, tmax, _CMP_LE_OQ);
    const i32 mask = _mm256_movemask_ps(ok);
    return static_cast<u8>(mask);
}

// Coherent-packet traversal of one BLAS for occlusion (shadow rays).
// `live_in`  = lanes still searching (bit i set ⇔ lane i still live).
// On exit, OR-s newly-occluded lanes into `out_occluded`.
void blas_packet_occlusion_avx2(const detail::Bvh8State& bs,
                                const Ray* local_rays,
                                u8 live_in,
                                u8& out_occluded) noexcept {
    if (live_in == 0 || bs.wide_nodes.empty() || bs.triangles.empty()) {
        return;
    }

    alignas(32) f32 ox_a[8], oy_a[8], oz_a[8];
    alignas(32) f32 ix_a[8], iy_a[8], iz_a[8];
    alignas(32) f32 tmin_a[8], tmax_a[8];
    for (u32 r = 0; r < 8; ++r) {
        ox_a[r] = local_rays[r].origin.x;
        oy_a[r] = local_rays[r].origin.y;
        oz_a[r] = local_rays[r].origin.z;
        const f32 dx = local_rays[r].direction.x;
        const f32 dy = local_rays[r].direction.y;
        const f32 dz = local_rays[r].direction.z;
        constexpr f32 kBig = 1e30f;
        ix_a[r] = (std::fabs(dx) < 1e-20f) ? kBig : (1.0f / dx);
        iy_a[r] = (std::fabs(dy) < 1e-20f) ? kBig : (1.0f / dy);
        iz_a[r] = (std::fabs(dz) < 1e-20f) ? kBig : (1.0f / dz);
        tmin_a[r] = local_rays[r].t_min;
        tmax_a[r] = local_rays[r].t_max;
        if ((live_in & (1u << r)) == 0) {
            tmin_a[r] = 1.0f;
            tmax_a[r] = 0.0f;  // empty interval → never hits
        }
    }
    const __m256 ox = _mm256_load_ps(ox_a);
    const __m256 oy = _mm256_load_ps(oy_a);
    const __m256 oz = _mm256_load_ps(oz_a);
    const __m256 ix = _mm256_load_ps(ix_a);
    const __m256 iy = _mm256_load_ps(iy_a);
    const __m256 iz = _mm256_load_ps(iz_a);
    const __m256 tmin_b = _mm256_load_ps(tmin_a);
    const __m256 tmax_b = _mm256_load_ps(tmax_a);

    constexpr u32 kStackCap = 128;
    u32 stack[kStackCap];
    u32 top = 0;
    stack[top++] = 0;

    u8 done = out_occluded;  // lanes already terminated
    u8 live = static_cast<u8>(live_in & ~done);

    while (top > 0 && live != 0) {
        const u32 nid = stack[--top];
        const detail::Bvh8Node& n = bs.wide_nodes[nid];
        for (u32 c = 0; c < 8; ++c) {
            const u8 kind = n.child_kind[c];
            if (kind == 2)
                continue;
            const u8 hit_mask = packet8_slab_one_child(n.min_x[c],
                                                       n.min_y[c],
                                                       n.min_z[c],
                                                       n.max_x[c],
                                                       n.max_y[c],
                                                       n.max_z[c],
                                                       ox,
                                                       oy,
                                                       oz,
                                                       ix,
                                                       iy,
                                                       iz,
                                                       tmin_b,
                                                       tmax_b);
            const u8 active = static_cast<u8>(hit_mask & live);
            if (active == 0)
                continue;
            if (kind == 1) {
                // Leaf — test each primitive against each active lane.
                const u32 first = n.child_index[c];
                const u32 cnt = n.child_count[c];
                for (u32 i = 0; i < cnt && live != 0; ++i) {
                    const u32 pid = bs.prim_indices[first + i];
                    if (pid >= bs.triangles.size())
                        continue;
                    const Triangle& tri = bs.triangles[pid];
                    u8 lane_bit = 1;
                    for (u32 r = 0; r < 8; ++r, lane_bit = static_cast<u8>(lane_bit << 1)) {
                        if ((active & lane_bit) == 0)
                            continue;
                        if ((live & lane_bit) == 0)
                            continue;
                        if (detail::ray_triangle_occluded(local_rays[r], tri)) {
                            done = static_cast<u8>(done | lane_bit);
                            live = static_cast<u8>(live & ~lane_bit);
                        }
                    }
                }
            } else {
                if (top < kStackCap)
                    stack[top++] = n.child_index[c];
            }
        }
    }
    out_occluded = done;
}

}  // namespace

#endif  // __AVX2__

// ────────────────────────────────────────────────────────────────────────
// NEON 4-wide packet shadow driver (arm64).
//
// We service the 8-ray packet as two 4-wide halves. Per node, all 4 active
// rays of the half are tested against each non-empty child via a NEON
// `float32x4_t` slab test.
// ────────────────────────────────────────────────────────────────────────

#if PSYNDER_RT_HAVE_NEON && !defined(__AVX2__)

namespace {

PSY_FORCEINLINE
u8 packet4_slab_one_child(f32 cmnx,
                          f32 cmny,
                          f32 cmnz,
                          f32 cmxx,
                          f32 cmxy,
                          f32 cmxz,
                          float32x4_t ox,
                          float32x4_t oy,
                          float32x4_t oz,
                          float32x4_t ix,
                          float32x4_t iy,
                          float32x4_t iz,
                          float32x4_t tmin_b,
                          float32x4_t tmax_b) noexcept {
    const float32x4_t bmx = vdupq_n_f32(cmnx);
    const float32x4_t bmy = vdupq_n_f32(cmny);
    const float32x4_t bmz = vdupq_n_f32(cmnz);
    const float32x4_t bMx = vdupq_n_f32(cmxx);
    const float32x4_t bMy = vdupq_n_f32(cmxy);
    const float32x4_t bMz = vdupq_n_f32(cmxz);

    const float32x4_t tx1 = vmulq_f32(vsubq_f32(bmx, ox), ix);
    const float32x4_t tx2 = vmulq_f32(vsubq_f32(bMx, ox), ix);
    const float32x4_t ty1 = vmulq_f32(vsubq_f32(bmy, oy), iy);
    const float32x4_t ty2 = vmulq_f32(vsubq_f32(bMy, oy), iy);
    const float32x4_t tz1 = vmulq_f32(vsubq_f32(bmz, oz), iz);
    const float32x4_t tz2 = vmulq_f32(vsubq_f32(bMz, oz), iz);

    const float32x4_t tmin =
        vmaxq_f32(tmin_b,
                  vmaxq_f32(vminq_f32(tx1, tx2), vmaxq_f32(vminq_f32(ty1, ty2), vminq_f32(tz1, tz2))));
    const float32x4_t tmax =
        vminq_f32(tmax_b,
                  vminq_f32(vmaxq_f32(tx1, tx2), vminq_f32(vmaxq_f32(ty1, ty2), vmaxq_f32(tz1, tz2))));
    const uint32x4_t ok = vcleq_f32(tmin, tmax);
    alignas(16) u32 lanes[4];
    vst1q_u32(lanes, ok);
    u8 m = 0;
    for (u32 k = 0; k < 4; ++k)
        if (lanes[k])
            m = static_cast<u8>(m | (1u << k));
    return m;
}

// Walk the BVH for a 4-wide half of the 8-ray packet. live_bits / done_bits
// are *half-relative* (bits 0..3). Caller composes back into the 8-bit mask.
void blas_packet4_occlusion_neon_half(const detail::Bvh8State& bs,
                                      const Ray* rays4,
                                      u8 live_half_in,
                                      u8& out_done_half) noexcept {
    if (live_half_in == 0 || bs.wide_nodes.empty() || bs.triangles.empty())
        return;

    alignas(16) f32 ox_a[4], oy_a[4], oz_a[4];
    alignas(16) f32 ix_a[4], iy_a[4], iz_a[4];
    alignas(16) f32 tmin_a[4], tmax_a[4];
    for (u32 r = 0; r < 4; ++r) {
        ox_a[r] = rays4[r].origin.x;
        oy_a[r] = rays4[r].origin.y;
        oz_a[r] = rays4[r].origin.z;
        constexpr f32 kBig = 1e30f;
        const f32 dx = rays4[r].direction.x;
        const f32 dy = rays4[r].direction.y;
        const f32 dz = rays4[r].direction.z;
        ix_a[r] = (std::fabs(dx) < 1e-20f) ? kBig : (1.0f / dx);
        iy_a[r] = (std::fabs(dy) < 1e-20f) ? kBig : (1.0f / dy);
        iz_a[r] = (std::fabs(dz) < 1e-20f) ? kBig : (1.0f / dz);
        tmin_a[r] = rays4[r].t_min;
        tmax_a[r] = rays4[r].t_max;
        if ((live_half_in & (1u << r)) == 0) {
            tmin_a[r] = 1.0f;
            tmax_a[r] = 0.0f;
        }
    }
    const float32x4_t ox = vld1q_f32(ox_a);
    const float32x4_t oy = vld1q_f32(oy_a);
    const float32x4_t oz = vld1q_f32(oz_a);
    const float32x4_t ix = vld1q_f32(ix_a);
    const float32x4_t iy = vld1q_f32(iy_a);
    const float32x4_t iz = vld1q_f32(iz_a);
    const float32x4_t tmin_b = vld1q_f32(tmin_a);
    const float32x4_t tmax_b = vld1q_f32(tmax_a);

    constexpr u32 kStackCap = 128;
    u32 stack[kStackCap];
    u32 top = 0;
    stack[top++] = 0;

    u8 done = out_done_half;
    u8 live = static_cast<u8>(live_half_in & ~done);

    while (top > 0 && live != 0) {
        const u32 nid = stack[--top];
        const detail::Bvh8Node& n = bs.wide_nodes[nid];
        for (u32 c = 0; c < 8; ++c) {
            const u8 kind = n.child_kind[c];
            if (kind == 2)
                continue;
            const u8 hit_mask = packet4_slab_one_child(n.min_x[c],
                                                       n.min_y[c],
                                                       n.min_z[c],
                                                       n.max_x[c],
                                                       n.max_y[c],
                                                       n.max_z[c],
                                                       ox,
                                                       oy,
                                                       oz,
                                                       ix,
                                                       iy,
                                                       iz,
                                                       tmin_b,
                                                       tmax_b);
            const u8 active = static_cast<u8>(hit_mask & live);
            if (active == 0)
                continue;
            if (kind == 1) {
                const u32 first = n.child_index[c];
                const u32 cnt = n.child_count[c];
                for (u32 i = 0; i < cnt && live != 0; ++i) {
                    const u32 pid = bs.prim_indices[first + i];
                    if (pid >= bs.triangles.size())
                        continue;
                    const Triangle& tri = bs.triangles[pid];
                    u8 lane_bit = 1;
                    for (u32 r = 0; r < 4; ++r, lane_bit = static_cast<u8>(lane_bit << 1)) {
                        if ((active & lane_bit) == 0)
                            continue;
                        if ((live & lane_bit) == 0)
                            continue;
                        // Inlined Möller–Trumbore for this 4-wide path.
                        const math::Vec3 e1 = math::sub(tri.v1, tri.v0);
                        const math::Vec3 e2 = math::sub(tri.v2, tri.v0);
                        const math::Vec3 pv = math::cross(rays4[r].direction, e2);
                        const f32 det = math::dot(e1, pv);
                        if (std::fabs(det) < 1e-8f)
                            continue;
                        const f32 inv_det = 1.0f / det;
                        const math::Vec3 tv = math::sub(rays4[r].origin, tri.v0);
                        const f32 u = math::dot(tv, pv) * inv_det;
                        if (u < 0.0f || u > 1.0f)
                            continue;
                        const math::Vec3 qv = math::cross(tv, e1);
                        const f32 v = math::dot(rays4[r].direction, qv) * inv_det;
                        if (v < 0.0f || u + v > 1.0f)
                            continue;
                        const f32 tt = math::dot(e2, qv) * inv_det;
                        if (tt < rays4[r].t_min || tt > rays4[r].t_max)
                            continue;
                        done = static_cast<u8>(done | lane_bit);
                        live = static_cast<u8>(live & ~lane_bit);
                    }
                }
            } else {
                if (top < kStackCap)
                    stack[top++] = n.child_index[c];
            }
        }
    }
    out_done_half = done;
}

}  // namespace

#endif  // PSYNDER_RT_HAVE_NEON && !__AVX2__

namespace {

PSY_FORCEINLINE
f32 packet_safe_inv(f32 d) noexcept {
    constexpr f32 kBig = 1e30f;
    return (std::fabs(d) < 1e-20f) ? kBig : (1.0f / d);
}

PSY_FORCEINLINE
bool packet_ray_child_slab(const Ray& ray, const detail::Bvh8Node& n, u32 child) noexcept {
    const f32 ix = packet_safe_inv(ray.direction.x);
    const f32 iy = packet_safe_inv(ray.direction.y);
    const f32 iz = packet_safe_inv(ray.direction.z);

    const f32 tx1 = (n.min_x[child] - ray.origin.x) * ix;
    const f32 tx2 = (n.max_x[child] - ray.origin.x) * ix;
    f32 tmin = std::fmin(tx1, tx2);
    f32 tmax = std::fmax(tx1, tx2);

    const f32 ty1 = (n.min_y[child] - ray.origin.y) * iy;
    const f32 ty2 = (n.max_y[child] - ray.origin.y) * iy;
    tmin = std::fmax(tmin, std::fmin(ty1, ty2));
    tmax = std::fmin(tmax, std::fmax(ty1, ty2));

    const f32 tz1 = (n.min_z[child] - ray.origin.z) * iz;
    const f32 tz2 = (n.max_z[child] - ray.origin.z) * iz;
    tmin = std::fmax(tmin, std::fmin(tz1, tz2));
    tmax = std::fmin(tmax, std::fmax(tz1, tz2));

    return tmax >= std::fmax(tmin, ray.t_min) && tmin <= ray.t_max;
}

u8 packet_tlas_child_lanes(const ShadowPacket8& pkt, const detail::Bvh8Node& n, u32 child, u8 live) noexcept {
    u8 out = 0;
    u8 lane_bit = 1;
    for (u32 r = 0; r < 8; ++r, lane_bit = static_cast<u8>(lane_bit << 1)) {
        if ((live & lane_bit) == 0)
            continue;
        if (packet_ray_child_slab(pkt.rays[r], n, child))
            out = static_cast<u8>(out | lane_bit);
    }
    return out;
}

void trace_instance_packet(const detail::TlasState& ts,
                           u32 inst_i,
                           const ShadowPacket8& pkt,
                           u8 live_in,
                           u8& packed_done) noexcept {
    const detail::Bvh8State* bs = inst_i < ts.blas_states.size() ? ts.blas_states[inst_i] : nullptr;
    if (!bs || live_in == 0)
        return;

    Ray local_rays[8];
    for (u32 r = 0; r < 8; ++r) {
        const math::Vec4 o4{pkt.rays[r].origin.x, pkt.rays[r].origin.y, pkt.rays[r].origin.z, 1.0f};
        const math::Vec4 d4{pkt.rays[r].direction.x, pkt.rays[r].direction.y, pkt.rays[r].direction.z, 0.0f};
        const math::Vec4 oo = math::mul(ts.inv_transform[inst_i], o4);
        const math::Vec4 dd = math::mul(ts.inv_transform[inst_i], d4);
        local_rays[r].origin = {oo.x, oo.y, oo.z};
        local_rays[r].direction = {dd.x, dd.y, dd.z};
        local_rays[r].t_min = pkt.rays[r].t_min;
        local_rays[r].t_max = pkt.rays[r].t_max;
    }

#if defined(__AVX2__)
    blas_packet_occlusion_avx2(*bs, local_rays, live_in, packed_done);
#elif PSYNDER_RT_HAVE_NEON
    u8 done_lo = static_cast<u8>(packed_done & 0x0F);
    u8 done_hi = static_cast<u8>((packed_done >> 4) & 0x0F);
    const u8 live_lo = static_cast<u8>(live_in & 0x0F);
    const u8 live_hi = static_cast<u8>((live_in >> 4) & 0x0F);
    blas_packet4_occlusion_neon_half(*bs, &local_rays[0], live_lo, done_lo);
    blas_packet4_occlusion_neon_half(*bs, &local_rays[4], live_hi, done_hi);
    packed_done = static_cast<u8>((done_lo & 0x0F) | ((done_hi & 0x0F) << 4));
#else
    for (u32 r = 0; r < 8; ++r) {
        const u8 lane_bit = static_cast<u8>(1u << r);
        if ((live_in & lane_bit) == 0)
            continue;
        if (detail::occluded_scalar(*bs, local_rays[r]))
            packed_done = static_cast<u8>(packed_done | lane_bit);
    }
#endif
}

}  // namespace

void trace_shadow_packet(const Tlas& tlas, ShadowPacket8& pkt) {
    const auto& ts = detail::state_of(tlas);
    for (u32 i = 0; i < 8; ++i)
        pkt.occluded[i] = false;

    if (ts.instances.empty())
        return;

    // Walk the wide TLAS once. Only leaf instances whose world-space bounds
    // overlap a live packet lane enter the BLAS packet kernel.
    u8 packed_done = 0;
    if (ts.wide_nodes.empty()) {
        for (u32 inst_i = 0; inst_i < ts.instances.size() && packed_done != 0xFFu; ++inst_i) {
            const u8 live_in = static_cast<u8>(static_cast<u32>(~packed_done) & 0xFFu);
            trace_instance_packet(ts, inst_i, pkt, live_in, packed_done);
        }
        for (u32 i = 0; i < 8; ++i)
            pkt.occluded[i] = ((packed_done >> i) & 1u) != 0u;
        return;
    }

    constexpr u32 kStackCap = 128;
    u32 stack[kStackCap];
    u32 top = 0;
    stack[top++] = 0;
    while (top > 0 && packed_done != 0xFFu) {
        const u32 nid = stack[--top];
        const detail::Bvh8Node& n = ts.wide_nodes[nid];
        const u8 live = static_cast<u8>(static_cast<u32>(~packed_done) & 0xFFu);
        for (u32 c = 0; c < 8 && packed_done != 0xFFu; ++c) {
            const u8 kind = n.child_kind[c];
            if (kind == 2)
                continue;
            const u8 active = packet_tlas_child_lanes(pkt, n, c, live);
            if (active == 0)
                continue;
            if (kind == 1) {
                const u32 first = n.child_index[c];
                const u32 cnt = n.child_count[c];
                for (u32 k = 0; k < cnt && packed_done != 0xFFu; ++k) {
                    const u32 inst_i = ts.prim_indices[first + k];
                    if (inst_i >= ts.instances.size())
                        continue;
                    const u8 live_in = static_cast<u8>(active & ~packed_done);
                    trace_instance_packet(ts, inst_i, pkt, live_in, packed_done);
                }
            } else {
                if (top < kStackCap) {
                    const u32 child_node = n.child_index[c];
                    PSYNDER_RT_PREFETCH_RO(&ts.wide_nodes[child_node]);
                    stack[top++] = child_node;
                }
            }
        }
    }

    for (u32 i = 0; i < 8; ++i) {
        pkt.occluded[i] = ((packed_done >> i) & 1u) != 0u;
    }
}

}  // namespace psynder::render::rt
