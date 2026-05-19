// SPDX-License-Identifier: MIT
// Psynder physics — algorithmic core, header-only.
//
// Mirrors the audio lane's pattern (engine/audio/internal/MixerCore.h):
// pure-algorithmic kernels live in a header so unit tests can pull them in
// without linking `psynder_physics`. The `.cpp` TUs in the lane forward to
// these same inline functions, so there's exactly one definition of every
// algorithm.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "physics/Body.h"
#include "physics/Shape.h"
#include "physics/Broadphase.h"
#include "physics/Narrowphase.h"
#include "physics/Solver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace psynder::physics::detail::kernels {

// ─── Helpers ─────────────────────────────────────────────────────────────

PSY_FORCEINLINE f32 length_sq(math::Vec3 v) noexcept {
    return math::dot(v, v);
}

PSY_FORCEINLINE math::Vec3 safe_normalize(math::Vec3 v, math::Vec3 fb) noexcept {
    f32 l2 = length_sq(v);
    if (l2 <= 1e-20f) return fb;
    f32 inv = 1.0f / std::sqrt(l2);
    return math::mul(v, inv);
}

PSY_FORCEINLINE math::Vec3 closest_pt_segment(math::Vec3 a, math::Vec3 b,
                                                     math::Vec3 p, f32& t) noexcept {
    math::Vec3 ab = math::sub(b, a);
    f32 ab2 = math::dot(ab, ab);
    if (ab2 < 1e-20f) { t = 0.0f; return a; }
    t = math::dot(math::sub(p, a), ab) / ab2;
    t = std::clamp(t, 0.0f, 1.0f);
    return math::add(a, math::mul(ab, t));
}

inline void closest_pts_segments(math::Vec3 p1, math::Vec3 q1,
                                 math::Vec3 p2, math::Vec3 q2,
                                 math::Vec3& c1, math::Vec3& c2) noexcept {
    math::Vec3 d1 = math::sub(q1, p1);
    math::Vec3 d2 = math::sub(q2, p2);
    math::Vec3 r  = math::sub(p1, p2);
    f32 a = math::dot(d1, d1);
    f32 e = math::dot(d2, d2);
    f32 f = math::dot(d2, r);

    f32 s, t;
    constexpr f32 kEps = 1e-12f;
    if (a <= kEps && e <= kEps) { c1 = p1; c2 = p2; return; }
    if (a <= kEps) {
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        f32 c = math::dot(d1, r);
        if (e <= kEps) {
            t = 0.0f;
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            f32 b = math::dot(d1, d2);
            f32 denom = a*e - b*b;
            s = (denom != 0.0f) ? std::clamp((b*f - c*e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b*s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = std::clamp(-c / a, 0.0f, 1.0f); }
            else if (t > 1.0f) { t = 1.0f; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
        }
    }
    c1 = math::add(p1, math::mul(d1, s));
    c2 = math::add(p2, math::mul(d2, t));
}

inline void capsule_endpoints(math::Vec3 center, math::Quat q, f32 half_h,
                              math::Vec3& a, math::Vec3& b) noexcept {
    math::Vec3 axis = quat_rotate(q, {0, half_h, 0});
    a = math::sub(center, axis);
    b = math::add(center, axis);
}

// ─── Specialised collision kernels ──────────────────────────────────────

inline bool kernel_sphere_sphere(math::Vec3 ca, f32 ra,
                                 math::Vec3 cb, f32 rb,
                                 Contact& out) noexcept {
    math::Vec3 d = math::sub(cb, ca);
    f32 d2 = math::dot(d, d);
    f32 r  = ra + rb;
    if (d2 >= r * r) return false;
    f32 dist = std::sqrt(d2);
    math::Vec3 n = (dist > 1e-12f) ? math::mul(d, 1.0f / dist)
                                   : math::Vec3{0, 1, 0};
    out.normal_world = n;
    out.depth        = r - dist;
    out.point_world  = math::add(ca, math::mul(n, ra - 0.5f * out.depth));
    return true;
}

inline bool kernel_sphere_capsule(math::Vec3 cs, f32 rs,
                                  math::Vec3 cc, math::Quat qc,
                                  f32 rc, f32 hc,
                                  Contact& out) noexcept {
    math::Vec3 p, q;
    capsule_endpoints(cc, qc, hc, p, q);
    f32 t;
    math::Vec3 closest = closest_pt_segment(p, q, cs, t);
    return kernel_sphere_sphere(cs, rs, closest, rc, out);
}

inline bool kernel_capsule_capsule(math::Vec3 ca, math::Quat qa, f32 ra, f32 ha,
                                   math::Vec3 cb, math::Quat qb, f32 rb, f32 hb,
                                   Contact& out) noexcept {
    math::Vec3 a0, a1, b0, b1;
    capsule_endpoints(ca, qa, ha, a0, a1);
    capsule_endpoints(cb, qb, hb, b0, b1);
    math::Vec3 c1, c2;
    closest_pts_segments(a0, a1, b0, b1, c1, c2);
    return kernel_sphere_sphere(c1, ra, c2, rb, out);
}

inline bool kernel_aabb_aabb(math::Aabb a, math::Aabb b, Contact& out) noexcept {
    if (a.max.x < b.min.x || a.min.x > b.max.x) return false;
    if (a.max.y < b.min.y || a.min.y > b.max.y) return false;
    if (a.max.z < b.min.z || a.min.z > b.max.z) return false;

    f32 ox = std::min(a.max.x, b.max.x) - std::max(a.min.x, b.min.x);
    f32 oy = std::min(a.max.y, b.max.y) - std::max(a.min.y, b.min.y);
    f32 oz = std::min(a.max.z, b.max.z) - std::max(a.min.z, b.min.z);
    math::Vec3 ca{(a.min.x + a.max.x) * 0.5f,
                  (a.min.y + a.max.y) * 0.5f,
                  (a.min.z + a.max.z) * 0.5f};
    math::Vec3 cb{(b.min.x + b.max.x) * 0.5f,
                  (b.min.y + b.max.y) * 0.5f,
                  (b.min.z + b.max.z) * 0.5f};
    math::Vec3 d = math::sub(cb, ca);
    if (ox <= oy && ox <= oz) {
        out.normal_world = {d.x >= 0 ? 1.0f : -1.0f, 0, 0};
        out.depth = ox;
    } else if (oy <= oz) {
        out.normal_world = {0, d.y >= 0 ? 1.0f : -1.0f, 0};
        out.depth = oy;
    } else {
        out.normal_world = {0, 0, d.z >= 0 ? 1.0f : -1.0f};
        out.depth = oz;
    }
    out.point_world = {(std::max(a.min.x, b.min.x) + std::min(a.max.x, b.max.x)) * 0.5f,
                       (std::max(a.min.y, b.min.y) + std::min(a.max.y, b.max.y)) * 0.5f,
                       (std::max(a.min.z, b.min.z) + std::min(a.max.z, b.max.z)) * 0.5f};
    return true;
}

// ─── GJK + EPA (support-mapping convex pair) ────────────────────────────

inline math::Vec3 kernel_support(const GjkSupport& s, math::Vec3 d) noexcept {
    math::Quat qinv{-s.rotation.x, -s.rotation.y, -s.rotation.z, s.rotation.w};
    math::Vec3 dl = quat_rotate(qinv, d);
    math::Vec3 local;
    switch (s.shape) {
        case 0: {
            f32 r = s.half_extent.x;
            math::Vec3 dn = safe_normalize(dl, {1, 0, 0});
            local = math::mul(dn, r);
            break;
        }
        case 1: {
            f32 r = s.half_extent.x;
            f32 h = s.half_extent.y;
            math::Vec3 dn = safe_normalize(dl, {1, 0, 0});
            local = math::mul(dn, r);
            local.y += (dl.y >= 0.0f) ? h : -h;
            break;
        }
        case 2: {
            local = {
                dl.x >= 0.0f ? s.half_extent.x : -s.half_extent.x,
                dl.y >= 0.0f ? s.half_extent.y : -s.half_extent.y,
                dl.z >= 0.0f ? s.half_extent.z : -s.half_extent.z,
            };
            break;
        }
        default: {
            local = {
                dl.x >= 0.0f ? s.half_extent.x : -s.half_extent.x,
                dl.y >= 0.0f ? s.half_extent.x : -s.half_extent.x,
                dl.z >= 0.0f ? s.half_extent.x : -s.half_extent.x,
            };
            break;
        }
    }
    return math::add(s.position, quat_rotate(s.rotation, local));
}

struct MinkowskiPt {
    math::Vec3 p;
    math::Vec3 sa;
    math::Vec3 sb;
};

inline MinkowskiPt mink_support(const GjkSupport& a, const GjkSupport& b,
                                math::Vec3 d) noexcept {
    math::Vec3 sa = kernel_support(a, d);
    math::Vec3 sb = kernel_support(b, math::mul(d, -1.0f));
    return { math::sub(sa, sb), sa, sb };
}

inline bool do_simplex(std::array<MinkowskiPt, 4>& simplex, u32& n,
                       math::Vec3& dir) noexcept {
    auto same_dir = [](math::Vec3 a, math::Vec3 b) {
        return math::dot(a, b) > 0.0f;
    };

    if (n == 2) {
        math::Vec3 A = simplex[1].p;
        math::Vec3 B = simplex[0].p;
        math::Vec3 AB = math::sub(B, A);
        math::Vec3 AO = math::mul(A, -1.0f);
        if (same_dir(AB, AO)) {
            dir = math::cross(math::cross(AB, AO), AB);
            if (length_sq(dir) < 1e-20f) dir = {-AB.y, AB.x, 0};
        } else {
            simplex[0] = simplex[1];
            n = 1;
            dir = AO;
        }
        return false;
    }
    if (n == 3) {
        math::Vec3 A = simplex[2].p;
        math::Vec3 B = simplex[1].p;
        math::Vec3 C = simplex[0].p;
        math::Vec3 AO = math::mul(A, -1.0f);
        math::Vec3 AB = math::sub(B, A);
        math::Vec3 AC = math::sub(C, A);
        math::Vec3 ABC = math::cross(AB, AC);
        if (same_dir(math::cross(ABC, AC), AO)) {
            if (same_dir(AC, AO)) {
                simplex[1] = simplex[2];
                n = 2;
                dir = math::cross(math::cross(AC, AO), AC);
            } else {
                simplex[0] = simplex[1];
                simplex[1] = simplex[2];
                n = 2;
                return do_simplex(simplex, n, dir);
            }
        } else if (same_dir(math::cross(AB, ABC), AO)) {
            simplex[0] = simplex[1];
            simplex[1] = simplex[2];
            n = 2;
            return do_simplex(simplex, n, dir);
        } else {
            if (same_dir(ABC, AO)) {
                dir = ABC;
            } else {
                std::swap(simplex[0], simplex[1]);
                dir = math::mul(ABC, -1.0f);
            }
        }
        return false;
    }
    // n == 4
    math::Vec3 A = simplex[3].p;
    math::Vec3 B = simplex[2].p;
    math::Vec3 C = simplex[1].p;
    math::Vec3 D = simplex[0].p;
    math::Vec3 AO = math::mul(A, -1.0f);
    math::Vec3 AB = math::sub(B, A);
    math::Vec3 AC = math::sub(C, A);
    math::Vec3 AD = math::sub(D, A);
    math::Vec3 ABC = math::cross(AB, AC);
    math::Vec3 ACD = math::cross(AC, AD);
    math::Vec3 ADB = math::cross(AD, AB);
    if (same_dir(ABC, AO)) {
        simplex[0] = simplex[1];
        simplex[1] = simplex[2];
        simplex[2] = simplex[3];
        n = 3;
        return do_simplex(simplex, n, dir);
    }
    if (same_dir(ACD, AO)) {
        simplex[2] = simplex[3];
        n = 3;
        return do_simplex(simplex, n, dir);
    }
    if (same_dir(ADB, AO)) {
        simplex[1] = simplex[0];
        simplex[0] = simplex[2];
        simplex[2] = simplex[3];
        n = 3;
        return do_simplex(simplex, n, dir);
    }
    return true;
}

inline bool gjk_intersect(const GjkSupport& a, const GjkSupport& b,
                          std::array<MinkowskiPt, 4>& out_simplex,
                          u32& out_n) noexcept {
    math::Vec3 dir = math::sub(b.position, a.position);
    if (length_sq(dir) < 1e-20f) dir = {1, 0, 0};
    MinkowskiPt s = mink_support(a, b, dir);
    out_simplex[0] = s;
    out_n = 1;
    dir = math::mul(s.p, -1.0f);
    for (u32 iter = 0; iter < 64; ++iter) {
        if (length_sq(dir) < 1e-20f) { return true; }
        MinkowskiPt p = mink_support(a, b, dir);
        if (math::dot(p.p, dir) < 0.0f) return false;
        out_simplex[out_n++] = p;
        if (do_simplex(out_simplex, out_n, dir)) return true;
    }
    return false;
}

struct EpaFace {
    u32 a, b, c;
    math::Vec3 n;
    f32 dist;
};

inline math::Vec3 tri_normal(math::Vec3 a, math::Vec3 b, math::Vec3 c) noexcept {
    return math::cross(math::sub(b, a), math::sub(c, a));
}

inline EpaFace make_face(const std::vector<MinkowskiPt>& verts,
                         u32 a, u32 b, u32 c) noexcept {
    EpaFace f{ a, b, c, {}, 0.0f };
    math::Vec3 n = tri_normal(verts[a].p, verts[b].p, verts[c].p);
    f.n = safe_normalize(n, {0, 1, 0});
    f.dist = math::dot(f.n, verts[a].p);
    if (f.dist < 0.0f) {
        f.n = math::mul(f.n, -1.0f);
        f.dist = -f.dist;
        std::swap(f.b, f.c);
    }
    return f;
}

inline bool epa_penetration(const GjkSupport& a, const GjkSupport& b,
                            const std::array<MinkowskiPt, 4>& seed,
                            Contact& out) noexcept {
    std::vector<MinkowskiPt> verts(seed.begin(), seed.end());
    std::vector<EpaFace> faces;
    faces.reserve(32);
    faces.push_back(make_face(verts, 0, 1, 2));
    faces.push_back(make_face(verts, 0, 2, 3));
    faces.push_back(make_face(verts, 0, 3, 1));
    faces.push_back(make_face(verts, 1, 3, 2));

    constexpr u32 kMaxIter = 32;
    for (u32 iter = 0; iter < kMaxIter; ++iter) {
        usize best = 0;
        f32 best_dist = faces[0].dist;
        for (usize i = 1; i < faces.size(); ++i) {
            if (faces[i].dist < best_dist) { best_dist = faces[i].dist; best = i; }
        }
        EpaFace bf = faces[best];

        MinkowskiPt p = mink_support(a, b, bf.n);
        f32 d = math::dot(p.p, bf.n);
        if (d - bf.dist < 1e-4f) {
            out.normal_world = bf.n;
            out.depth        = d;
            math::Vec3 origin_on_face = math::mul(bf.n, bf.dist);
            math::Vec3 v0 = math::sub(verts[bf.b].p, verts[bf.a].p);
            math::Vec3 v1 = math::sub(verts[bf.c].p, verts[bf.a].p);
            math::Vec3 v2 = math::sub(origin_on_face, verts[bf.a].p);
            f32 d00 = math::dot(v0, v0);
            f32 d01 = math::dot(v0, v1);
            f32 d11 = math::dot(v1, v1);
            f32 d20 = math::dot(v2, v0);
            f32 d21 = math::dot(v2, v1);
            f32 denom = d00 * d11 - d01 * d01;
            f32 bu = 0.0f, bv = 0.0f, bw = 1.0f;
            if (std::fabs(denom) > 1e-12f) {
                bv = (d11 * d20 - d01 * d21) / denom;
                bw = (d00 * d21 - d01 * d20) / denom;
                bu = 1.0f - bv - bw;
            }
            math::Vec3 pa = math::add(math::add(math::mul(verts[bf.a].sa, bu),
                                                math::mul(verts[bf.b].sa, bv)),
                                      math::mul(verts[bf.c].sa, bw));
            math::Vec3 pb = math::add(math::add(math::mul(verts[bf.a].sb, bu),
                                                math::mul(verts[bf.b].sb, bv)),
                                      math::mul(verts[bf.c].sb, bw));
            out.point_world = math::mul(math::add(pa, pb), 0.5f);
            return true;
        }

        struct Edge { u32 a, b; };
        std::vector<Edge> silhouette;
        std::vector<EpaFace> new_faces;
        new_faces.reserve(faces.size());
        for (const EpaFace& f : faces) {
            if (math::dot(f.n, math::sub(p.p, verts[f.a].p)) > 0.0f) {
                auto add_edge = [&](u32 e0, u32 e1) {
                    for (auto it = silhouette.begin(); it != silhouette.end(); ++it) {
                        if (it->a == e1 && it->b == e0) {
                            silhouette.erase(it);
                            return;
                        }
                    }
                    silhouette.push_back({e0, e1});
                };
                add_edge(f.a, f.b);
                add_edge(f.b, f.c);
                add_edge(f.c, f.a);
            } else {
                new_faces.push_back(f);
            }
        }
        u32 pi = static_cast<u32>(verts.size());
        verts.push_back(p);
        for (const Edge& e : silhouette) {
            new_faces.push_back(make_face(verts, e.a, e.b, pi));
        }
        faces.swap(new_faces);
        if (faces.empty()) return false;
    }
    return false;
}

inline bool kernel_gjk_epa(const GjkSupport& a, const GjkSupport& b,
                           Contact& out) noexcept {
    std::array<MinkowskiPt, 4> simplex{};
    u32 n = 0;
    if (!gjk_intersect(a, b, simplex, n)) return false;
    while (n < 4) {
        math::Vec3 dir{1, 0, 0};
        if (n >= 2) {
            math::Vec3 e0 = math::sub(simplex[1].p, simplex[0].p);
            dir = math::cross(e0, {0, 1, 0});
            if (length_sq(dir) < 1e-12f) dir = math::cross(e0, {1, 0, 0});
        }
        MinkowskiPt sp = mink_support(a, b, dir);
        if (math::dot(sp.p, dir) <= 0.0f) {
            sp = mink_support(a, b, math::mul(dir, -1.0f));
        }
        simplex[n++] = sp;
    }
    return epa_penetration(a, b, simplex, out);
}

// ─── Pair-shape dispatcher (header-inline so tests get the full path) ───

inline bool kernel_collide_pair(const Body& a, const Body& b, Contact& out) noexcept {
    if (a.shape == 0 && b.shape == 0) {
        return kernel_sphere_sphere(a.position, a.half_extent.x,
                                    b.position, b.half_extent.x, out);
    }
    if (a.shape == 0 && b.shape == 1) {
        return kernel_sphere_capsule(a.position, a.half_extent.x,
                                     b.position, b.rotation,
                                     b.half_extent.x, b.half_extent.y, out);
    }
    if (a.shape == 1 && b.shape == 0) {
        bool hit = kernel_sphere_capsule(b.position, b.half_extent.x,
                                         a.position, a.rotation,
                                         a.half_extent.x, a.half_extent.y, out);
        if (hit) out.normal_world = math::mul(out.normal_world, -1.0f);
        return hit;
    }
    if (a.shape == 1 && b.shape == 1) {
        return kernel_capsule_capsule(a.position, a.rotation, a.half_extent.x, a.half_extent.y,
                                      b.position, b.rotation, b.half_extent.x, b.half_extent.y,
                                      out);
    }
    if (a.shape == 2 && b.shape == 2) {
        auto is_id = [](math::Quat q) {
            return std::fabs(q.x) + std::fabs(q.y) + std::fabs(q.z) < 1e-4f;
        };
        if (is_id(a.rotation) && is_id(b.rotation)) {
            math::Aabb ba{ math::sub(a.position, a.half_extent),
                           math::add(a.position, a.half_extent) };
            math::Aabb bb{ math::sub(b.position, b.half_extent),
                           math::add(b.position, b.half_extent) };
            return kernel_aabb_aabb(ba, bb, out);
        }
    }
    GjkSupport sa{a.position, a.rotation, a.shape, a.half_extent};
    GjkSupport sb{b.position, b.rotation, b.shape, b.half_extent};
    return kernel_gjk_epa(sa, sb, out);
}

// ─── Union-find island detection (header-inline) ────────────────────────

struct Dsu {
    std::vector<u32> parent;
    std::vector<u32> size;
    void make_set_for(u32 max_index) {
        parent.resize(max_index + 1);
        size.assign(max_index + 1, 1);
        for (u32 i = 0; i <= max_index; ++i) parent[i] = i;
    }
    u32 find(u32 x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(u32 a, u32 b) {
        u32 ra = find(a);
        u32 rb = find(b);
        if (ra == rb) return;
        if (size[ra] < size[rb]) std::swap(ra, rb);
        parent[rb] = ra;
        size[ra] += size[rb];
    }
};

inline void kernel_detect_islands(std::vector<Contact>& contacts,
                                  std::span<const Body> bodies,
                                  std::vector<u32>& body_indices,
                                  std::vector<Island>& islands) {
    body_indices.clear();
    islands.clear();
    if (contacts.empty()) return;

    u32 max_body = 0;
    for (const Contact& c : contacts) {
        max_body = std::max(max_body, std::max(c.body_a, c.body_b));
    }
    Dsu dsu;
    dsu.make_set_for(max_body);

    for (const Contact& c : contacts) {
        bool a_dyn = bodies[c.body_a].inv_mass > 0.0f;
        bool b_dyn = bodies[c.body_b].inv_mass > 0.0f;
        if (a_dyn && b_dyn) dsu.unite(c.body_a, c.body_b);
    }

    auto root_of_contact = [&](const Contact& c) {
        if (bodies[c.body_a].inv_mass > 0.0f) return dsu.find(c.body_a);
        return dsu.find(c.body_b);
    };
    std::stable_sort(contacts.begin(), contacts.end(),
        [&](const Contact& x, const Contact& y) {
            return root_of_contact(x) < root_of_contact(y);
        });

    u32 cur_root = root_of_contact(contacts.front());
    Island isl{};
    isl.first_contact = 0;
    isl.first_body    = 0;

    std::vector<u32> island_body_set;
    island_body_set.reserve(8);

    auto flush_island = [&](u32 end_contact_index) {
        std::sort(island_body_set.begin(), island_body_set.end());
        island_body_set.erase(std::unique(island_body_set.begin(),
                                          island_body_set.end()),
                              island_body_set.end());
        isl.first_body    = static_cast<u32>(body_indices.size());
        isl.body_count    = static_cast<u32>(island_body_set.size());
        body_indices.insert(body_indices.end(),
                            island_body_set.begin(), island_body_set.end());
        isl.contact_count = end_contact_index - isl.first_contact;
        islands.push_back(isl);
    };

    for (u32 i = 0; i < contacts.size(); ++i) {
        u32 r = root_of_contact(contacts[i]);
        if (r != cur_root) {
            flush_island(i);
            cur_root = r;
            isl.first_contact = i;
            island_body_set.clear();
        }
        island_body_set.push_back(contacts[i].body_a);
        island_body_set.push_back(contacts[i].body_b);
    }
    flush_island(static_cast<u32>(contacts.size()));
}

// ─── Solver per-island (header-inline) ──────────────────────────────────

PSY_FORCEINLINE math::Vec3 apply_inv_inertia(const Body& b, math::Vec3 v) noexcept {
    return { v.x * b.inertia.inv_local.x,
             v.y * b.inertia.inv_local.y,
             v.z * b.inertia.inv_local.z };
}

inline void basis_for_normal(math::Vec3 n, math::Vec3& t1, math::Vec3& t2) noexcept {
    if (std::fabs(n.x) >= 0.57735f) {
        t1 = math::Vec3{ n.y, -n.x, 0 };
    } else {
        t1 = math::Vec3{ 0, n.z, -n.y };
    }
    t1 = math::mul(t1, 1.0f / std::sqrt(math::dot(t1, t1)));
    t2 = math::cross(n, t1);
}

inline void kernel_solve_island(const Island& island,
                                std::span<Contact> contacts,
                                std::span<const u32> body_indices,
                                std::span<Body> bodies,
                                const SolverParams& params,
                                f32 dt) noexcept {
    (void)body_indices;
    (void)island;

    struct CC {
        math::Vec3 ra, rb;
        math::Vec3 t1, t2;
        f32        eff_n  = 0.0f, eff_t1 = 0.0f, eff_t2 = 0.0f;
        f32        bias   = 0.0f, e = 0.0f, mu = 0.0f;
    };
    std::vector<CC> caches(contacts.size());

    for (usize i = 0; i < contacts.size(); ++i) {
        Contact& c = contacts[i];
        CC& cc = caches[i];
        Body& A = bodies[c.body_a];
        Body& B = bodies[c.body_b];
        cc.ra = math::sub(c.point_world, A.position);
        cc.rb = math::sub(c.point_world, B.position);
        basis_for_normal(c.normal_world, cc.t1, cc.t2);

        auto eff_mass = [&](math::Vec3 dir) -> f32 {
            math::Vec3 ra_x = math::cross(cc.ra, dir);
            math::Vec3 rb_x = math::cross(cc.rb, dir);
            f32 ang = math::dot(dir, math::cross(apply_inv_inertia(A, ra_x), cc.ra))
                    + math::dot(dir, math::cross(apply_inv_inertia(B, rb_x), cc.rb));
            f32 lin = A.inv_mass + B.inv_mass;
            f32 k = lin + ang;
            return (k > 1e-12f) ? 1.0f / k : 0.0f;
        };
        cc.eff_n  = eff_mass(c.normal_world);
        cc.eff_t1 = eff_mass(cc.t1);
        cc.eff_t2 = eff_mass(cc.t2);

        math::Vec3 va = math::add(A.linear_velocity, math::cross(A.angular_velocity, cc.ra));
        math::Vec3 vb = math::add(B.linear_velocity, math::cross(B.angular_velocity, cc.rb));
        f32 rel_n = math::dot(math::sub(vb, va), c.normal_world);
        f32 e = std::max(A.restitution, B.restitution);
        cc.e = (rel_n < -params.restitution_threshold) ? -e * rel_n : 0.0f;

        cc.mu = std::sqrt(std::max(0.0f, A.friction) * std::max(0.0f, B.friction));

        f32 pen = std::max(0.0f, c.depth - params.slop);
        cc.bias = (params.baumgarte / dt) * pen;

        math::Vec3 P = math::add(math::mul(c.normal_world, c.normal_impulse_acc),
                                  math::add(math::mul(cc.t1, c.friction_impulse_acc1),
                                            math::mul(cc.t2, c.friction_impulse_acc2)));
        A.linear_velocity  = math::sub(A.linear_velocity,  math::mul(P, A.inv_mass));
        B.linear_velocity  = math::add(B.linear_velocity,  math::mul(P, B.inv_mass));
        A.angular_velocity = math::sub(A.angular_velocity, apply_inv_inertia(A, math::cross(cc.ra, P)));
        B.angular_velocity = math::add(B.angular_velocity, apply_inv_inertia(B, math::cross(cc.rb, P)));
    }

    for (u32 it = 0; it < params.velocity_iterations; ++it) {
        for (usize i = 0; i < contacts.size(); ++i) {
            Contact& c = contacts[i];
            CC& cc = caches[i];
            Body& A = bodies[c.body_a];
            Body& B = bodies[c.body_b];

            auto rel_vel = [&](math::Vec3 dir) -> f32 {
                math::Vec3 va = math::add(A.linear_velocity, math::cross(A.angular_velocity, cc.ra));
                math::Vec3 vb = math::add(B.linear_velocity, math::cross(B.angular_velocity, cc.rb));
                return math::dot(math::sub(vb, va), dir);
            };

            f32 vn = rel_vel(c.normal_world);
            f32 jn = (cc.e + cc.bias - vn) * cc.eff_n;
            f32 new_jn = std::max(0.0f, c.normal_impulse_acc + jn);
            jn = new_jn - c.normal_impulse_acc;
            c.normal_impulse_acc = new_jn;

            f32 vt1 = rel_vel(cc.t1);
            f32 vt2 = rel_vel(cc.t2);
            f32 jt1 = -vt1 * cc.eff_t1;
            f32 jt2 = -vt2 * cc.eff_t2;
            f32 jt1_new = c.friction_impulse_acc1 + jt1;
            f32 jt2_new = c.friction_impulse_acc2 + jt2;
            f32 limit   = cc.mu * c.normal_impulse_acc;
            f32 mag     = std::sqrt(jt1_new*jt1_new + jt2_new*jt2_new);
            if (mag > limit && mag > 1e-12f) {
                f32 s = limit / mag;
                jt1_new *= s;
                jt2_new *= s;
            }
            jt1 = jt1_new - c.friction_impulse_acc1;
            jt2 = jt2_new - c.friction_impulse_acc2;
            c.friction_impulse_acc1 = jt1_new;
            c.friction_impulse_acc2 = jt2_new;

            math::Vec3 P = math::add(math::add(math::mul(c.normal_world, jn),
                                                math::mul(cc.t1, jt1)),
                                      math::mul(cc.t2, jt2));
            A.linear_velocity  = math::sub(A.linear_velocity,  math::mul(P, A.inv_mass));
            B.linear_velocity  = math::add(B.linear_velocity,  math::mul(P, B.inv_mass));
            A.angular_velocity = math::sub(A.angular_velocity, apply_inv_inertia(A, math::cross(cc.ra, P)));
            B.angular_velocity = math::add(B.angular_velocity, apply_inv_inertia(B, math::cross(cc.rb, P)));
        }
    }

    for (u32 it = 0; it < params.position_iterations; ++it) {
        for (usize i = 0; i < contacts.size(); ++i) {
            Contact& c = contacts[i];
            CC& cc = caches[i];
            Body& A = bodies[c.body_a];
            Body& B = bodies[c.body_b];
            f32 pen = c.depth - params.slop;
            if (pen <= 0.0f) continue;
            f32 corr = params.baumgarte * pen * cc.eff_n;
            math::Vec3 P = math::mul(c.normal_world, corr);
            A.position = math::sub(A.position, math::mul(P, A.inv_mass));
            B.position = math::add(B.position, math::mul(P, B.inv_mass));
        }
    }
}

// ─── Single-axis SAP pass (header-inline, used by tests directly) ───────

struct AxisEndpoint {
    f32 v;
    u32 body_index;
    u8  is_max;
};

inline f32 axis_min(const AabbEntry& e, u32 axis) noexcept {
    return (axis == 0 ? e.min.x : axis == 1 ? e.min.y : e.min.z);
}
inline f32 axis_max(const AabbEntry& e, u32 axis) noexcept {
    return (axis == 0 ? e.max.x : axis == 1 ? e.max.y : e.max.z);
}

inline void kernel_sap_axis(const AabbEntry* aabbs, usize count, u32 axis,
                            std::vector<CandidatePair>& out_pairs) {
    out_pairs.clear();
    if (count < 2) return;

    std::vector<AxisEndpoint> endpoints;
    endpoints.reserve(count * 2);
    for (usize i = 0; i < count; ++i) {
        endpoints.push_back({axis_min(aabbs[i], axis), aabbs[i].body_index, 0});
        endpoints.push_back({axis_max(aabbs[i], axis), aabbs[i].body_index, 1});
    }
    std::sort(endpoints.begin(), endpoints.end(),
              [](const AxisEndpoint& a, const AxisEndpoint& b) {
                  if (a.v != b.v) return a.v < b.v;
                  return a.is_max < b.is_max;
              });

    std::vector<u32> active;
    active.reserve(64);
    for (const AxisEndpoint& e : endpoints) {
        if (!e.is_max) {
            for (u32 other : active) {
                u32 lo = std::min(other, e.body_index);
                u32 hi = std::max(other, e.body_index);
                if (lo != hi) out_pairs.push_back({lo, hi});
            }
            active.push_back(e.body_index);
        } else {
            for (auto it = active.begin(); it != active.end(); ++it) {
                if (*it == e.body_index) { active.erase(it); break; }
            }
        }
    }
    std::sort(out_pairs.begin(), out_pairs.end(),
              [](CandidatePair a, CandidatePair b) {
                  if (a.a != b.a) return a.a < b.a;
                  return a.b < b.b;
              });
    out_pairs.erase(std::unique(out_pairs.begin(), out_pairs.end()),
                    out_pairs.end());
}

}  // namespace psynder::physics::detail::kernels
