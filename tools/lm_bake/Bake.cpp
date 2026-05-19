// SPDX-License-Identifier: MIT
// Psynder — lm_bake: offline lightmap baker (Lane 24 / tools).

#include "Bake.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace psynder::tools::bake {

namespace fs = std::filesystem;

namespace {

template <class T>
void append_le(std::vector<u8>& out, T value) {
    using U = std::make_unsigned_t<T>;
    auto u = static_cast<U>(value);
    for (usize i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<u8>(u >> (8 * i)));
    }
}
template <class T>
bool read_le(std::span<const u8> bytes, usize off, T& out) {
    if (off + sizeof(T) > bytes.size()) return false;
    using U = std::make_unsigned_t<T>;
    U u = 0;
    for (usize i = 0; i < sizeof(T); ++i) u |= static_cast<U>(bytes[off + i]) << (8 * i);
    out = static_cast<T>(u);
    return true;
}

// Möller–Trumbore single-ray triangle intersect. Returns (hit, t).
struct TriHit {
    bool hit = false;
    f32  t   = 0.0f;
};
TriHit intersect_tri(math::Vec3 ro, math::Vec3 rd,
                     math::Vec3 v0, math::Vec3 v1, math::Vec3 v2,
                     f32 t_max) {
    constexpr f32 kEps = 1e-7f;
    math::Vec3 e1 = math::sub(v1, v0);
    math::Vec3 e2 = math::sub(v2, v0);
    math::Vec3 p  = math::cross(rd, e2);
    f32 det = math::dot(e1, p);
    if (std::fabs(det) < kEps) return {};
    f32 inv = 1.0f / det;
    math::Vec3 s = math::sub(ro, v0);
    f32 u = math::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return {};
    math::Vec3 q = math::cross(s, e1);
    f32 v = math::dot(rd, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return {};
    f32 t = math::dot(e2, q) * inv;
    if (t < kEps || t > t_max) return {};
    return { true, t };
}

bool occluded(const BakeScene& scene, math::Vec3 ro, math::Vec3 rd, f32 t_max, u32 self_tri) {
    for (u32 i = 0; i < scene.triangles.size(); ++i) {
        if (i == self_tri) continue;
        const auto& tri = scene.triangles[i];
        auto h = intersect_tri(ro, rd, tri.v0, tri.v1, tri.v2, t_max);
        if (h.hit) return true;
    }
    return false;
}

// Build an orthonormal basis aligned with the triangle plane: u, v, n.
struct TriBasis {
    math::Vec3 origin;
    math::Vec3 u_axis;
    math::Vec3 v_axis;
    math::Vec3 normal;
    f32        u_len;        // length of v1-v0
    f32        v_extent;     // length of perpendicular extent
};
TriBasis build_basis(const BakeTriangle& tri) {
    TriBasis b{};
    b.origin = tri.v0;
    math::Vec3 e1 = math::sub(tri.v1, tri.v0);
    math::Vec3 e2 = math::sub(tri.v2, tri.v0);
    math::Vec3 n = math::cross(e1, e2);
    f32 nlen = std::sqrt(math::dot(n, n));
    if (nlen > 1e-12f) n = math::mul(n, 1.0f / nlen);
    f32 e1_len = std::sqrt(math::dot(e1, e1));
    math::Vec3 u_axis = e1;
    if (e1_len > 1e-12f) u_axis = math::mul(u_axis, 1.0f / e1_len);
    // v_axis perpendicular to u within the plane.
    math::Vec3 v_axis = math::cross(n, u_axis);
    // v extent = e2 projected onto v_axis.
    f32 v_extent = math::dot(e2, v_axis);
    b.normal = n;
    b.u_axis = u_axis;
    b.v_axis = v_axis;
    b.u_len = e1_len;
    b.v_extent = std::fabs(v_extent);
    return b;
}

// For grid texel (i, j) ∈ [0, res), return its world-space position
// and barycentrics. We sample the triangle in (u, v) parameter space:
//   u = (i + 0.5) / res, v = (j + 0.5) / res, accept if u+v <= 1.
struct TexelSample {
    math::Vec3 world{};
    bool       inside = false;
    f32        u = 0, v = 0;
};
TexelSample sample_texel(const BakeTriangle& tri, u32 i, u32 j, u32 res) {
    TexelSample ts{};
    f32 u = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(res);
    f32 v = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(res);
    if (u + v > 1.0f) return ts;
    ts.inside = true;
    ts.u = u;
    ts.v = v;
    math::Vec3 e1 = math::sub(tri.v1, tri.v0);
    math::Vec3 e2 = math::sub(tri.v2, tri.v0);
    ts.world = math::add(math::add(tri.v0, math::mul(e1, u)), math::mul(e2, v));
    return ts;
}

}  // anon namespace

BakedSurface bake_triangle_direct(const BakeScene& scene, u32 ti, const BakeOptions& opt) {
    BakedSurface surf{};
    if (ti >= scene.triangles.size()) return surf;
    const BakeTriangle& tri = scene.triangles[ti];
    TriBasis basis = build_basis(tri);

    surf.width = opt.lightmap_resolution;
    surf.height = opt.lightmap_resolution;
    surf.pixels.assign(static_cast<usize>(surf.width) * surf.height * 3u, 0.0f);

    for (u32 j = 0; j < surf.height; ++j) {
        for (u32 i = 0; i < surf.width; ++i) {
            TexelSample ts = sample_texel(tri, i, j, opt.lightmap_resolution);
            usize px = (static_cast<usize>(j) * surf.width + i) * 3u;
            if (!ts.inside) continue;
            math::Vec3 p = math::add(ts.world, math::mul(basis.normal, opt.ray_epsilon));

            // Loop over lights. Direct illumination Lambertian: I/r² * max(N·L, 0).
            f32 r = 0, g = 0, b = 0;
            for (const auto& light : scene.lights) {
                math::Vec3 L{};
                f32 t_max = 0;
                f32 atten = 0;
                if (light.kind == LightKind::kPoint) {
                    L = math::sub(light.position, p);
                    f32 dist = std::sqrt(math::dot(L, L));
                    if (dist < 1e-6f) continue;
                    L = math::mul(L, 1.0f / dist);
                    t_max = dist;
                    atten = light.intensity / (dist * dist);
                } else {
                    // Directional: `direction` points along the light's
                    // travel; flip for the incoming vector.
                    L = math::mul(light.direction, -1.0f);
                    f32 ln = std::sqrt(math::dot(L, L));
                    if (ln > 1e-9f) L = math::mul(L, 1.0f / ln);
                    t_max = 1e6f;
                    atten = light.intensity;
                }
                f32 ndotl = std::max(0.0f, math::dot(basis.normal, L));
                if (ndotl <= 0.0f) continue;
                if (occluded(scene, p, L, t_max, ti)) continue;
                r += atten * ndotl * light.color.x;
                g += atten * ndotl * light.color.y;
                b += atten * ndotl * light.color.z;
            }
            surf.pixels[px + 0] = r;
            surf.pixels[px + 1] = g;
            surf.pixels[px + 2] = b;
        }
    }
    return surf;
}

BakedAtlas bake(const BakeScene& scene, const BakeOptions& opt) {
    BakedAtlas atlas;
    atlas.surfaces.reserve(scene.triangles.size());
    for (u32 i = 0; i < scene.triangles.size(); ++i) {
        atlas.surfaces.push_back(bake_triangle_direct(scene, i, opt));
    }
    // Indirect bounces: stub (Wave-B drops in path-traced gather).
    (void)opt.max_indirect_bounces;
    return atlas;
}

// ─── f32 ↔ f16 ───────────────────────────────────────────────────────────

u16 f32_to_f16(f32 v) noexcept {
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    u32 sign = (bits >> 31) & 1u;
    i32 exp  = static_cast<i32>((bits >> 23) & 0xFFu) - 127;
    u32 mant = bits & 0x7FFFFFu;

    if (exp == 128) {                                  // Inf / NaN
        return static_cast<u16>((sign << 15) | 0x7C00u | (mant ? 0x0200u : 0u));
    }
    if (exp > 15) {                                    // overflow to inf
        return static_cast<u16>((sign << 15) | 0x7C00u);
    }
    if (exp >= -14) {                                  // normalized
        return static_cast<u16>((sign << 15) | (static_cast<u32>(exp + 15) << 10) | (mant >> 13));
    }
    if (exp >= -25) {                                  // subnormal
        mant |= 0x800000u;
        u32 shift = static_cast<u32>(-14 - exp + 13);
        return static_cast<u16>((sign << 15) | (mant >> shift));
    }
    return static_cast<u16>(sign << 15);              // underflow to zero
}

f32 f16_to_f32(u16 h) noexcept {
    u32 sign = (h >> 15) & 1u;
    u32 exp  = (h >> 10) & 0x1Fu;
    u32 mant = h & 0x3FFu;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign << 31; }
        else {
            // subnormal -> normalize
            int e = -14;
            while ((mant & 0x400u) == 0) { mant <<= 1; --e; }
            mant &= 0x3FFu;
            bits = (sign << 31) | (static_cast<u32>(e + 127) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = (sign << 31) | 0x7F800000u | (mant << 13);
    } else {
        bits = (sign << 31) | (static_cast<u32>(exp + (127 - 15)) << 23) | (mant << 13);
    }
    f32 out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// ─── .lmlight ────────────────────────────────────────────────────────────

void write_lmlight(const BakedAtlas& atlas, std::vector<u8>& out) {
    out.clear();
    append_le<u32>(out, kLmlMagic);
    append_le<u32>(out, kLmlVersion);
    append_le<u32>(out, static_cast<u32>(atlas.surfaces.size()));
    append_le<u32>(out, 0);  // reserved

    for (const auto& s : atlas.surfaces) {
        append_le<u32>(out, s.width);
        append_le<u32>(out, s.height);
        for (f32 px : s.pixels) {
            u16 h = f32_to_f16(px);
            append_le<u16>(out, h);
        }
    }
}

bool read_lmlight(std::span<const u8> bytes, BakedAtlas& out, std::string* err) {
    auto fail = [&](const char* msg) { if (err) *err = msg; return false; };
    if (bytes.size() < 16) return fail("lmlight header truncated");
    u32 magic = 0; read_le<u32>(bytes, 0, magic);
    if (magic != kLmlMagic) return fail("lmlight bad magic");
    u32 version = 0; read_le<u32>(bytes, 4, version);
    if (version != kLmlVersion) return fail("lmlight unsupported version");
    u32 count = 0; read_le<u32>(bytes, 8, count);
    usize cursor = 16;
    out.surfaces.clear(); out.surfaces.resize(count);
    for (u32 i = 0; i < count; ++i) {
        u32 w = 0, h = 0;
        if (!read_le<u32>(bytes, cursor + 0, w)) return fail("lmlight surf header");
        if (!read_le<u32>(bytes, cursor + 4, h)) return fail("lmlight surf header");
        cursor += 8;
        BakedSurface s;
        s.width = w; s.height = h;
        s.pixels.resize(static_cast<usize>(w) * h * 3u);
        for (usize px = 0; px < s.pixels.size(); ++px) {
            u16 hv = 0;
            if (!read_le<u16>(bytes, cursor, hv)) return fail("lmlight pixel truncated");
            s.pixels[px] = f16_to_f32(hv);
            cursor += 2;
        }
        out.surfaces[i] = std::move(s);
    }
    return true;
}

// ─── CLI ─────────────────────────────────────────────────────────────────

void print_help() {
    std::fprintf(stdout,
        "lm_bake — Psynder offline lightmap baker\n"
        "\n"
        "Usage:\n"
        "  lm_bake <scene.psyscene> <out.lmlight> [--resolution N] [--bounces N]\n"
        "  lm_bake --help\n"
        "\n"
        "Wave-A bakes direct lighting only; multi-bounce indirect comes\n"
        "in Wave-B once the path-traced gather lands.\n"
        "\n"
        ".psyscene is a tiny ad-hoc text format (see Bake.cpp::parse_scene)\n"
        "good for round-trip tests. Real scenes will come out of lm_qbsp +\n"
        "lm_cook in Wave-B.\n");
}

namespace {

// A very small text scene format for CLI testing. Each line is one of:
//
//   tri V0X V0Y V0Z V1X V1Y V1Z V2X V2Y V2Z
//   light_point X Y Z R G B INTENSITY
//   light_dir   DX DY DZ R G B INTENSITY
//
// Comments start with #. This is intentionally minimal — production scenes
// flow through .psybsp / .lmm assets and the engine, not this CLI shim.
bool parse_scene(std::string_view text, BakeScene& out, std::string& err) {
    usize i = 0;
    while (i < text.size()) {
        usize end = text.find('\n', i);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(i, end - i);
        i = end + 1;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty() || line.front() == '#') continue;

        std::vector<std::string> tok;
        usize p = 0;
        while (p < line.size()) {
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r')) ++p;
            usize start = p;
            while (p < line.size() && line[p] != ' ' && line[p] != '\t' && line[p] != '\r') ++p;
            if (p > start) tok.emplace_back(line.substr(start, p - start));
        }
        if (tok.empty()) continue;
        auto& cmd = tok[0];
        auto pf = [](const std::string& s, f32& v) {
            char* e = nullptr;
            v = std::strtof(s.c_str(), &e);
            return e != s.c_str();
        };
        if (cmd == "tri" && tok.size() >= 10) {
            BakeTriangle t{};
            f32 vals[9];
            for (int k = 0; k < 9; ++k) {
                if (!pf(tok[1 + k], vals[k])) { err = "bake: bad tri"; return false; }
            }
            t.v0 = {vals[0], vals[1], vals[2]};
            t.v1 = {vals[3], vals[4], vals[5]};
            t.v2 = {vals[6], vals[7], vals[8]};
            t.normal = math::normalize(math::cross(math::sub(t.v1, t.v0), math::sub(t.v2, t.v0)));
            out.triangles.push_back(t);
        } else if (cmd == "light_point" && tok.size() >= 8) {
            BakeLight l{};
            l.kind = LightKind::kPoint;
            f32 v[7];
            for (int k = 0; k < 7; ++k) if (!pf(tok[1 + k], v[k])) { err = "bake: bad point light"; return false; }
            l.position = {v[0], v[1], v[2]};
            l.color    = {v[3], v[4], v[5]};
            l.intensity = v[6];
            out.lights.push_back(l);
        } else if (cmd == "light_dir" && tok.size() >= 8) {
            BakeLight l{};
            l.kind = LightKind::kDirectional;
            f32 v[7];
            for (int k = 0; k < 7; ++k) if (!pf(tok[1 + k], v[k])) { err = "bake: bad dir light"; return false; }
            l.direction = math::normalize(math::Vec3{v[0], v[1], v[2]});
            l.color     = {v[3], v[4], v[5]};
            l.intensity = v[6];
            out.lights.push_back(l);
        } else {
            // ignore unknown — forward-compat.
        }
    }
    return true;
}

bool read_file(const fs::path& p, std::string& out, std::string& err) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { err = "cannot open " + p.string(); return false; }
    in.seekg(0, std::ios::end);
    out.resize(static_cast<usize>(in.tellg()));
    in.seekg(0, std::ios::beg);
    if (!out.empty()) in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(in);
}
bool write_file(const fs::path& p, std::span<const u8> data, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot write " + p.string(); return false; }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

}  // anon

int cli_main(int argc, char** argv) {
    if (argc < 2) { print_help(); return 1; }
    std::string_view a = argv[1];
    if (a == "--help" || a == "-h" || a == "help") { print_help(); return 0; }
    if (argc < 3) { print_help(); return 1; }
    BakeOptions opt;
    for (int i = 3; i < argc; ++i) {
        std::string_view k = argv[i];
        if (k == "--resolution" && i + 1 < argc) opt.lightmap_resolution = static_cast<u32>(std::atoi(argv[++i]));
        else if (k == "--bounces" && i + 1 < argc) opt.max_indirect_bounces = static_cast<u32>(std::atoi(argv[++i]));
    }
    std::string text, err;
    if (!read_file(fs::path(argv[1]), text, err)) {
        std::fprintf(stderr, "lm_bake: %s\n", err.c_str());
        return 1;
    }
    BakeScene scene;
    if (!parse_scene(text, scene, err)) {
        std::fprintf(stderr, "lm_bake: %s\n", err.c_str());
        return 1;
    }
    BakedAtlas atlas = bake(scene, opt);
    std::vector<u8> bytes;
    write_lmlight(atlas, bytes);
    if (!write_file(fs::path(argv[2]), bytes, err)) {
        std::fprintf(stderr, "lm_bake: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stdout, "lm_bake: baked %u surfaces -> %s\n",
                 static_cast<u32>(atlas.surfaces.size()), argv[2]);
    return 0;
}

}  // namespace psynder::tools::bake
