// SPDX-License-Identifier: MIT
// Psynder physics — capsule contact-MANIFOLD generation (header-only).
//
// Background (ADR-016, DESIGN.md §16). The capsule narrowphase in Kernels.h
// (kernel_capsule_capsule / capsule-box via GJK / kernel_plane_capsule) emits a
// SINGLE deepest contact point per step and re-collides every tick. One point
// is fine for a point-like or end-on contact, but a capsule lying on its SIDE
// (or two parallel capsules stacked) has an extended, line-like contact region.
// Resolving that with one point lets the body ROCK / squirm: there is no second
// point to supply a torque that resists rotation or lateral creep about the
// contact line, so the body slowly drifts and oscillates as the single point
// migrates from tick to tick.
//
// This header upgrades the NEAR-PARALLEL cases to a proper resting manifold: it
// clips the capsule's core SEGMENT against the other feature (the other
// capsule's segment, or the plane / box face) and emits TWO contacts, one at
// each end of the overlapping region, each carrying its own per-point depth.
// Two points spread along the contact line resist the rocking torque, so the
// capsule settles flat and parallel stacks rest without rolling off.
//
// Design constraints (match the rest of the lane):
//   * Deterministic + alloc-free: the manifold is a fixed-size struct (<= 2
//     points) returned by value; no heap, no RNG, -fno-fast-math friendly.
//   * No regression: the POINT-like / non-parallel / deep cases return exactly
//     ONE point identical to the legacy single-point kernel. Only the
//     near-parallel resting configuration grows a second point.
//   * Output convention matches Contact: `normal` points from A -> B, `depth`
//     is positive penetration, `point` sits between the two surfaces (the
//     legacy "ra - 0.5*depth from the A surface" convention per point).

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "physics/Shape.h"  // detail::quat_rotate

#include <algorithm>
#include <cmath>

namespace psynder::physics::detail::kernels {

// One contact point of a manifold (mirrors the relevant fields of Contact; the
// caller copies these into the flat Contact buffer the solver consumes).
struct ManifoldPoint {
    math::Vec3 point{0, 0, 0};
    math::Vec3 normal{0, 1, 0};  // A -> B
    f32 depth = 0.0f;            // >0 penetration
};

// A capsule/segment manifold: 1 or 2 points. Fixed storage, no heap.
struct CapsuleManifold {
    ManifoldPoint pts[2];
    u32 count = 0;
};

// ─── Small local geometry helpers (kept self-contained on purpose) ──────────
//
// These intentionally duplicate the tiny vector helpers used elsewhere in the
// lane rather than reaching into Kernels.h (which would create an include
// cycle: Kernels.h includes THIS header). They are trivially inlined.

namespace manifold_detail {

PSY_FORCEINLINE f32 len_sq(math::Vec3 v) noexcept {
    return math::dot(v, v);
}

PSY_FORCEINLINE math::Vec3 normalize_or(math::Vec3 v, math::Vec3 fb) noexcept {
    f32 l2 = len_sq(v);
    if (l2 <= 1e-20f)
        return fb;
    return math::mul(v, 1.0f / std::sqrt(l2));
}

PSY_FORCEINLINE void endpoints(
    math::Vec3 center, math::Quat q, f32 half_h, math::Vec3& a, math::Vec3& b) noexcept {
    math::Vec3 axis = detail::quat_rotate(q, {0, half_h, 0});
    a = math::sub(center, axis);
    b = math::add(center, axis);
}

// Closest point on segment [a,b] to point p; returns the clamped parameter t.
PSY_FORCEINLINE math::Vec3 closest_on_seg(
    math::Vec3 a, math::Vec3 b, math::Vec3 p, f32& t) noexcept {
    math::Vec3 ab = math::sub(b, a);
    f32 ab2 = math::dot(ab, ab);
    if (ab2 < 1e-20f) {
        t = 0.0f;
        return a;
    }
    t = std::clamp(math::dot(math::sub(p, a), ab) / ab2, 0.0f, 1.0f);
    return math::add(a, math::mul(ab, t));
}

// Single sphere-vs-sphere contact between two radius-padded points. `ca`/`cb`
// are the segment witness points (capsule cores); `ra`/`rb` the radii. Returns
// false when separated. Fills one ManifoldPoint with the legacy convention.
PSY_FORCEINLINE bool sphere_pair_point(
    math::Vec3 ca, f32 ra, math::Vec3 cb, f32 rb, ManifoldPoint& out) noexcept {
    math::Vec3 d = math::sub(cb, ca);
    f32 d2 = math::dot(d, d);
    f32 r = ra + rb;
    if (d2 >= r * r)
        return false;
    f32 dist = std::sqrt(d2);
    math::Vec3 n = (dist > 1e-12f) ? math::mul(d, 1.0f / dist) : math::Vec3{0, 1, 0};
    out.normal = n;
    out.depth = r - dist;
    out.point = math::add(ca, math::mul(n, ra - 0.5f * out.depth));
    return true;
}

}  // namespace manifold_detail

// Near-parallel threshold for promoting a single point to a two-point manifold.
// |sin(angle)| between the two features must be below this for the contact to
// count as line-like. ~0.04 rad (2.3 deg) — beyond that the contact is point-
// like enough that one point already supplies the right torque, so we keep the
// single point and avoid a degenerate (collapsed-to-one-end) clip.
inline constexpr f32 kManifoldParallelSin = 0.04f;

// ─── Capsule-capsule manifold ───────────────────────────────────────────────
//
// Core segments A=[a0,a1] (radius ra), B=[b0,b1] (radius rb). When the two
// segments are NEARLY PARALLEL and overlapping, the contact is a line: we clip
// the projection of one core segment onto the other and emit a contact at each
// end of the overlap, each from its own local closest-point pair. Otherwise we
// emit the single closest-point contact (legacy behaviour, no regression).
inline CapsuleManifold capsule_capsule_manifold(math::Vec3 ca,
                                                math::Quat qa,
                                                f32 ra,
                                                f32 ha,
                                                math::Vec3 cb,
                                                math::Quat qb,
                                                f32 rb,
                                                f32 hb) noexcept {
    using namespace manifold_detail;
    CapsuleManifold m;

    math::Vec3 a0, a1, b0, b1;
    endpoints(ca, qa, ha, a0, a1);
    endpoints(cb, qb, hb, b0, b1);

    math::Vec3 da = math::sub(a1, a0);
    math::Vec3 db = math::sub(b1, b0);
    math::Vec3 ua = normalize_or(da, {0, 1, 0});
    math::Vec3 ub = normalize_or(db, {0, 1, 0});

    // Parallelism: |sin| = |ua x ub|. Also require both segments to have real
    // length (a zero-length "capsule" is a sphere — keep it single point).
    f32 sin_ang = math::length(math::cross(ua, ub));
    bool both_have_len = (len_sq(da) > 1e-12f) && (len_sq(db) > 1e-12f);
    bool parallel = both_have_len && (sin_ang <= kManifoldParallelSin);

    if (!parallel) {
        // Legacy single point: closest points between the two core segments.
        // (closest_pts_segments equivalent — reuse the robust clamp below by
        // sampling A against B and B against A and taking the nearer pair.)
        f32 ta;
        math::Vec3 ca_pt;
        // Closest point on A to the midpoint of B, then refine onto B.
        math::Vec3 bm = math::mul(math::add(b0, b1), 0.5f);
        ca_pt = closest_on_seg(a0, a1, bm, ta);
        f32 tb;
        math::Vec3 cb_pt = closest_on_seg(b0, b1, ca_pt, tb);
        ca_pt = closest_on_seg(a0, a1, cb_pt, ta);
        ManifoldPoint p;
        if (sphere_pair_point(ca_pt, ra, cb_pt, rb, p)) {
            m.pts[0] = p;
            m.count = 1;
        }
        return m;
    }

    // Parallel: clip B's core segment to A's extent along the shared axis `ua`.
    // Project the four endpoints onto `ua` (origin a0) and find the overlap
    // interval [lo, hi] of [0, |da|] (A) with B's projected span.
    f32 la = math::length(da);
    auto proj = [&](math::Vec3 p) { return math::dot(math::sub(p, a0), ua); };
    f32 pb0 = proj(b0);
    f32 pb1 = proj(b1);
    f32 b_lo = std::min(pb0, pb1);
    f32 b_hi = std::max(pb0, pb1);
    f32 lo = std::max(0.0f, b_lo);
    f32 hi = std::min(la, b_hi);

    if (hi <= lo) {
        // Projections do not overlap (capsules meet near an endpoint cap) ->
        // single closest-point contact.
        f32 ta, tb;
        math::Vec3 bm = math::mul(math::add(b0, b1), 0.5f);
        math::Vec3 ca_pt = closest_on_seg(a0, a1, bm, ta);
        math::Vec3 cb_pt = closest_on_seg(b0, b1, ca_pt, tb);
        ca_pt = closest_on_seg(a0, a1, cb_pt, ta);
        ManifoldPoint p;
        if (sphere_pair_point(ca_pt, ra, cb_pt, rb, p)) {
            m.pts[0] = p;
            m.count = 1;
        }
        return m;
    }

    // Two clip points at parameters lo and hi along A's axis. For each, take the
    // point on A and the closest point on B, then build a sphere-pair contact.
    const f32 ends[2] = {lo, hi};
    for (int i = 0; i < 2; ++i) {
        math::Vec3 a_pt = math::add(a0, math::mul(ua, ends[i]));
        f32 tb;
        math::Vec3 b_pt = closest_on_seg(b0, b1, a_pt, tb);
        ManifoldPoint p;
        if (sphere_pair_point(a_pt, ra, b_pt, rb, p)) {
            m.pts[m.count++] = p;
        }
    }
    // If clipping produced a single overlapping point (the other end separated),
    // fall through with count==1; if BOTH ends separated (numerically), retry
    // the single closest-point contact so we never silently drop a real hit.
    if (m.count == 0) {
        f32 ta, tb;
        math::Vec3 bm = math::mul(math::add(b0, b1), 0.5f);
        math::Vec3 ca_pt = closest_on_seg(a0, a1, bm, ta);
        math::Vec3 cb_pt = closest_on_seg(b0, b1, ca_pt, tb);
        ca_pt = closest_on_seg(a0, a1, cb_pt, ta);
        ManifoldPoint p;
        if (sphere_pair_point(ca_pt, ra, cb_pt, rb, p)) {
            m.pts[0] = p;
            m.count = 1;
        }
    }
    return m;
}

// ─── Plane-capsule manifold ─────────────────────────────────────────────────
//
// Plane: surface normal `pn` (unit), offset `pd = dot(pn, plane_pos)`. Capsule
// core [e0,e1] radius rc. When the capsule axis is NEARLY PARALLEL to the plane
// (both endpoints similarly close to the surface) we emit a contact at BOTH
// endpoints, each with its own depth; otherwise the single deepest endpoint
// (legacy kernel_plane_capsule behaviour). `normal` is +pn (A=plane -> B=shape).
inline CapsuleManifold plane_capsule_manifold(
    math::Vec3 pn, f32 pd, math::Vec3 cc, math::Quat qc, f32 rc, f32 hc) noexcept {
    using namespace manifold_detail;
    CapsuleManifold m;

    math::Vec3 e0, e1;
    endpoints(cc, qc, hc, e0, e1);
    f32 sd0 = math::dot(pn, e0) - pd;  // signed dist of core endpoints
    f32 sd1 = math::dot(pn, e1) - pd;

    // Axis vs plane: how parallel is the capsule to the surface? The component
    // of the (unit) axis along pn is sin(angle to the surface). Near 0 => the
    // axis lies in the surface plane => extended (line) contact.
    math::Vec3 axis = math::sub(e1, e0);
    math::Vec3 uaxis = normalize_or(axis, {1, 0, 0});
    f32 axis_dot_n = std::fabs(math::dot(uaxis, pn));  // |sin(tilt)|
    bool has_len = len_sq(axis) > 1e-12f;
    bool parallel = has_len && (axis_dot_n <= kManifoldParallelSin);

    auto emit = [&](math::Vec3 e, f32 sd) {
        f32 sep = sd - rc;  // gap from capsule surface to plane
        if (sep >= 0.0f)
            return;  // this endpoint not penetrating
        ManifoldPoint p;
        p.normal = pn;
        // Contact point: capsule surface point projected onto the plane surface.
        math::Vec3 support = math::sub(e, math::mul(pn, rc));
        p.point = math::sub(support, math::mul(pn, sd - rc));
        p.depth = -sep;
        m.pts[m.count++] = p;
    };

    if (parallel) {
        emit(e0, sd0);
        emit(e1, sd1);
        if (m.count > 0)
            return m;
        // Neither endpoint penetrates -> no contact.
        return m;
    }

    // Single deepest endpoint (legacy behaviour).
    math::Vec3 lower = (sd0 <= sd1) ? e0 : e1;
    f32 lower_sd = (sd0 <= sd1) ? sd0 : sd1;
    f32 sep = lower_sd - rc;
    if (sep < 0.0f) {
        ManifoldPoint p;
        p.normal = pn;
        math::Vec3 support = math::sub(lower, math::mul(pn, rc));
        p.point = math::sub(support, math::mul(pn, lower_sd - rc));
        p.depth = -sep;
        m.pts[0] = p;
        m.count = 1;
    }
    return m;
}

// ─── Capsule-vs-box face manifold ───────────────────────────────────────────
//
// `seg_n` is the contact normal recovered by the narrowphase (A=capsule ->
// B=box, unit). When that normal is (nearly) aligned with one of the box's
// three face axes AND the capsule axis is nearly parallel to that face, the
// capsule lies flat on the box face: we clip the capsule core segment to the
// face rectangle and emit a contact at each clipped end. Otherwise the single
// `fallback` point is returned unchanged (no regression).
//
// `cap_c/cap_q/cap_r/cap_h` describe the capsule; `box_c/box_q/box_he` the box;
// `fallback` is the legacy single contact (already computed by the caller).
inline CapsuleManifold capsule_box_manifold(math::Vec3 cap_c,
                                            math::Quat cap_q,
                                            f32 cap_r,
                                            f32 cap_h,
                                            math::Vec3 box_c,
                                            math::Quat box_q,
                                            math::Vec3 box_he,
                                            math::Vec3 seg_n,  // A(capsule) -> B(box)
                                            const ManifoldPoint& fallback) noexcept {
    using namespace manifold_detail;
    CapsuleManifold m;
    m.pts[0] = fallback;
    m.count = 1;

    // Capsule core in world.
    math::Vec3 e0, e1;
    endpoints(cap_c, cap_q, cap_h, e0, e1);
    math::Vec3 axis = math::sub(e1, e0);
    if (len_sq(axis) <= 1e-12f)
        return m;  // sphere-like capsule -> single point
    math::Vec3 uaxis = math::mul(axis, 1.0f / std::sqrt(len_sq(axis)));

    // Box axes (world).
    math::Vec3 bx = detail::quat_rotate(box_q, {1, 0, 0});
    math::Vec3 by = detail::quat_rotate(box_q, {0, 1, 0});
    math::Vec3 bz = detail::quat_rotate(box_q, {0, 0, 1});
    const math::Vec3 axesw[3] = {bx, by, bz};
    const f32 hew[3] = {box_he.x, box_he.y, box_he.z};

    // Which box face does the contact normal point along? `seg_n` is capsule ->
    // box, so the box's outward face normal at the contact is -seg_n. Find the
    // box axis most aligned with -seg_n.
    math::Vec3 face_out = math::mul(seg_n, -1.0f);
    int fa = 0;
    f32 best = -1.0f;
    f32 sgn = 1.0f;
    for (int i = 0; i < 3; ++i) {
        f32 d = math::dot(axesw[i], face_out);
        if (std::fabs(d) > best) {
            best = std::fabs(d);
            fa = i;
            sgn = (d >= 0.0f) ? 1.0f : -1.0f;
        }
    }
    // Require the normal to be cleanly aligned with a face (not an edge/corner).
    if (best < 0.999f)
        return m;

    math::Vec3 fn = math::mul(axesw[fa], sgn);  // outward face normal (unit)
    // Capsule axis must be parallel to the face (perpendicular to face normal).
    if (std::fabs(math::dot(uaxis, fn)) > kManifoldParallelSin)
        return m;

    // In-face tangent axes (the two box axes other than `fa`).
    int t0 = (fa + 1) % 3;
    int t1 = (fa + 2) % 3;
    math::Vec3 ua_t = axesw[t0];
    math::Vec3 va_t = axesw[t1];
    f32 he_u = hew[t0];
    f32 he_v = hew[t1];

    // Face centre (box centre pushed out along the face normal by its half
    // extent on the face axis).
    math::Vec3 face_c = math::add(box_c, math::mul(fn, hew[fa]));

    // Clip the capsule core segment [e0,e1] to the rectangle (face_c, +-he_u
    // along ua_t, +-he_v along va_t) in the face's 2D tangent frame. We clip the
    // PARAMETER interval [0,1] of the segment against the 4 rectangle slabs.
    auto coord = [&](math::Vec3 p, math::Vec3 ax) {
        return math::dot(math::sub(p, face_c), ax);
    };
    f32 u0 = coord(e0, ua_t), u1 = coord(e1, ua_t);
    f32 v0 = coord(e0, va_t), v1 = coord(e1, va_t);

    f32 tmin = 0.0f, tmax = 1.0f;
    auto clip_slab = [&](f32 s0, f32 s1, f32 lo, f32 hi) -> bool {
        f32 d = s1 - s0;
        if (std::fabs(d) < 1e-9f) {
            // Segment parallel to this slab: reject if outside.
            return (s0 >= lo - 1e-6f && s0 <= hi + 1e-6f);
        }
        f32 ta = (lo - s0) / d;
        f32 tb = (hi - s0) / d;
        if (ta > tb)
            std::swap(ta, tb);
        tmin = std::max(tmin, ta);
        tmax = std::min(tmax, tb);
        return tmin <= tmax;
    };

    if (!clip_slab(u0, u1, -he_u, he_u))
        return m;
    if (!clip_slab(v0, v1, -he_v, he_v))
        return m;
    if (tmax - tmin < 1e-4f)
        return m;  // degenerate (touches face at a point) -> keep single point

    // Two clipped endpoints on the capsule core; project each onto the face and
    // measure penetration along the (inward) contact normal seg_n.
    math::Vec3 cseg[2] = {
        math::add(e0, math::mul(axis, tmin)),
        math::add(e0, math::mul(axis, tmax)),
    };
    CapsuleManifold out;
    for (int i = 0; i < 2; ++i) {
        // Penetration: the capsule surface point along the face-INward normal
        // (-fn == toward the box interior) vs the face plane. The capsule
        // surface point nearest the face is core - fn*cap_r... but core may be
        // inside; use signed distance of the core to the face plane.
        f32 core_sd = math::dot(math::sub(cseg[i], face_c), fn);  // >0 outside
        f32 sep = core_sd - cap_r;
        if (sep >= 0.0f)
            continue;  // this end not penetrating
        ManifoldPoint p;
        p.normal = seg_n;  // A(capsule) -> B(box)
        // Contact point: capsule surface point projected to the face plane,
        // placed midway (legacy "between surfaces" convention).
        math::Vec3 surf = math::sub(cseg[i], math::mul(fn, cap_r));
        p.point = math::sub(surf, math::mul(fn, 0.5f * sep));  // sep<0 -> nudge out
        p.depth = -sep;
        out.pts[out.count++] = p;
    }
    if (out.count >= 2)
        return out;     // genuine two-point manifold
    return m;           // fewer than 2 clipped points penetrate -> keep fallback
}

}  // namespace psynder::physics::detail::kernels
