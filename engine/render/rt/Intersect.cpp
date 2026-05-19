// SPDX-License-Identifier: MIT
// Psynder — ray / packet intersection kernels. Lane 08 owns.
//
// Two paths:
//   * Scalar traversal — single ray, walks the 8-wide BVH testing one
//     ray against eight child slabs per node. Always available; used by
//     Bvh8::intersect / Bvh8::occluded and Tlas object-space dispatch.
//   * AVX2 8-wide packet — `trace_shadow_packet` driver. A coherent DFS:
//     for every visited node, all 8 rays test against each of the 8
//     children (one AVX2 8-way slab test per child). Children whose slab
//     is hit by *any* live lane get pushed. At leaves, each primitive is
//     tested per-lane via scalar Möller–Trumbore.
//
// On platforms without AVX2 (Apple Silicon NEON arm64 + others), we run
// the scalar `occluded_scalar` path eight times. The wide NEON kernel
// (two 4-wide packets) lands in Wave B per the lane brief.
//
// Triangle test: Möller–Trumbore, single-precision.
// Slab test: branchless safe-divide (no div-by-zero on axis-aligned rays).

#include "Bvh.h"
#include "Bvh_internal.h"
#include "Bvh_impl.h"

#include <cmath>
#include <limits>

#if defined(__AVX2__)
#   include <immintrin.h>
#endif

namespace psynder::render::rt::detail {

namespace {

// Möller–Trumbore single-ray vs triangle. Writes hit_t / normal on success.
PSY_FORCEINLINE
bool ray_triangle_mt(const Ray& r, const Triangle& tri,
                     f32& hit_t, math::Vec3& hit_n) noexcept
{
    const math::Vec3 e1 = math::sub(tri.v1, tri.v0);
    const math::Vec3 e2 = math::sub(tri.v2, tri.v0);
    const math::Vec3 pv = math::cross(r.direction, e2);
    const f32 det = math::dot(e1, pv);
    constexpr f32 kEps = 1e-8f;
    if (std::fabs(det) < kEps) return false;
    const f32 inv_det = 1.0f / det;

    const math::Vec3 tv = math::sub(r.origin, tri.v0);
    const f32 u = math::dot(tv, pv) * inv_det;
    if (u < 0.0f || u > 1.0f) return false;

    const math::Vec3 qv = math::cross(tv, e1);
    const f32 v = math::dot(r.direction, qv) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return false;

    const f32 t = math::dot(e2, qv) * inv_det;
    if (t < r.t_min || t > r.t_max) return false;

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
// [t_min, t_max] window.
PSY_FORCEINLINE
bool node_slab_test(const Bvh8Node& node, u32 child,
                    math::Vec3 inv_dir, math::Vec3 origin,
                    f32 t_min, f32 t_max, f32& t_entry) noexcept
{
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

    if (tmax < std::fmax(tmin, t_min)) return false;
    if (tmin > t_max) return false;
    t_entry = std::fmax(tmin, t_min);
    return true;
}

// Safe-divide reciprocal of the ray direction.
PSY_FORCEINLINE
math::Vec3 safe_inv(math::Vec3 d) noexcept {
    constexpr f32 kBig = 1e30f;
    auto inv = [](f32 c) {
        return (std::fabs(c) < 1e-20f) ? kBig : (1.0f / c);
    };
    return { inv(d.x), inv(d.y), inv(d.z) };
}

}  // anonymous namespace

// ────────────────────────────────────────────────────────────────────────
// Scalar traversal (single ray)
// ────────────────────────────────────────────────────────────────────────

LocalHit traverse_scalar(const Bvh8State& s, const Ray& ray_in) noexcept {
    LocalHit out;
    out.hit = false;
    out.t   = ray_in.t_max;

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
        for (u32 c = 0; c < 8; ++c) {
            const u8 kind = n.child_kind[c];
            if (kind == 2) continue;  // empty
            f32 t_entry;
            if (!node_slab_test(n, c, inv_dir, ray.origin, ray.t_min, ray.t_max, t_entry)) {
                continue;
            }
            if (kind == 1) {
                const u32 first = n.child_index[c];
                const u32 cnt   = n.child_count[c];
                for (u32 i = 0; i < cnt; ++i) {
                    const u32 pid = s.prim_indices[first + i];
                    if (pid >= s.triangles.size()) continue;
                    f32 hit_t;
                    math::Vec3 hit_n;
                    if (ray_triangle_mt(ray, s.triangles[pid], hit_t, hit_n)) {
                        if (hit_t < ray.t_max) {
                            ray.t_max     = hit_t;
                            out.hit       = true;
                            out.t         = hit_t;
                            out.normal    = hit_n;
                            out.primitive = pid;
                        }
                    }
                }
            } else {
                if (top < kStackCap) {
                    stack[top++] = n.child_index[c];
                }
            }
        }
    }

    return out;
}

bool occluded_scalar(const Bvh8State& s, const Ray& ray_in) noexcept {
    if (s.wide_nodes.empty() || s.triangles.empty()) return false;
    Ray ray = ray_in;
    const math::Vec3 inv_dir = safe_inv(ray.direction);

    constexpr u32 kStackCap = 128;
    u32 stack[kStackCap];
    u32 top = 0;
    stack[top++] = 0;

    while (top > 0) {
        const u32 nid = stack[--top];
        const Bvh8Node& n = s.wide_nodes[nid];
        for (u32 c = 0; c < 8; ++c) {
            const u8 kind = n.child_kind[c];
            if (kind == 2) continue;
            f32 t_entry;
            if (!node_slab_test(n, c, inv_dir, ray.origin, ray.t_min, ray.t_max, t_entry)) {
                continue;
            }
            if (kind == 1) {
                const u32 first = n.child_index[c];
                const u32 cnt   = n.child_count[c];
                for (u32 i = 0; i < cnt; ++i) {
                    const u32 pid = s.prim_indices[first + i];
                    if (pid >= s.triangles.size()) continue;
                    if (ray_triangle_occluded(ray, s.triangles[pid])) {
                        return true;
                    }
                }
            } else {
                if (top < kStackCap) stack[top++] = n.child_index[c];
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
// Inputs are pre-broadcast 8-lane vectors (one per ray). Returns the bit
// mask of lanes that hit this child.
PSY_FORCEINLINE
u8 packet_slab_one_child(f32 cmnx, f32 cmny, f32 cmnz,
                         f32 cmxx, f32 cmxy, f32 cmxz,
                         __m256 ox, __m256 oy, __m256 oz,
                         __m256 ix, __m256 iy, __m256 iz,
                         __m256 tmin_b, __m256 tmax_b) noexcept
{
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

    __m256 tmin = _mm256_max_ps(tmin_b,
                  _mm256_max_ps(_mm256_min_ps(tx1, tx2),
                  _mm256_max_ps(_mm256_min_ps(ty1, ty2),
                                _mm256_min_ps(tz1, tz2))));
    __m256 tmax = _mm256_min_ps(tmax_b,
                  _mm256_min_ps(_mm256_max_ps(tx1, tx2),
                  _mm256_min_ps(_mm256_max_ps(ty1, ty2),
                                _mm256_max_ps(tz1, tz2))));

    const __m256 ok = _mm256_cmp_ps(tmin, tmax, _CMP_LE_OQ);
    const i32 mask  = _mm256_movemask_ps(ok);
    return static_cast<u8>(mask);
}

// Coherent-packet traversal of one BLAS for occlusion (shadow rays).
// `live_mask` = which lanes are still searching (others already occluded
// or terminated). On exit, OR-s newly-occluded lanes into `pkt.occluded`.
void blas_packet_occlusion_avx2(const detail::Bvh8State& bs,
                                const Ray* local_rays,
                                u8 live_in,
                                u8& out_occluded) noexcept
{
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
            tmax_a[r] = 0.0f;   // empty interval → never hits
        }
    }
    const __m256 ox     = _mm256_load_ps(ox_a);
    const __m256 oy     = _mm256_load_ps(oy_a);
    const __m256 oz     = _mm256_load_ps(oz_a);
    const __m256 ix     = _mm256_load_ps(ix_a);
    const __m256 iy     = _mm256_load_ps(iy_a);
    const __m256 iz     = _mm256_load_ps(iz_a);
    const __m256 tmin_b = _mm256_load_ps(tmin_a);
    const __m256 tmax_b = _mm256_load_ps(tmax_a);

    constexpr u32 kStackCap = 128;
    u32 stack[kStackCap];
    u32 top = 0;
    stack[top++] = 0;

    u8 done = out_occluded;       // lanes already terminated
    u8 live = static_cast<u8>(live_in & ~done);

    while (top > 0 && live != 0) {
        const u32 nid = stack[--top];
        const detail::Bvh8Node& n = bs.wide_nodes[nid];
        for (u32 c = 0; c < 8; ++c) {
            const u8 kind = n.child_kind[c];
            if (kind == 2) continue;
            const u8 hit_mask = packet_slab_one_child(
                n.min_x[c], n.min_y[c], n.min_z[c],
                n.max_x[c], n.max_y[c], n.max_z[c],
                ox, oy, oz, ix, iy, iz, tmin_b, tmax_b);
            const u8 active = static_cast<u8>(hit_mask & live);
            if (active == 0) continue;
            if (kind == 1) {
                // Leaf — test each primitive against each active lane.
                const u32 first = n.child_index[c];
                const u32 cnt   = n.child_count[c];
                for (u32 i = 0; i < cnt && live != 0; ++i) {
                    const u32 pid = bs.prim_indices[first + i];
                    if (pid >= bs.triangles.size()) continue;
                    const Triangle& tri = bs.triangles[pid];
                    u8 lane_bit = 1;
                    for (u32 r = 0; r < 8; ++r, lane_bit = static_cast<u8>(lane_bit << 1)) {
                        if ((active & lane_bit) == 0) continue;
                        if ((live & lane_bit) == 0)   continue;
                        if (ray_triangle_occluded(local_rays[r], tri)) {
                            done = static_cast<u8>(done | lane_bit);
                            live = static_cast<u8>(live & ~lane_bit);
                        }
                    }
                }
            } else {
                if (top < kStackCap) stack[top++] = n.child_index[c];
            }
        }
    }
    out_occluded = done;
}

}  // namespace

#endif  // __AVX2__


void trace_shadow_packet(const Tlas& tlas, ShadowPacket8& pkt) {
    const auto& ts = detail::state_of(tlas);
    for (u32 i = 0; i < 8; ++i) pkt.occluded[i] = false;

    if (ts.instances.empty()) return;

    // Walk each instance once. Per instance we transform the 8 rays into
    // object space and dispatch the packet kernel. Lanes already occluded
    // by an earlier instance get masked off via the live-mask.
    u8 packed_done = 0;

    for (u32 inst_i = 0; inst_i < ts.instances.size(); ++inst_i) {
        const auto& inst = ts.instances[inst_i];
        if (!inst.blas) continue;
        const auto& bs = detail::state_of(*inst.blas);

        // Build object-space rays for this instance.
        Ray local_rays[8];
        for (u32 r = 0; r < 8; ++r) {
            math::Vec4 o4{ pkt.rays[r].origin.x, pkt.rays[r].origin.y, pkt.rays[r].origin.z, 1.0f };
            math::Vec4 d4{ pkt.rays[r].direction.x, pkt.rays[r].direction.y, pkt.rays[r].direction.z, 0.0f };
            math::Vec4 oo = math::mul(ts.inv_transform[inst_i], o4);
            math::Vec4 dd = math::mul(ts.inv_transform[inst_i], d4);
            local_rays[r].origin    = { oo.x, oo.y, oo.z };
            local_rays[r].direction = { dd.x, dd.y, dd.z };
            local_rays[r].t_min     = pkt.rays[r].t_min;
            local_rays[r].t_max     = pkt.rays[r].t_max;
        }
        const u8 live_in = static_cast<u8>(static_cast<u32>(~packed_done) & 0xFFu);
        if (live_in == 0) break;

#if defined(__AVX2__)
        blas_packet_occlusion_avx2(bs, local_rays, live_in, packed_done);
#else
        // Portable / NEON: scalar per lane. Wave-B NEON-real kernel will
        // replace this with two 4-wide vget/vminq-based slab tests.
        for (u32 r = 0; r < 8; ++r) {
            if (packed_done & (1u << r)) continue;
            if (detail::occluded_scalar(bs, local_rays[r])) {
                packed_done = static_cast<u8>(packed_done | (1u << r));
            }
        }
#endif
        if (packed_done == 0xFFu) break;
    }

    for (u32 i = 0; i < 8; ++i) {
        pkt.occluded[i] = ((packed_done >> i) & 1u) != 0u;
    }
}

}  // namespace psynder::render::rt
