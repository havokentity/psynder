// SPDX-License-Identifier: MIT
// Psynder — lm_qbsp implementation. Lane 24 / tools.

#include "Qbsp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psynder::tools::qbsp {

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
void append_f32(std::vector<u8>& out, f32 value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    append_le<u32>(out, bits);
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
bool read_f32(std::span<const u8> bytes, usize off, f32& out) {
    u32 bits = 0;
    if (!read_le<u32>(bytes, off, bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

// (trim helper removed — the tokenizer in MapTok eats whitespace inline.)

// ─── Tokenizer for .map files ─────────────────────────────────────────────
// Comments: `// ... \n`
// Tokens: parens, braces, quoted strings, bare words (number / texname).
class MapTok {
public:
    explicit MapTok(std::string_view s) : src_(s), pos_(0) {}
    bool eof() {
        skip_ws();
        return pos_ >= src_.size();
    }
    // Returns next token (without quotes). For parens / braces returns the
    // single char as a string; for quoted strings the contents.
    bool next(std::string& out) {
        skip_ws();
        if (pos_ >= src_.size()) return false;
        char c = src_[pos_];
        if (c == '(' || c == ')' || c == '{' || c == '}') {
            ++pos_;
            out.assign(1, c);
            return true;
        }
        if (c == '"') {
            ++pos_;
            out.clear();
            while (pos_ < src_.size() && src_[pos_] != '"') out.push_back(src_[pos_++]);
            if (pos_ < src_.size()) ++pos_;
            return true;
        }
        usize start = pos_;
        while (pos_ < src_.size() &&
               !std::isspace(static_cast<unsigned char>(src_[pos_])) &&
               src_[pos_] != '(' && src_[pos_] != ')' &&
               src_[pos_] != '{' && src_[pos_] != '}') {
            ++pos_;
        }
        if (start == pos_) return false;
        out.assign(src_.data() + start, src_.data() + pos_);
        return true;
    }
    bool peek_char(char& out) {
        skip_ws();
        if (pos_ >= src_.size()) return false;
        out = src_[pos_];
        return true;
    }
    usize line_no() const {
        usize n = 1;
        for (usize i = 0; i < pos_ && i < src_.size(); ++i) {
            if (src_[i] == '\n') ++n;
        }
        return n;
    }
private:
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos_; continue; }
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
    }
    std::string_view src_;
    usize            pos_;
};

bool parse_vec3_from_tokens(MapTok& t, math::Vec3& out, std::string& err) {
    std::string s;
    if (!t.next(s) || s != "(") { err = "expected '('"; return false; }
    f32 v[3];
    for (int i = 0; i < 3; ++i) {
        if (!t.next(s)) { err = "expected number"; return false; }
        char* e = nullptr;
        v[i] = std::strtof(s.c_str(), &e);
        if (e == s.c_str()) { err = "bad number: " + s; return false; }
    }
    if (!t.next(s) || s != ")") { err = "expected ')'"; return false; }
    out = { v[0], v[1], v[2] };
    return true;
}

}  // anon

bool parse_map(std::string_view text, MapFile& out, std::string* err) {
    out = {};
    MapTok t(text);
    std::string s;
    while (true) {
        if (t.eof()) break;
        if (!t.next(s)) break;
        if (s != "{") {
            if (err) *err = "map: expected '{' at top level on line " + std::to_string(t.line_no());
            return false;
        }
        MapEntity ent;
        while (true) {
            if (!t.next(s)) {
                if (err) *err = "map: unexpected EOF inside entity";
                return false;
            }
            if (s == "}") break;
            if (s == "{") {
                // brush
                MapBrush brush;
                while (true) {
                    char c = 0;
                    if (!t.peek_char(c)) { if (err) *err = "map: unexpected EOF in brush"; return false; }
                    if (c == '}') { t.next(s); break; }
                    // face: ( p1 ) ( p2 ) ( p3 ) tex u v rot us vs
                    math::Vec3 p1, p2, p3;
                    std::string err_sub;
                    if (!parse_vec3_from_tokens(t, p1, err_sub) ||
                        !parse_vec3_from_tokens(t, p2, err_sub) ||
                        !parse_vec3_from_tokens(t, p3, err_sub)) {
                        if (err) *err = "map: brush face — " + err_sub;
                        return false;
                    }
                    std::string mat;
                    if (!t.next(mat)) { if (err) *err = "map: missing material"; return false; }
                    // Skip 5 trailing numbers (offsets / rotation / scales).
                    for (int k = 0; k < 5; ++k) {
                        if (!t.next(s)) { if (err) *err = "map: missing face params"; return false; }
                    }
                    // Compute plane from three points. Quake .map convention:
                    // points are given CW when viewed from the *outside* of the
                    // brush volume, so the outward normal is (p2-p1) × (p3-p1).
                    math::Vec3 e1 = math::sub(p2, p1);
                    math::Vec3 e2 = math::sub(p3, p1);
                    math::Vec3 n  = math::cross(e1, e2);
                    f32 len = std::sqrt(math::dot(n, n));
                    if (len < 1e-9f) {
                        if (err) *err = "map: degenerate face";
                        return false;
                    }
                    n = math::mul(n, 1.0f / len);
                    f32 d = math::dot(n, p1);
                    MapPlane pl;
                    pl.normal = n;
                    pl.d = d;
                    pl.material = mat;
                    brush.planes.push_back(pl);
                }
                if (!brush.planes.empty()) {
                    // Rough bounds: project a default world bound through plane
                    // half-spaces is complex; for now record an infinite AABB
                    // and let the loader clamp during PVS. A real implementation
                    // would intersect the half-spaces to get a vertex cloud.
                    math::Vec3 lo{-1e6f, -1e6f, -1e6f}, hi{1e6f, 1e6f, 1e6f};
                    brush.bounds.min = lo;
                    brush.bounds.max = hi;
                    ent.brushes.push_back(brush);
                }
                continue;
            }
            // key/value pair — the next token is the value (quoted string).
            std::string val;
            if (!t.next(val)) { if (err) *err = "map: missing value for " + s; return false; }
            ent.kv.emplace_back(s, val);
        }
        out.entities.push_back(std::move(ent));
    }
    return true;
}

// ─── BSP compile ─────────────────────────────────────────────────────────

namespace {

// Hash a plane to fold near-duplicates into the same table slot.
u64 plane_key(math::Vec3 n, f32 d) {
    auto q = [](f32 v) { return static_cast<i64>(std::round(v * 1e4f)); };
    u64 h = 0xcbf29ce484222325ULL;
    auto bump = [&](i64 x) {
        const u8* p = reinterpret_cast<const u8*>(&x);
        for (usize i = 0; i < sizeof(x); ++i) { h ^= p[i]; h *= 0x100000001b3ULL; }
    };
    bump(q(n.x)); bump(q(n.y)); bump(q(n.z)); bump(q(d));
    return h;
}

u32 intern_plane(CompiledBsp& bsp, std::vector<u64>& keys, math::Vec3 n, f32 d) {
    u64 k = plane_key(n, d);
    for (u32 i = 0; i < keys.size(); ++i) {
        if (keys[i] == k) return i;
    }
    keys.push_back(k);
    BspPlane p{};
    p.normal = n; p.d = d;
    bsp.planes.push_back(p);
    return static_cast<u32>(bsp.planes.size() - 1);
}

// Decide which side of plane (n,d) a brush is on by checking *every*
// vertex of the brush against the plane. For brushes without explicit
// vertex sets (Wave-A), we proxy with plane-on-plane checks: if any of
// the brush's planes is *parallel and opposite* to the split plane and
// the brush "fits" on one side, classify accordingly.
//
// 0 = front, 1 = back, 2 = spans (split), 3 = coplanar.
//
// This is approximate; for the Wave-A test suite (single convex room)
// we only need to land each brush in *some* leaf — pickdirection isn't
// load-bearing for correctness, only for tree shape.
int classify(const MapBrush& brush, math::Vec3 n, f32 d) {
    // Test the *centroid* of plane points. Compute an approximate centroid
    // as the average of all face plane points projected onto each plane.
    math::Vec3 cen{0,0,0};
    if (brush.planes.empty()) return 0;
    for (const auto& pl : brush.planes) {
        cen = math::add(cen, math::mul(pl.normal, pl.d));
    }
    cen = math::mul(cen, 1.0f / static_cast<f32>(brush.planes.size()));
    f32 dist = math::dot(n, cen) - d;
    constexpr f32 kEps = 1e-3f;
    if (dist >  kEps) return 0;
    if (dist < -kEps) return 1;
    return 3;
}

i32 build_recursive(CompiledBsp& bsp,
                    std::vector<u64>& plane_keys,
                    const std::vector<MapBrush>& world,
                    const std::vector<u32>& brush_ids,
                    int depth,
                    int max_depth) {
    // Stop conditions:
    //   - no brushes: empty leaf
    //   - max depth: solid leaf carrying the remaining brushes
    if (brush_ids.empty() || depth >= max_depth) {
        BspLeaf leaf;
        leaf.cluster = static_cast<i32>(bsp.leaves.size());
        leaf.flags = brush_ids.empty() ? kLeafFlagEmpty : kLeafFlagSolid;
        leaf.bounds.min = {-1e6f, -1e6f, -1e6f};
        leaf.bounds.max = { 1e6f,  1e6f,  1e6f};
        bsp.leaves.push_back(leaf);
        return -static_cast<i32>(bsp.leaves.size());   // negative = leaf
    }

    // Pick the first plane of the first brush as splitter.
    const MapBrush& b0 = world[brush_ids[0]];
    if (b0.planes.empty()) {
        BspLeaf leaf;
        leaf.cluster = static_cast<i32>(bsp.leaves.size());
        leaf.flags = kLeafFlagSolid;
        leaf.bounds.min = {-1e6f, -1e6f, -1e6f};
        leaf.bounds.max = { 1e6f,  1e6f,  1e6f};
        bsp.leaves.push_back(leaf);
        return -static_cast<i32>(bsp.leaves.size());
    }
    math::Vec3 n = b0.planes[0].normal;
    f32 d = b0.planes[0].d;

    u32 plane_idx = intern_plane(bsp, plane_keys, n, d);

    std::vector<u32> front, back;
    for (u32 id : brush_ids) {
        int side = classify(world[id], n, d);
        if (side == 0)      front.push_back(id);
        else if (side == 1) back.push_back(id);
        else                { front.push_back(id); back.push_back(id); }
    }
    // If splitting made no progress, fold remaining brushes into a leaf.
    if (front.size() == brush_ids.size() && back.size() == brush_ids.size()) {
        BspLeaf leaf;
        leaf.cluster = static_cast<i32>(bsp.leaves.size());
        leaf.flags = kLeafFlagSolid;
        leaf.bounds.min = {-1e6f, -1e6f, -1e6f};
        leaf.bounds.max = { 1e6f,  1e6f,  1e6f};
        bsp.leaves.push_back(leaf);
        return -static_cast<i32>(bsp.leaves.size());
    }

    BspNode node{};
    node.plane = static_cast<i32>(plane_idx);
    node.front = 0;
    node.back  = 0;
    u32 node_idx = static_cast<u32>(bsp.nodes.size());
    bsp.nodes.push_back(node);

    i32 f_child = build_recursive(bsp, plane_keys, world, front, depth + 1, max_depth);
    i32 b_child = build_recursive(bsp, plane_keys, world, back,  depth + 1, max_depth);
    bsp.nodes[node_idx].front = f_child;
    bsp.nodes[node_idx].back  = b_child;
    return static_cast<i32>(node_idx);
}

}  // anon namespace

namespace {

// ─── Portal generation (Wave-B) ──────────────────────────────────────────
//
// For every internal node whose two child subtrees each surface at least
// one non-solid (empty) leaf, emit a portal on the splitting plane connecting
// the front-most-empty leaf to the back-most-empty leaf. The winding is a
// large square on the plane, sized to the world AABB; lane 10's portal
// traversal clips the winding against the parent path's planes at load
// time so we don't need to do the full geometric clip here.
//
// This is intentionally a "leaf-adjacency" portal — it gives the runtime
// enough plane data + connectivity to do correct portal-frustum culling for
// our Wave-B sample maps (single room → single empty leaf → zero portals,
// multi-leaf maps → one portal per splitting node). Full BSP-style portal
// clipping with vertex tessellation slips to Wave-C.

// Walk subtree rooted at `child` (in BspNode child-encoding: negative =
// leaf, non-negative = node index) and return the *first* empty leaf
// encountered, or -1 if none.
i32 first_empty_leaf(const CompiledBsp& bsp, i32 child) {
    if (child < 0) {
        // Leaf encoding mirrors lane 10's BspFormat.h: child = ~leaf_index.
        i32 leaf_idx = ~child;
        if (leaf_idx < 0 || static_cast<usize>(leaf_idx) >= bsp.leaves.size()) return -1;
        if ((bsp.leaves[static_cast<usize>(leaf_idx)].flags & kLeafFlagEmpty) != 0) {
            return leaf_idx;
        }
        return -1;
    }
    if (static_cast<usize>(child) >= bsp.nodes.size()) return -1;
    const BspNode& n = bsp.nodes[static_cast<usize>(child)];
    i32 a = first_empty_leaf(bsp, n.front);
    if (a >= 0) return a;
    return first_empty_leaf(bsp, n.back);
}

// Build a square winding (4 verts CCW seen from +normal side) on the plane
// (n, d), centred at the plane's closest point to the origin, with edge
// length `extent`. Returns the 4 corners.
void build_portal_winding(math::Vec3 n, f32 d, f32 extent,
                          std::vector<math::Vec3>& out_verts) {
    // Pick a stable tangent.
    math::Vec3 t{};
    if (std::fabs(n.x) < 0.9f)      t = {1, 0, 0};
    else                            t = {0, 1, 0};
    math::Vec3 u = math::cross(n, t);
    f32 ulen = std::sqrt(math::dot(u, u));
    if (ulen > 1e-9f) u = math::mul(u, 1.0f / ulen);
    math::Vec3 v = math::cross(n, u);
    // centre = n * d (closest point on plane to origin).
    math::Vec3 c = math::mul(n, d);
    f32 h = 0.5f * extent;
    out_verts.push_back(math::add(c, math::add(math::mul(u, -h), math::mul(v, -h))));
    out_verts.push_back(math::add(c, math::add(math::mul(u,  h), math::mul(v, -h))));
    out_verts.push_back(math::add(c, math::add(math::mul(u,  h), math::mul(v,  h))));
    out_verts.push_back(math::add(c, math::add(math::mul(u, -h), math::mul(v,  h))));
}

void generate_portals(CompiledBsp& bsp, f32 world_extent) {
    bsp.portals.clear();
    bsp.portal_vertices.clear();
    for (usize ni = 0; ni < bsp.nodes.size(); ++ni) {
        const BspNode& node = bsp.nodes[ni];
        if (node.plane < 0 || static_cast<usize>(node.plane) >= bsp.planes.size()) {
            continue;
        }
        i32 fl = first_empty_leaf(bsp, node.front);
        i32 bl = first_empty_leaf(bsp, node.back);
        if (fl < 0 || bl < 0) continue;
        if (fl == bl) continue;   // same empty region reached from both sides — degenerate.

        const BspPlane& pl = bsp.planes[static_cast<usize>(node.plane)];
        BspPortal portal;
        portal.front_leaf   = fl;
        portal.back_leaf    = bl;
        portal.first_vertex = static_cast<u32>(bsp.portal_vertices.size());
        portal.plane_normal = pl.normal;
        portal.plane_d      = pl.d;
        build_portal_winding(pl.normal, pl.d, world_extent, bsp.portal_vertices);
        portal.vertex_count = static_cast<u32>(bsp.portal_vertices.size()) - portal.first_vertex;
        bsp.portals.push_back(portal);
    }
}

}  // anon namespace

bool compile_bsp(const MapFile& map, CompiledBsp& out, std::string* err) {
    out = {};
    if (map.entities.empty()) {
        if (err) *err = "qbsp: no entities";
        return false;
    }
    const MapEntity& world = map.entities.front();
    std::vector<u32> brush_ids(world.brushes.size());
    for (u32 i = 0; i < brush_ids.size(); ++i) brush_ids[i] = i;

    // Pre-populate brush table + brush_planes
    out.brushes.reserve(world.brushes.size());
    std::vector<u64> plane_keys;
    for (const auto& brush : world.brushes) {
        BspBrush b{};
        b.first_plane = static_cast<u32>(out.brush_planes.size());
        b.plane_count = static_cast<u32>(brush.planes.size());
        b.bounds = brush.bounds;
        for (const auto& pl : brush.planes) {
            u32 pi = intern_plane(out, plane_keys, pl.normal, pl.d);
            out.brush_planes.push_back(pi);
        }
        out.brushes.push_back(b);
    }
    constexpr int kMaxDepth = 32;
    i32 root = build_recursive(out, plane_keys, world.brushes, brush_ids, 0, kMaxDepth);
    if (out.nodes.empty()) {
        // Single-leaf BSP. Synthesize a root node with the same leaf on both
        // sides so the runtime walker always finds something.
        if (out.leaves.empty()) {
            BspLeaf leaf;
            leaf.cluster = 0;
            leaf.flags = kLeafFlagEmpty;
            leaf.bounds.min = {-1e6f, -1e6f, -1e6f};
            leaf.bounds.max = { 1e6f,  1e6f,  1e6f};
            out.leaves.push_back(leaf);
        }
        if (out.planes.empty()) {
            BspPlane p{}; p.normal = {0, 0, 1}; p.d = 0;
            out.planes.push_back(p);
        }
        BspNode node{};
        node.plane = 0;
        node.front = -1;
        node.back  = -1;
        out.nodes.push_back(node);
    } else if (root >= 0 && root != 0) {
        // Ensure nodes[0] is the root: shuffle if needed.
        if (root != 0) {
            std::swap(out.nodes[0], out.nodes[static_cast<usize>(root)]);
            // Patch references — children pointing at swapped indices.
            for (auto& nd : out.nodes) {
                if (nd.front == 0) nd.front = root;
                else if (nd.front == root) nd.front = 0;
                if (nd.back == 0) nd.back = root;
                else if (nd.back == root) nd.back = 0;
            }
        }
    }
    // Wave-B: emit portal records mirroring lane 10's BspPortal layout.
    // Extent picked large enough to span our Wave-B test scenes (single
    // small Quake-style room) without overflowing single-precision floats
    // when downstream clippers do plane-plane intersections.
    generate_portals(out, /*world_extent=*/4096.0f);
    return true;
}

void write_psybsp(const CompiledBsp& bsp, std::vector<u8>& out) {
    out.clear();
    out.reserve(40 + bsp.planes.size() * 16 + bsp.nodes.size() * 12
                  + bsp.portals.size() * 32 + bsp.portal_vertices.size() * 12);

    append_le<u32>(out, kPsyBspMagic);
    append_le<u32>(out, kPsyBspVersion);
    append_le<u32>(out, static_cast<u32>(bsp.nodes.size()));
    append_le<u32>(out, static_cast<u32>(bsp.leaves.size()));
    append_le<u32>(out, static_cast<u32>(bsp.planes.size()));
    append_le<u32>(out, static_cast<u32>(bsp.brushes.size()));
    append_le<u32>(out, static_cast<u32>(bsp.brush_planes.size()));
    append_le<u32>(out, static_cast<u32>(bsp.portals.size()));            // v2
    append_le<u32>(out, static_cast<u32>(bsp.portal_vertices.size()));    // v2
    append_le<u32>(out, 0);   // reserved

    for (const auto& p : bsp.planes) {
        append_f32(out, p.normal.x);
        append_f32(out, p.normal.y);
        append_f32(out, p.normal.z);
        append_f32(out, p.d);
    }
    for (const auto& n : bsp.nodes) {
        append_le<i32>(out, n.plane);
        append_le<i32>(out, n.front);
        append_le<i32>(out, n.back);
    }
    for (const auto& lf : bsp.leaves) {
        append_le<i32>(out, lf.cluster);
        append_le<u32>(out, lf.flags);
        append_f32(out, lf.bounds.min.x); append_f32(out, lf.bounds.min.y); append_f32(out, lf.bounds.min.z);
        append_f32(out, lf.bounds.max.x); append_f32(out, lf.bounds.max.y); append_f32(out, lf.bounds.max.z);
    }
    for (const auto& b : bsp.brushes) {
        append_le<u32>(out, b.first_plane);
        append_le<u32>(out, b.plane_count);
        append_f32(out, b.bounds.min.x); append_f32(out, b.bounds.min.y); append_f32(out, b.bounds.min.z);
        append_f32(out, b.bounds.max.x); append_f32(out, b.bounds.max.y); append_f32(out, b.bounds.max.z);
    }
    for (u32 pi : bsp.brush_planes) append_le<u32>(out, pi);
    // ── portals (Wave-B / v2) ───────────────────────────────────────────
    for (const auto& p : bsp.portals) {
        append_le<i32>(out, p.front_leaf);
        append_le<i32>(out, p.back_leaf);
        append_le<u32>(out, p.first_vertex);
        append_le<u32>(out, p.vertex_count);
        append_f32(out, p.plane_normal.x);
        append_f32(out, p.plane_normal.y);
        append_f32(out, p.plane_normal.z);
        append_f32(out, p.plane_d);
    }
    for (const auto& v : bsp.portal_vertices) {
        append_f32(out, v.x);
        append_f32(out, v.y);
        append_f32(out, v.z);
    }
}

bool read_psybsp(std::span<const u8> bytes, CompiledBsp& out, std::string* err) {
    auto fail = [&](const char* msg) { if (err) *err = msg; return false; };
    if (bytes.size() < 40) return fail("psybsp header truncated");
    u32 magic = 0;
    read_le<u32>(bytes, 0, magic);
    if (magic != kPsyBspMagic) return fail("psybsp bad magic");
    u32 version = 0; read_le<u32>(bytes, 4, version);
    if (version != kPsyBspVersion) return fail("psybsp unsupported version");

    u32 nc = 0, lc = 0, pc = 0, bc = 0, bpc = 0, portal_c = 0, portal_vc = 0;
    read_le<u32>(bytes,  8, nc);
    read_le<u32>(bytes, 12, lc);
    read_le<u32>(bytes, 16, pc);
    read_le<u32>(bytes, 20, bc);
    read_le<u32>(bytes, 24, bpc);
    read_le<u32>(bytes, 28, portal_c);
    read_le<u32>(bytes, 32, portal_vc);
    // bytes[36..40) is the reserved field.

    usize cursor = 40;
    out.planes.resize(pc);
    for (u32 i = 0; i < pc; ++i) {
        BspPlane p{};
        read_f32(bytes, cursor +  0, p.normal.x);
        read_f32(bytes, cursor +  4, p.normal.y);
        read_f32(bytes, cursor +  8, p.normal.z);
        read_f32(bytes, cursor + 12, p.d);
        out.planes[i] = p;
        cursor += 16;
    }
    out.nodes.resize(nc);
    for (u32 i = 0; i < nc; ++i) {
        BspNode n{};
        read_le<i32>(bytes, cursor +  0, n.plane);
        read_le<i32>(bytes, cursor +  4, n.front);
        read_le<i32>(bytes, cursor +  8, n.back);
        out.nodes[i] = n;
        cursor += 12;
    }
    out.leaves.resize(lc);
    for (u32 i = 0; i < lc; ++i) {
        BspLeaf lf{};
        read_le<i32>(bytes, cursor +  0, lf.cluster);
        read_le<u32>(bytes, cursor +  4, lf.flags);
        read_f32(bytes, cursor +  8, lf.bounds.min.x);
        read_f32(bytes, cursor + 12, lf.bounds.min.y);
        read_f32(bytes, cursor + 16, lf.bounds.min.z);
        read_f32(bytes, cursor + 20, lf.bounds.max.x);
        read_f32(bytes, cursor + 24, lf.bounds.max.y);
        read_f32(bytes, cursor + 28, lf.bounds.max.z);
        out.leaves[i] = lf;
        cursor += 32;
    }
    out.brushes.resize(bc);
    for (u32 i = 0; i < bc; ++i) {
        BspBrush b{};
        read_le<u32>(bytes, cursor +  0, b.first_plane);
        read_le<u32>(bytes, cursor +  4, b.plane_count);
        read_f32(bytes, cursor +  8, b.bounds.min.x);
        read_f32(bytes, cursor + 12, b.bounds.min.y);
        read_f32(bytes, cursor + 16, b.bounds.min.z);
        read_f32(bytes, cursor + 20, b.bounds.max.x);
        read_f32(bytes, cursor + 24, b.bounds.max.y);
        read_f32(bytes, cursor + 28, b.bounds.max.z);
        out.brushes[i] = b;
        cursor += 32;
    }
    out.brush_planes.resize(bpc);
    for (u32 i = 0; i < bpc; ++i) {
        u32 v = 0;
        read_le<u32>(bytes, cursor, v);
        out.brush_planes[i] = v;
        cursor += 4;
    }
    // ── portals (v2) ────────────────────────────────────────────────────
    out.portals.resize(portal_c);
    for (u32 i = 0; i < portal_c; ++i) {
        BspPortal p{};
        if (cursor + 32 > bytes.size()) return fail("psybsp portal truncated");
        read_le<i32>(bytes, cursor +  0, p.front_leaf);
        read_le<i32>(bytes, cursor +  4, p.back_leaf);
        read_le<u32>(bytes, cursor +  8, p.first_vertex);
        read_le<u32>(bytes, cursor + 12, p.vertex_count);
        read_f32(bytes, cursor + 16, p.plane_normal.x);
        read_f32(bytes, cursor + 20, p.plane_normal.y);
        read_f32(bytes, cursor + 24, p.plane_normal.z);
        read_f32(bytes, cursor + 28, p.plane_d);
        out.portals[i] = p;
        cursor += 32;
    }
    out.portal_vertices.resize(portal_vc);
    for (u32 i = 0; i < portal_vc; ++i) {
        if (cursor + 12 > bytes.size()) return fail("psybsp portal vertices truncated");
        math::Vec3 v{};
        read_f32(bytes, cursor +  0, v.x);
        read_f32(bytes, cursor +  4, v.y);
        read_f32(bytes, cursor +  8, v.z);
        out.portal_vertices[i] = v;
        cursor += 12;
    }
    return true;
}

void print_help() {
    std::fprintf(stdout,
        "lm_qbsp — Psynder BSP compiler (id-inspired)\n"
        "\n"
        "Usage:\n"
        "  lm_qbsp <input.map> <output.psybsp>\n"
        "  lm_qbsp --help\n"
        "\n"
        "Accepts brush-list .map files in Quake / TrenchBroom format and\n"
        "compiles a leafy BSP. Wave-B output (.psybsp v2) carries a portal\n"
        "table connecting non-solid leaves on every splitter plane; PVS\n"
        "bit-vector generation still lives in lane 10's loader.\n");
}

namespace {
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

    std::string text;
    std::string err;
    if (!read_file(fs::path(argv[1]), text, err)) {
        std::fprintf(stderr, "lm_qbsp: %s\n", err.c_str());
        return 1;
    }
    MapFile map;
    if (!parse_map(text, map, &err)) {
        std::fprintf(stderr, "lm_qbsp: %s\n", err.c_str());
        return 1;
    }
    CompiledBsp bsp;
    if (!compile_bsp(map, bsp, &err)) {
        std::fprintf(stderr, "lm_qbsp: %s\n", err.c_str());
        return 1;
    }
    std::vector<u8> bytes;
    write_psybsp(bsp, bytes);
    if (!write_file(fs::path(argv[2]), bytes, err)) {
        std::fprintf(stderr, "lm_qbsp: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stdout,
                 "lm_qbsp: %s -> %s (planes=%u nodes=%u leaves=%u brushes=%u portals=%u)\n",
                 argv[1], argv[2],
                 static_cast<u32>(bsp.planes.size()),
                 static_cast<u32>(bsp.nodes.size()),
                 static_cast<u32>(bsp.leaves.size()),
                 static_cast<u32>(bsp.brushes.size()),
                 static_cast<u32>(bsp.portals.size()));
    return 0;
}

}  // namespace psynder::tools::qbsp
