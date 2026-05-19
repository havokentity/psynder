// SPDX-License-Identifier: MIT
// Psynder — curve-brush tessellation. Lane 24 / tools (Wave-B).
//
// Cylinders and spheres are emitted as single convex brushes (the BSP
// compiler only handles intersections of half-spaces, which forces a
// convex partition anyway). For a cylinder we emit N rectangular sides +
// 2 caps; for a sphere we emit the faces of an icosahedron, optionally
// subdivided. Tangent-to-sphere planes are convex, so the result is a
// valid Quake-style brush.
//
// Spheres and cylinders are common in TrenchBroom 2 / J.A.C.K. as
// "primitive shapes". The exporter normally writes them as decomposed
// convex brushes; this preprocessor lets a hand-written .map declare the
// primitive directly via `@cylinder` / `@sphere` directives that we expand
// before forwarding to qbsp::parse_map.

#include "MapImport.h"

#include "math/Math.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::tools::mapimport {

namespace {

// Write a single .map face line. The Quake convention: three CCW points on
// the outward-facing side of the plane, then `material u_off v_off rot
// u_scale v_scale`. We always emit `0 0 0 1 1` for the tex params.
void emit_face(std::ostringstream& out,
               math::Vec3 a, math::Vec3 b, math::Vec3 c,
               std::string_view material) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %.*s 0 0 0 1 1\n",
        static_cast<double>(a.x), static_cast<double>(a.y), static_cast<double>(a.z),
        static_cast<double>(b.x), static_cast<double>(b.y), static_cast<double>(b.z),
        static_cast<double>(c.x), static_cast<double>(c.y), static_cast<double>(c.z),
        static_cast<int>(material.size()), material.data());
    out << buf;
}

// Build the tangent frame {u, v} perpendicular to `axis` (unit length).
void build_tangent_frame(math::Vec3 axis, math::Vec3& out_u, math::Vec3& out_v) {
    math::Vec3 a = (std::fabs(axis.x) > 0.9f) ? math::Vec3{0, 1, 0} : math::Vec3{1, 0, 0};
    out_u = math::cross(axis, a);
    f32 ul = std::sqrt(math::dot(out_u, out_u));
    if (ul > 1e-9f) out_u = math::mul(out_u, 1.0f / ul);
    out_v = math::cross(axis, out_u);
}

// Icosahedron base vertices (φ = (1 + √5) / 2, unit radius after normalize).
// Used for sphere tessellation. 12 verts / 20 tris.
void make_icosahedron(std::vector<math::Vec3>& verts,
                      std::vector<std::array<u32,3>>& tris) {
    constexpr f32 t = 1.61803398875f;
    verts = {
        {-1,  t,  0}, { 1,  t,  0}, {-1, -t,  0}, { 1, -t,  0},
        { 0, -1,  t}, { 0,  1,  t}, { 0, -1, -t}, { 0,  1, -t},
        { t,  0, -1}, { t,  0,  1}, {-t,  0, -1}, {-t,  0,  1},
    };
    for (auto& v : verts) {
        f32 l = std::sqrt(math::dot(v, v));
        v = math::mul(v, 1.0f / l);
    }
    tris = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1},
    };
}

// Subdivide an icosahedron `n` times: each triangle becomes 4 triangles by
// splitting edges at the midpoint (renormalized to the sphere surface).
void subdivide_icosahedron(std::vector<math::Vec3>& verts,
                           std::vector<std::array<u32,3>>& tris,
                           u32 levels) {
    for (u32 lvl = 0; lvl < levels; ++lvl) {
        std::vector<std::array<u32,3>> next;
        next.reserve(tris.size() * 4);
        for (const auto& tri : tris) {
            u32 a = tri[0], b = tri[1], c = tri[2];
            math::Vec3 m_ab = math::mul(math::add(verts[a], verts[b]), 0.5f);
            math::Vec3 m_bc = math::mul(math::add(verts[b], verts[c]), 0.5f);
            math::Vec3 m_ca = math::mul(math::add(verts[c], verts[a]), 0.5f);
            f32 l_ab = std::sqrt(math::dot(m_ab, m_ab));
            f32 l_bc = std::sqrt(math::dot(m_bc, m_bc));
            f32 l_ca = std::sqrt(math::dot(m_ca, m_ca));
            m_ab = (l_ab > 1e-9f) ? math::mul(m_ab, 1.0f / l_ab) : m_ab;
            m_bc = (l_bc > 1e-9f) ? math::mul(m_bc, 1.0f / l_bc) : m_bc;
            m_ca = (l_ca > 1e-9f) ? math::mul(m_ca, 1.0f / l_ca) : m_ca;
            u32 i_ab = static_cast<u32>(verts.size()); verts.push_back(m_ab);
            u32 i_bc = static_cast<u32>(verts.size()); verts.push_back(m_bc);
            u32 i_ca = static_cast<u32>(verts.size()); verts.push_back(m_ca);
            next.push_back({a, i_ab, i_ca});
            next.push_back({b, i_bc, i_ab});
            next.push_back({c, i_ca, i_bc});
            next.push_back({i_ab, i_bc, i_ca});
        }
        tris.swap(next);
    }
}

}  // anon namespace

std::string tessellate_cylinder(const CurveCylinder& c) {
    constexpr f32 kTwoPi = 2.0f * std::numbers::pi_v<f32>;
    std::ostringstream out;

    math::Vec3 axis = c.axis;
    f32 axis_len = std::sqrt(math::dot(axis, axis));
    if (axis_len > 1e-9f) axis = math::mul(axis, 1.0f / axis_len);

    math::Vec3 u_dir, v_dir;
    build_tangent_frame(axis, u_dir, v_dir);

    u32 segs = std::max<u32>(3u, c.segments);
    f32 r = std::max(c.radius, 1e-3f);
    f32 h = std::max(c.height, 1e-3f);

    math::Vec3 base = c.origin;
    math::Vec3 top  = math::add(c.origin, math::mul(axis, h));

    // Bottom cap (normal = -axis). Three CCW points seen from outside (i.e.
    // from below the base): looking from -axis side, the cap winds CW around
    // +axis. Pick three corner points on the base ring.
    {
        math::Vec3 p0 = math::add(base, math::mul(u_dir, r));
        f32 a1 = kTwoPi / static_cast<f32>(segs);
        math::Vec3 p1 = math::add(base, math::add(math::mul(u_dir, r * std::cos(a1)),
                                                  math::mul(v_dir, r * std::sin(a1))));
        f32 a2 = 2.0f * a1;
        math::Vec3 p2 = math::add(base, math::add(math::mul(u_dir, r * std::cos(a2)),
                                                  math::mul(v_dir, r * std::sin(a2))));
        emit_face(out, p0, p2, p1, c.material);   // reversed → normal = -axis
    }
    // Top cap (normal = +axis).
    {
        math::Vec3 p0 = math::add(top, math::mul(u_dir, r));
        f32 a1 = kTwoPi / static_cast<f32>(segs);
        math::Vec3 p1 = math::add(top, math::add(math::mul(u_dir, r * std::cos(a1)),
                                                 math::mul(v_dir, r * std::sin(a1))));
        f32 a2 = 2.0f * a1;
        math::Vec3 p2 = math::add(top, math::add(math::mul(u_dir, r * std::cos(a2)),
                                                 math::mul(v_dir, r * std::sin(a2))));
        emit_face(out, p0, p1, p2, c.material);
    }
    // Side faces.
    for (u32 i = 0; i < segs; ++i) {
        f32 a0 = kTwoPi * static_cast<f32>(i)         / static_cast<f32>(segs);
        f32 a1 = kTwoPi * static_cast<f32>((i + 1) % segs) / static_cast<f32>(segs);
        math::Vec3 du0 = math::add(math::mul(u_dir, r * std::cos(a0)),
                                   math::mul(v_dir, r * std::sin(a0)));
        math::Vec3 du1 = math::add(math::mul(u_dir, r * std::cos(a1)),
                                   math::mul(v_dir, r * std::sin(a1)));
        math::Vec3 b0 = math::add(base, du0);
        math::Vec3 b1 = math::add(base, du1);
        math::Vec3 t0 = math::add(top, du0);
        // Outward CCW (seen from outside): b0 → b1 → t0.
        emit_face(out, b0, b1, t0, c.material);
    }
    return out.str();
}

std::string tessellate_sphere(const CurveSphere& s) {
    std::ostringstream out;
    f32 r = std::max(s.radius, 1e-3f);
    std::vector<math::Vec3> verts;
    std::vector<std::array<u32,3>> tris;
    make_icosahedron(verts, tris);
    subdivide_icosahedron(verts, tris, s.subdivisions);
    for (auto& v : verts) v = math::add(s.origin, math::mul(v, r));
    for (const auto& tri : tris) {
        emit_face(out, verts[tri[0]], verts[tri[1]], verts[tri[2]], s.material);
    }
    return out.str();
}

namespace {

// Find the next non-whitespace char at or after `p`. Returns size on EOF.
usize skip_ws(std::string_view s, usize p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
    return p;
}

// Try to parse a `@cylinder ...` or `@sphere ...` directive starting at `p`.
// On success returns the position just past the directive line.
bool try_parse_cylinder(std::string_view s, usize& p, std::string& expansion) {
    constexpr std::string_view kKey = "@cylinder";
    if (s.size() - p < kKey.size()) return false;
    if (s.substr(p, kKey.size()) != kKey) return false;
    p += kKey.size();
    // Read up to end-of-line.
    usize line_end = s.find('\n', p);
    if (line_end == std::string_view::npos) line_end = s.size();
    std::string_view args = s.substr(p, line_end - p);
    p = line_end;

    // Tokenise.
    std::vector<std::string> tok;
    usize tp = 0;
    while (tp < args.size()) {
        while (tp < args.size() && (args[tp] == ' ' || args[tp] == '\t' || args[tp] == '\r')) ++tp;
        usize start = tp;
        while (tp < args.size() && args[tp] != ' ' && args[tp] != '\t' && args[tp] != '\r') ++tp;
        if (tp > start) tok.emplace_back(args.substr(start, tp - start));
    }
    // Need 9 args: cx cy cz ax ay az radius height segments [material]
    if (tok.size() < 9) return false;
    auto pf = [](const std::string& v, f32& out) {
        char* e = nullptr;
        out = std::strtof(v.c_str(), &e);
        return e != v.c_str();
    };
    CurveCylinder c;
    f32 fv[8];
    for (usize i = 0; i < 8; ++i) if (!pf(tok[i], fv[i])) return false;
    c.origin = { fv[0], fv[1], fv[2] };
    c.axis   = { fv[3], fv[4], fv[5] };
    c.radius = fv[6];
    c.height = fv[7];
    c.segments = static_cast<u32>(std::strtol(tok[8].c_str(), nullptr, 10));
    if (tok.size() >= 10) c.material = tok[9];
    expansion = tessellate_cylinder(c);
    return true;
}

bool try_parse_sphere(std::string_view s, usize& p, std::string& expansion) {
    constexpr std::string_view kKey = "@sphere";
    if (s.size() - p < kKey.size()) return false;
    if (s.substr(p, kKey.size()) != kKey) return false;
    p += kKey.size();
    usize line_end = s.find('\n', p);
    if (line_end == std::string_view::npos) line_end = s.size();
    std::string_view args = s.substr(p, line_end - p);
    p = line_end;
    std::vector<std::string> tok;
    usize tp = 0;
    while (tp < args.size()) {
        while (tp < args.size() && (args[tp] == ' ' || args[tp] == '\t' || args[tp] == '\r')) ++tp;
        usize start = tp;
        while (tp < args.size() && args[tp] != ' ' && args[tp] != '\t' && args[tp] != '\r') ++tp;
        if (tp > start) tok.emplace_back(args.substr(start, tp - start));
    }
    // 5 args: cx cy cz radius subdivisions [material]
    if (tok.size() < 5) return false;
    auto pf = [](const std::string& v, f32& out) {
        char* e = nullptr;
        out = std::strtof(v.c_str(), &e);
        return e != v.c_str();
    };
    CurveSphere c;
    f32 fv[4];
    for (usize i = 0; i < 4; ++i) if (!pf(tok[i], fv[i])) return false;
    c.origin = { fv[0], fv[1], fv[2] };
    c.radius = fv[3];
    c.subdivisions = static_cast<u32>(std::strtol(tok[4].c_str(), nullptr, 10));
    if (tok.size() >= 6) c.material = tok[5];
    expansion = tessellate_sphere(c);
    return true;
}

}  // anon namespace

std::string expand_curve_brushes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    usize p = 0;
    while (p < text.size()) {
        // Look ahead for either '@cylinder' or '@sphere' at the current
        // non-whitespace token position. The scan is line-based; copy
        // bytes verbatim unless we land on a directive.
        usize line_start = p;
        // Skip leading whitespace.
        usize first_nws = skip_ws(text, p);
        if (first_nws < text.size() && text[first_nws] == '@') {
            usize probe = first_nws;
            std::string expansion;
            if (try_parse_cylinder(text, probe, expansion) ||
                try_parse_sphere(text, probe, expansion)) {
                out.append(expansion);
                p = probe;
                if (p < text.size() && text[p] == '\n') ++p;
                continue;
            }
        }
        // Copy this line verbatim.
        usize line_end = text.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = text.size();
        out.append(text.substr(line_start, line_end - line_start));
        if (line_end < text.size()) {
            out.push_back('\n');
            p = line_end + 1;
        } else {
            p = line_end;
        }
    }
    return out;
}

}  // namespace psynder::tools::mapimport
