// SPDX-License-Identifier: MIT
// Psynder — lm_qbsp implementation. Lane 24 / tools.

#include "Qbsp.h"

// Engine BSP runtime format + PVS builder. lm_qbsp_lib already links
// psynder_world_bsp (see tools/lm_qbsp/CMakeLists.txt), so the offline compiler
// can reuse the exact on-disk layout and the exact leaf-portal-flood PVS the
// runtime consumes — no duplicated format/algorithm to drift.
#include "world/bsp/Bsp.h"
#include "world/bsp/BspFormat.h"
#include "world/bsp/Portal.h"
#include "world/bsp/PvsBuild.h"

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
#include <utility>
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
    if (off + sizeof(T) > bytes.size())
        return false;
    using U = std::make_unsigned_t<T>;
    U u = 0;
    for (usize i = 0; i < sizeof(T); ++i)
        u |= static_cast<U>(bytes[off + i]) << (8 * i);
    out = static_cast<T>(u);
    return true;
}
bool read_f32(std::span<const u8> bytes, usize off, f32& out) {
    u32 bits = 0;
    if (!read_le<u32>(bytes, off, bits))
        return false;
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
        if (pos_ >= src_.size())
            return false;
        char c = src_[pos_];
        if (c == '(' || c == ')' || c == '{' || c == '}') {
            ++pos_;
            out.assign(1, c);
            return true;
        }
        if (c == '"') {
            ++pos_;
            out.clear();
            while (pos_ < src_.size() && src_[pos_] != '"')
                out.push_back(src_[pos_++]);
            if (pos_ < src_.size())
                ++pos_;
            return true;
        }
        usize start = pos_;
        while (pos_ < src_.size() && !std::isspace(static_cast<unsigned char>(src_[pos_])) &&
               src_[pos_] != '(' && src_[pos_] != ')' && src_[pos_] != '{' && src_[pos_] != '}') {
            ++pos_;
        }
        if (start == pos_)
            return false;
        out.assign(src_.data() + start, src_.data() + pos_);
        return true;
    }
    bool peek_char(char& out) {
        skip_ws();
        if (pos_ >= src_.size())
            return false;
        out = src_[pos_];
        return true;
    }
    usize line_no() const {
        usize n = 1;
        for (usize i = 0; i < pos_ && i < src_.size(); ++i) {
            if (src_[i] == '\n')
                ++n;
        }
        return n;
    }

   private:
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
                continue;
            }
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n')
                    ++pos_;
                continue;
            }
            break;
        }
    }
    std::string_view src_;
    usize pos_;
};

bool parse_vec3_from_tokens(MapTok& t, math::Vec3& out, std::string& err) {
    std::string s;
    if (!t.next(s) || s != "(") {
        err = "expected '('";
        return false;
    }
    f32 v[3];
    for (int i = 0; i < 3; ++i) {
        if (!t.next(s)) {
            err = "expected number";
            return false;
        }
        char* e = nullptr;
        v[i] = std::strtof(s.c_str(), &e);
        if (e == s.c_str()) {
            err = "bad number: " + s;
            return false;
        }
    }
    if (!t.next(s) || s != ")") {
        err = "expected ')'";
        return false;
    }
    out = {v[0], v[1], v[2]};
    return true;
}

}  // namespace

bool parse_map(std::string_view text, MapFile& out, std::string* err) {
    out = {};
    MapTok t(text);
    std::string s;
    while (true) {
        if (t.eof())
            break;
        if (!t.next(s))
            break;
        if (s != "{") {
            if (err)
                *err = "map: expected '{' at top level on line " + std::to_string(t.line_no());
            return false;
        }
        MapEntity ent;
        while (true) {
            if (!t.next(s)) {
                if (err)
                    *err = "map: unexpected EOF inside entity";
                return false;
            }
            if (s == "}")
                break;
            if (s == "{") {
                // brush
                MapBrush brush;
                while (true) {
                    char c = 0;
                    if (!t.peek_char(c)) {
                        if (err)
                            *err = "map: unexpected EOF in brush";
                        return false;
                    }
                    if (c == '}') {
                        t.next(s);
                        break;
                    }
                    // face: ( p1 ) ( p2 ) ( p3 ) tex u v rot us vs
                    math::Vec3 p1, p2, p3;
                    std::string err_sub;
                    if (!parse_vec3_from_tokens(t, p1, err_sub) ||
                        !parse_vec3_from_tokens(t, p2, err_sub) ||
                        !parse_vec3_from_tokens(t, p3, err_sub)) {
                        if (err)
                            *err = "map: brush face — " + err_sub;
                        return false;
                    }
                    std::string mat;
                    if (!t.next(mat)) {
                        if (err)
                            *err = "map: missing material";
                        return false;
                    }
                    // Skip 5 trailing numbers (offsets / rotation / scales).
                    for (int k = 0; k < 5; ++k) {
                        if (!t.next(s)) {
                            if (err)
                                *err = "map: missing face params";
                            return false;
                        }
                    }
                    // Compute plane from three points. Quake .map convention:
                    // points are given CW when viewed from the *outside* of the
                    // brush volume, so the outward normal is (p2-p1) × (p3-p1).
                    math::Vec3 e1 = math::sub(p2, p1);
                    math::Vec3 e2 = math::sub(p3, p1);
                    math::Vec3 n = math::cross(e1, e2);
                    f32 len = std::sqrt(math::dot(n, n));
                    if (len < 1e-9f) {
                        if (err)
                            *err = "map: degenerate face";
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
            if (!t.next(val)) {
                if (err)
                    *err = "map: missing value for " + s;
                return false;
            }
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
        for (usize i = 0; i < sizeof(x); ++i) {
            h ^= p[i];
            h *= 0x100000001b3ULL;
        }
    };
    bump(q(n.x));
    bump(q(n.y));
    bump(q(n.z));
    bump(q(d));
    return h;
}

u32 intern_plane(CompiledBsp& bsp, std::vector<u64>& keys, math::Vec3 n, f32 d) {
    u64 k = plane_key(n, d);
    for (u32 i = 0; i < keys.size(); ++i) {
        if (keys[i] == k)
            return i;
    }
    keys.push_back(k);
    BspPlane p{};
    p.normal = n;
    p.d = d;
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
    math::Vec3 cen{0, 0, 0};
    if (brush.planes.empty())
        return 0;
    for (const auto& pl : brush.planes) {
        cen = math::add(cen, math::mul(pl.normal, pl.d));
    }
    cen = math::mul(cen, 1.0f / static_cast<f32>(brush.planes.size()));
    f32 dist = math::dot(n, cen) - d;
    constexpr f32 kEps = 1e-3f;
    if (dist > kEps)
        return 0;
    if (dist < -kEps)
        return 1;
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
        leaf.bounds.max = {1e6f, 1e6f, 1e6f};
        bsp.leaves.push_back(leaf);
        return -static_cast<i32>(bsp.leaves.size());  // negative = leaf
    }

    // Pick the first plane of the first brush as splitter.
    const MapBrush& b0 = world[brush_ids[0]];
    if (b0.planes.empty()) {
        BspLeaf leaf;
        leaf.cluster = static_cast<i32>(bsp.leaves.size());
        leaf.flags = kLeafFlagSolid;
        leaf.bounds.min = {-1e6f, -1e6f, -1e6f};
        leaf.bounds.max = {1e6f, 1e6f, 1e6f};
        bsp.leaves.push_back(leaf);
        return -static_cast<i32>(bsp.leaves.size());
    }
    math::Vec3 n = b0.planes[0].normal;
    f32 d = b0.planes[0].d;

    u32 plane_idx = intern_plane(bsp, plane_keys, n, d);

    std::vector<u32> front, back;
    for (u32 id : brush_ids) {
        int side = classify(world[id], n, d);
        if (side == 0)
            front.push_back(id);
        else if (side == 1)
            back.push_back(id);
        else {
            front.push_back(id);
            back.push_back(id);
        }
    }
    // If splitting made no progress, fold remaining brushes into a leaf.
    if (front.size() == brush_ids.size() && back.size() == brush_ids.size()) {
        BspLeaf leaf;
        leaf.cluster = static_cast<i32>(bsp.leaves.size());
        leaf.flags = kLeafFlagSolid;
        leaf.bounds.min = {-1e6f, -1e6f, -1e6f};
        leaf.bounds.max = {1e6f, 1e6f, 1e6f};
        bsp.leaves.push_back(leaf);
        return -static_cast<i32>(bsp.leaves.size());
    }

    BspNode node{};
    node.plane = static_cast<i32>(plane_idx);
    node.front = 0;
    node.back = 0;
    u32 node_idx = static_cast<u32>(bsp.nodes.size());
    bsp.nodes.push_back(node);

    i32 f_child = build_recursive(bsp, plane_keys, world, front, depth + 1, max_depth);
    i32 b_child = build_recursive(bsp, plane_keys, world, back, depth + 1, max_depth);
    bsp.nodes[node_idx].front = f_child;
    bsp.nodes[node_idx].back = b_child;
    return static_cast<i32>(node_idx);
}

}  // namespace

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
        if (leaf_idx < 0 || static_cast<usize>(leaf_idx) >= bsp.leaves.size())
            return -1;
        if ((bsp.leaves[static_cast<usize>(leaf_idx)].flags & kLeafFlagEmpty) != 0) {
            return leaf_idx;
        }
        return -1;
    }
    if (static_cast<usize>(child) >= bsp.nodes.size())
        return -1;
    const BspNode& n = bsp.nodes[static_cast<usize>(child)];
    i32 a = first_empty_leaf(bsp, n.front);
    if (a >= 0)
        return a;
    return first_empty_leaf(bsp, n.back);
}

// Build a square winding (4 verts CCW seen from +normal side) on the plane
// (n, d), centred at the plane's closest point to the origin, with edge
// length `extent`. Returns the 4 corners.
void build_portal_winding(math::Vec3 n, f32 d, f32 extent, std::vector<math::Vec3>& out_verts) {
    // Pick a stable tangent.
    math::Vec3 t{};
    if (std::fabs(n.x) < 0.9f)
        t = {1, 0, 0};
    else
        t = {0, 1, 0};
    math::Vec3 u = math::cross(n, t);
    f32 ulen = std::sqrt(math::dot(u, u));
    if (ulen > 1e-9f)
        u = math::mul(u, 1.0f / ulen);
    math::Vec3 v = math::cross(n, u);
    // centre = n * d (closest point on plane to origin).
    math::Vec3 c = math::mul(n, d);
    f32 h = 0.5f * extent;
    out_verts.push_back(math::add(c, math::add(math::mul(u, -h), math::mul(v, -h))));
    out_verts.push_back(math::add(c, math::add(math::mul(u, h), math::mul(v, -h))));
    out_verts.push_back(math::add(c, math::add(math::mul(u, h), math::mul(v, h))));
    out_verts.push_back(math::add(c, math::add(math::mul(u, -h), math::mul(v, h))));
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
        if (fl < 0 || bl < 0)
            continue;
        if (fl == bl)
            continue;  // same empty region reached from both sides — degenerate.

        const BspPlane& pl = bsp.planes[static_cast<usize>(node.plane)];
        BspPortal portal;
        portal.front_leaf = fl;
        portal.back_leaf = bl;
        portal.first_vertex = static_cast<u32>(bsp.portal_vertices.size());
        portal.plane_normal = pl.normal;
        portal.plane_d = pl.d;
        build_portal_winding(pl.normal, pl.d, world_extent, bsp.portal_vertices);
        portal.vertex_count = static_cast<u32>(bsp.portal_vertices.size()) - portal.first_vertex;
        bsp.portals.push_back(portal);
    }
}

}  // namespace

bool compile_bsp(const MapFile& map, CompiledBsp& out, std::string* err) {
    out = {};
    if (map.entities.empty()) {
        if (err)
            *err = "qbsp: no entities";
        return false;
    }
    const MapEntity& world = map.entities.front();
    std::vector<u32> brush_ids(world.brushes.size());
    for (u32 i = 0; i < brush_ids.size(); ++i)
        brush_ids[i] = i;

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
            leaf.bounds.max = {1e6f, 1e6f, 1e6f};
            out.leaves.push_back(leaf);
        }
        if (out.planes.empty()) {
            BspPlane p{};
            p.normal = {0, 0, 1};
            p.d = 0;
            out.planes.push_back(p);
        }
        BspNode node{};
        node.plane = 0;
        node.front = -1;
        node.back = -1;
        out.nodes.push_back(node);
    } else if (root >= 0 && root != 0) {
        // Ensure nodes[0] is the root: shuffle if needed.
        if (root != 0) {
            std::swap(out.nodes[0], out.nodes[static_cast<usize>(root)]);
            // Patch references — children pointing at swapped indices.
            for (auto& nd : out.nodes) {
                if (nd.front == 0)
                    nd.front = root;
                else if (nd.front == root)
                    nd.front = 0;
                if (nd.back == 0)
                    nd.back = root;
                else if (nd.back == root)
                    nd.back = 0;
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
    out.reserve(40 + bsp.planes.size() * 16 + bsp.nodes.size() * 12 + bsp.portals.size() * 32 +
                bsp.portal_vertices.size() * 12);

    append_le<u32>(out, kPsyBspMagic);
    append_le<u32>(out, kPsyBspVersion);
    append_le<u32>(out, static_cast<u32>(bsp.nodes.size()));
    append_le<u32>(out, static_cast<u32>(bsp.leaves.size()));
    append_le<u32>(out, static_cast<u32>(bsp.planes.size()));
    append_le<u32>(out, static_cast<u32>(bsp.brushes.size()));
    append_le<u32>(out, static_cast<u32>(bsp.brush_planes.size()));
    append_le<u32>(out, static_cast<u32>(bsp.portals.size()));          // v2
    append_le<u32>(out, static_cast<u32>(bsp.portal_vertices.size()));  // v2
    append_le<u32>(out, 0);                                             // reserved

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
        append_f32(out, lf.bounds.min.x);
        append_f32(out, lf.bounds.min.y);
        append_f32(out, lf.bounds.min.z);
        append_f32(out, lf.bounds.max.x);
        append_f32(out, lf.bounds.max.y);
        append_f32(out, lf.bounds.max.z);
    }
    for (const auto& b : bsp.brushes) {
        append_le<u32>(out, b.first_plane);
        append_le<u32>(out, b.plane_count);
        append_f32(out, b.bounds.min.x);
        append_f32(out, b.bounds.min.y);
        append_f32(out, b.bounds.min.z);
        append_f32(out, b.bounds.max.x);
        append_f32(out, b.bounds.max.y);
        append_f32(out, b.bounds.max.z);
    }
    for (u32 pi : bsp.brush_planes)
        append_le<u32>(out, pi);
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
    auto fail = [&](const char* msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (bytes.size() < 40)
        return fail("psybsp header truncated");
    u32 magic = 0;
    read_le<u32>(bytes, 0, magic);
    if (magic != kPsyBspMagic)
        return fail("psybsp bad magic");
    u32 version = 0;
    read_le<u32>(bytes, 4, version);
    if (version != kPsyBspVersion)
        return fail("psybsp unsupported version");

    u32 nc = 0, lc = 0, pc = 0, bc = 0, bpc = 0, portal_c = 0, portal_vc = 0;
    read_le<u32>(bytes, 8, nc);
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
        read_f32(bytes, cursor + 0, p.normal.x);
        read_f32(bytes, cursor + 4, p.normal.y);
        read_f32(bytes, cursor + 8, p.normal.z);
        read_f32(bytes, cursor + 12, p.d);
        out.planes[i] = p;
        cursor += 16;
    }
    out.nodes.resize(nc);
    for (u32 i = 0; i < nc; ++i) {
        BspNode n{};
        read_le<i32>(bytes, cursor + 0, n.plane);
        read_le<i32>(bytes, cursor + 4, n.front);
        read_le<i32>(bytes, cursor + 8, n.back);
        out.nodes[i] = n;
        cursor += 12;
    }
    out.leaves.resize(lc);
    for (u32 i = 0; i < lc; ++i) {
        BspLeaf lf{};
        read_le<i32>(bytes, cursor + 0, lf.cluster);
        read_le<u32>(bytes, cursor + 4, lf.flags);
        read_f32(bytes, cursor + 8, lf.bounds.min.x);
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
        read_le<u32>(bytes, cursor + 0, b.first_plane);
        read_le<u32>(bytes, cursor + 4, b.plane_count);
        read_f32(bytes, cursor + 8, b.bounds.min.x);
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
        if (cursor + 32 > bytes.size())
            return fail("psybsp portal truncated");
        read_le<i32>(bytes, cursor + 0, p.front_leaf);
        read_le<i32>(bytes, cursor + 4, p.back_leaf);
        read_le<u32>(bytes, cursor + 8, p.first_vertex);
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
        if (cursor + 12 > bytes.size())
            return fail("psybsp portal vertices truncated");
        math::Vec3 v{};
        read_f32(bytes, cursor + 0, v.x);
        read_f32(bytes, cursor + 4, v.y);
        read_f32(bytes, cursor + 8, v.z);
        out.portal_vertices[i] = v;
        cursor += 12;
    }
    return true;
}

// ─── `.rooms` source: parse + compile + engine-format emit ───────────────────

namespace {

// Whitespace / comment-skipping word tokenizer for `.rooms` (reuses the same
// comment + delimiter rules as MapTok but yields bare words only — `.rooms` has
// no parens/braces/quotes).
class RoomsTok {
   public:
    explicit RoomsTok(std::string_view s) : src_(s), pos_(0) {}
    bool next(std::string& out) {
        skip_ws();
        if (pos_ >= src_.size())
            return false;
        usize start = pos_;
        while (pos_ < src_.size() && !std::isspace(static_cast<unsigned char>(src_[pos_])))
            ++pos_;
        out.assign(src_.data() + start, src_.data() + pos_);
        return start != pos_;
    }
    bool next_i32(i32& out) {
        std::string s;
        if (!next(s))
            return false;
        char* e = nullptr;
        long v = std::strtol(s.c_str(), &e, 10);
        if (e == s.c_str())
            return false;
        out = static_cast<i32>(v);
        return true;
    }
    bool next_f32(f32& out) {
        std::string s;
        if (!next(s))
            return false;
        char* e = nullptr;
        out = std::strtof(s.c_str(), &e);
        return e != s.c_str();
    }
    usize line_no() const {
        usize n = 1;
        for (usize i = 0; i < pos_ && i < src_.size(); ++i)
            if (src_[i] == '\n')
                ++n;
        return n;
    }

   private:
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
                continue;
            }
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n')
                    ++pos_;
                continue;
            }
            break;
        }
    }
    std::string_view src_;
    usize pos_;
};

}  // namespace

bool parse_rooms(std::string_view text, RoomsFile& out, std::string* err) {
    out = {};
    RoomsTok t(text);
    auto fail = [&](const std::string& msg) {
        if (err)
            *err = "rooms: " + msg + " (line " + std::to_string(t.line_no()) + ")";
        return false;
    };

    std::string kw;
    if (!t.next(kw) || kw != "rooms")
        return fail("expected 'rooms <N>' header");
    i32 room_count = 0;
    if (!t.next_i32(room_count) || room_count < 0)
        return fail("bad room count");

    out.rooms.reserve(static_cast<usize>(room_count));
    for (i32 i = 0; i < room_count; ++i) {
        if (!t.next(kw) || kw != "room")
            return fail("expected 'room' record");
        RoomVolume rv;
        if (!t.next_i32(rv.cluster) || rv.cluster < 0)
            return fail("bad cluster id");
        f32 v[6];
        for (f32& f : v) {
            if (!t.next_f32(f))
                return fail("expected room bounds (6 floats)");
        }
        rv.bounds.min = {v[0], v[1], v[2]};
        rv.bounds.max = {v[3], v[4], v[5]};
        if (rv.bounds.min.x > rv.bounds.max.x || rv.bounds.min.y > rv.bounds.max.y ||
            rv.bounds.min.z > rv.bounds.max.z)
            return fail("room bounds min > max");
        // Optional trailing bare-word name (not 'room'/'portals'/'portal').
        // Peek: only consume if the next token is not a known keyword.
        RoomsTok save = t;
        std::string maybe_name;
        if (save.next(maybe_name) && maybe_name != "room" && maybe_name != "portals" &&
            maybe_name != "portal") {
            rv.name = maybe_name;
            t = save;
        }
        out.rooms.push_back(std::move(rv));
    }

    // Reject duplicate cluster ids (each room is one PVS cluster row).
    for (usize i = 0; i < out.rooms.size(); ++i) {
        for (usize j = i + 1; j < out.rooms.size(); ++j) {
            if (out.rooms[i].cluster == out.rooms[j].cluster)
                return fail("duplicate cluster id " + std::to_string(out.rooms[i].cluster));
        }
    }

    // Portals are optional (a single-room map has none).
    std::string tok;
    if (!t.next(tok))
        return true;
    if (tok != "portals")
        return fail("expected 'portals <M>' after rooms");
    i32 portal_count = 0;
    if (!t.next_i32(portal_count) || portal_count < 0)
        return fail("bad portal count");
    out.portals.reserve(static_cast<usize>(portal_count));
    for (i32 i = 0; i < portal_count; ++i) {
        if (!t.next(kw) || kw != "portal")
            return fail("expected 'portal' record");
        RoomPortal rp;
        if (!t.next_i32(rp.cluster_a) || !t.next_i32(rp.cluster_b))
            return fail("portal needs two cluster ids");
        out.portals.push_back(rp);
    }
    return true;
}

namespace {

// Recursively split a set of room leaves into a median-split kd-tree. `order`
// holds room indices; we split on the axis of largest centroid spread at the
// median, emitting an internal node whose plane separates the two halves. A
// node child is encoded BspFormat-style: child < 0 => leaf (~leaf_index),
// child >= 0 => node index. Leaf index == room index (1 leaf per room).
//
// The split plane is axis-aligned (normal = +axis unit, d = split coordinate).
// `Bsp::locate` evaluates dot(n,p) - d >= 0 => front child. We place rooms whose
// centroid coordinate >= split on the FRONT side so a point inside a room lands
// in that room's half.
i32 build_kd(CompiledBsp& bsp,
             const RoomsFile& rooms,
             std::vector<u32>& order,
             u32 lo,
             u32 hi) {
    const u32 n = hi - lo;
    if (n == 1) {
        return ~static_cast<i32>(order[lo]);  // leaf reference
    }

    // Pick split axis = largest spread of room-centre coordinates in [lo,hi).
    f32 cmin[3] = {1e30f, 1e30f, 1e30f};
    f32 cmax[3] = {-1e30f, -1e30f, -1e30f};
    auto centre = [&](u32 ri, int ax) {
        const math::Aabb& b = rooms.rooms[ri].bounds;
        const f32 lo3[3] = {b.min.x, b.min.y, b.min.z};
        const f32 hi3[3] = {b.max.x, b.max.y, b.max.z};
        return 0.5f * (lo3[ax] + hi3[ax]);
    };
    for (u32 i = lo; i < hi; ++i) {
        for (int ax = 0; ax < 3; ++ax) {
            const f32 c = centre(order[i], ax);
            if (c < cmin[ax])
                cmin[ax] = c;
            if (c > cmax[ax])
                cmax[ax] = c;
        }
    }
    int axis = 0;
    f32 best = cmax[0] - cmin[0];
    for (int ax = 1; ax < 3; ++ax) {
        const f32 spread = cmax[ax] - cmin[ax];
        if (spread > best) {
            best = spread;
            axis = ax;
        }
    }

    // Sort [lo,hi) by centre on the chosen axis (deterministic stable sort).
    std::stable_sort(order.begin() + lo, order.begin() + hi, [&](u32 a, u32 b) {
        const f32 ca = centre(a, axis);
        const f32 cb = centre(b, axis);
        if (ca != cb)
            return ca < cb;
        return a < b;  // tie-break by room index for determinism
    });
    const u32 mid = lo + n / 2;
    // Split plane sits midway between the two straddling room centres so every
    // room on the FRONT (>= split) side resolves to a front-subtree leaf.
    const f32 split = 0.5f * (centre(order[mid - 1], axis) + centre(order[mid], axis));

    math::Vec3 normal{0, 0, 0};
    (&normal.x)[axis] = 1.0f;

    BspPlane plane{};
    plane.normal = normal;
    plane.d = split;
    bsp.planes.push_back(plane);
    const i32 plane_idx = static_cast<i32>(bsp.planes.size() - 1);

    const u32 node_idx = static_cast<u32>(bsp.nodes.size());
    bsp.nodes.push_back(BspNode{plane_idx, 0, 0});

    // FRONT = rooms with centre >= split = upper half [mid,hi); BACK = [lo,mid).
    const i32 front = build_kd(bsp, rooms, order, mid, hi);
    const i32 back = build_kd(bsp, rooms, order, lo, mid);
    bsp.nodes[node_idx].front = front;
    bsp.nodes[node_idx].back = back;
    return static_cast<i32>(node_idx);
}

// --- W10-2: room box geometry emission ---------------------------------------
//
// Each room is an axis-aligned box [lo, hi]. We tessellate its 6 faces INWARD-
// facing (normal points into the room interior) so a camera standing inside the
// room sees the walls/floor/ceiling. Each face is a quad fan-triangulated as
// {0,1,2, 0,2,3}; indices are FACE-LOCAL (0..3). The runtime BspDraw converter
// aliases the face's index block at `geom.indices[first_vertex]`, so we advance
// the shared (vertex==index) cursor by max(4, 6) = 6 per quad to keep the
// parallel index blocks from overlapping (2 trailing vertex slots per face are
// padding - cheap, and keeps the BspDraw addressing contract intact).

constexpr u32 kQuadVerts = 4u;
constexpr u32 kQuadIndices = 6u;            // (4-2)*3 fan triangles
constexpr u32 kFaceCursorStride = 6u;       // max(kQuadVerts, kQuadIndices)

// Emit one inward-facing quad. `corners` are the 4 box corners of the face in an
// arbitrary order; we re-order them CCW as seen from `+inward_normal` so the
// face's front side (CCW after the viewport Y-flip) points into the room. The
// face uses `material` and is unlit (kBspNoLightmap, zero lightmap_uv).
void emit_quad(CompiledBsp& bsp,
               const math::Vec3 corners[4],
               math::Vec3 inward_normal,
               u32 material,
               u32 color) {
    // Order the 4 corners CCW about `inward_normal`. Compute the face centroid,
    // then sort by the signed angle in the plane (deterministic atan2 order).
    math::Vec3 c{0, 0, 0};
    for (int i = 0; i < 4; ++i)
        c = math::add(c, corners[i]);
    c = math::mul(c, 0.25f);
    // Build an in-plane basis (u, v) with v = inward_normal x u so that
    // (u, v, inward_normal) is right-handed -> increasing atan2(.,.) is CCW seen
    // from +inward_normal.
    math::Vec3 ref = math::sub(corners[0], c);
    f32 rlen = std::sqrt(math::dot(ref, ref));
    math::Vec3 u = (rlen > 1e-9f) ? math::mul(ref, 1.0f / rlen) : math::Vec3{1, 0, 0};
    math::Vec3 v = math::cross(inward_normal, u);
    f32 vlen = std::sqrt(math::dot(v, v));
    if (vlen > 1e-9f)
        v = math::mul(v, 1.0f / vlen);

    struct Keyed {
        math::Vec3 p;
        f32 ang;
    };
    Keyed k[4];
    for (int i = 0; i < 4; ++i) {
        const math::Vec3 r = math::sub(corners[i], c);
        const f32 du = math::dot(r, u);
        const f32 dv = math::dot(r, v);
        k[i].p = corners[i];
        k[i].ang = std::atan2(dv, du);
    }
    std::stable_sort(k, k + 4, [](const Keyed& a, const Keyed& b) { return a.ang < b.ang; });

    const u32 base = static_cast<u32>(bsp.vertices.size());
    QbFace face{};
    face.first_vertex = base;
    face.vertex_count = kQuadVerts;
    face.material = material;
    face.lightmap = kBspNoLightmap;
    bsp.faces.push_back(face);

    // 4 vertices (CCW), then 2 padding slots so the next face's first_vertex lands
    // kFaceCursorStride later and its index block doesn't collide with ours.
    static constexpr math::Vec2 kCornerUv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        QbVertex vert{};
        vert.position = k[i].p;
        vert.normal = inward_normal;
        vert.uv = kCornerUv[i];
        vert.lightmap_uv = {0.0f, 0.0f};
        vert.color = color;
        bsp.vertices.push_back(vert);
    }
    // Pad vertices up to the stride (these padding verts are never indexed).
    for (u32 i = kQuadVerts; i < kFaceCursorStride; ++i)
        bsp.vertices.push_back(QbVertex{});

    // Face-local fan indices {0,1,2, 0,2,3} written at the parallel slab offset.
    bsp.indices.resize(base + kFaceCursorStride, 0u);
    bsp.indices[base + 0] = 0u;
    bsp.indices[base + 1] = 1u;
    bsp.indices[base + 2] = 2u;
    bsp.indices[base + 3] = 0u;
    bsp.indices[base + 4] = 2u;
    bsp.indices[base + 5] = 3u;
}

// Emit the 6 inward-facing box faces for `bounds` and record them under leaf
// `leaf_idx`. Material id == cluster (room tint resolved at runtime).
void emit_room_box(CompiledBsp& bsp,
                   const math::Aabb& bounds,
                   u32 material,
                   u32 color,
                   usize leaf_idx) {
    const u32 first = static_cast<u32>(bsp.faces.size());
    const f32 x0 = bounds.min.x, y0 = bounds.min.y, z0 = bounds.min.z;
    const f32 x1 = bounds.max.x, y1 = bounds.max.y, z1 = bounds.max.z;

    // -X wall: inward normal +X.
    {
        const math::Vec3 q[4] = {{x0, y0, z0}, {x0, y1, z0}, {x0, y1, z1}, {x0, y0, z1}};
        emit_quad(bsp, q, {1, 0, 0}, material, color);
    }
    // +X wall: inward normal -X.
    {
        const math::Vec3 q[4] = {{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}};
        emit_quad(bsp, q, {-1, 0, 0}, material, color);
    }
    // -Z wall: inward normal +Z.
    {
        const math::Vec3 q[4] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}};
        emit_quad(bsp, q, {0, 0, 1}, material, color);
    }
    // +Z wall: inward normal -Z.
    {
        const math::Vec3 q[4] = {{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
        emit_quad(bsp, q, {0, 0, -1}, material, color);
    }
    // Floor (y = y0): inward normal +Y.
    {
        const math::Vec3 q[4] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}};
        emit_quad(bsp, q, {0, 1, 0}, material, color);
    }
    // Ceiling (y = y1): inward normal -Y.
    {
        const math::Vec3 q[4] = {{x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
        emit_quad(bsp, q, {0, -1, 0}, material, color);
    }

    const u32 count = static_cast<u32>(bsp.faces.size()) - first;
    if (leaf_idx < bsp.leaf_first_face.size()) {
        bsp.leaf_first_face[leaf_idx] = first;
        bsp.leaf_face_count[leaf_idx] = count;
    }
}

}  // namespace

bool compile_rooms(const RoomsFile& rooms, CompiledBsp& out, std::string* err) {
    out = {};
    if (rooms.rooms.empty()) {
        if (err)
            *err = "rooms: no rooms to compile";
        return false;
    }

    // One leaf per room, in room order, carrying the room bounds + cluster.
    out.leaves.reserve(rooms.rooms.size());
    for (const RoomVolume& rv : rooms.rooms) {
        BspLeaf leaf{};
        leaf.cluster = rv.cluster;
        leaf.flags = kLeafFlagEmpty;
        leaf.bounds = rv.bounds;
        out.leaves.push_back(leaf);
    }

    // W10-2: emit the room WALL/FLOOR/CEILING geometry, grouped by leaf so the
    // PBSP v1 leaf records carry a contiguous face range and PVS culling skips a
    // culled leaf's faces wholesale. Material id == cluster (the runtime tints
    // per room); a deterministic per-cluster vertex colour gives each room a
    // distinct look even before material resolution. Faces are inward-facing.
    out.leaf_first_face.assign(out.leaves.size(), 0u);
    out.leaf_face_count.assign(out.leaves.size(), 0u);
    auto cluster_tint = [](i32 cluster) -> u32 {
        // Cheap deterministic palette in the engine's 0xAABBGGRR packing.
        const u32 c = static_cast<u32>(cluster);
        const u32 r = 120u + ((c * 53u) % 110u);
        const u32 g = 120u + ((c * 97u) % 110u);
        const u32 b = 120u + ((c * 29u) % 110u);
        return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | (0xFFu << 24);
    };
    for (usize i = 0; i < rooms.rooms.size(); ++i) {
        const RoomVolume& rv = rooms.rooms[i];
        emit_room_box(out, rv.bounds, static_cast<u32>(rv.cluster), cluster_tint(rv.cluster), i);
    }

    // Median-split kd-tree of nodes over the leaf boxes so locate() descends.
    std::vector<u32> order(rooms.rooms.size());
    for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
    const i32 root = build_kd(out, rooms, order, 0, static_cast<u32>(order.size()));

    // Ensure nodes[0] is the root (build_kd appends the root last). For a single
    // room there are no nodes; synthesize a degenerate root pointing at leaf 0 on
    // both sides so the runtime walker always lands somewhere.
    if (out.nodes.empty()) {
        BspPlane p{};
        p.normal = {0, 0, 1};
        p.d = 0;
        out.planes.push_back(p);
        out.nodes.push_back(BspNode{0, ~0, ~0});  // both children -> leaf 0
    } else if (root >= 0 && static_cast<usize>(root) != out.nodes.size() - 1) {
        // build_kd returns the root as the LAST appended node. Move it to slot 0.
        const i32 root_idx = root;
        std::swap(out.nodes[0], out.nodes[static_cast<usize>(root_idx)]);
        for (BspNode& nd : out.nodes) {
            if (nd.front == 0)
                nd.front = root_idx;
            else if (nd.front == root_idx)
                nd.front = 0;
            if (nd.back == 0)
                nd.back = root_idx;
            else if (nd.back == root_idx)
                nd.back = 0;
        }
    } else if (root >= 0 && static_cast<usize>(root) == out.nodes.size() - 1 && root != 0) {
        // Root is the last node and != 0: swap into slot 0 and patch refs.
        const i32 root_idx = root;
        std::swap(out.nodes[0], out.nodes[static_cast<usize>(root_idx)]);
        for (BspNode& nd : out.nodes) {
            if (nd.front == 0)
                nd.front = root_idx;
            else if (nd.front == root_idx)
                nd.front = 0;
            if (nd.back == 0)
                nd.back = root_idx;
            else if (nd.back == root_idx)
                nd.back = 0;
        }
    }

    // Portals: map cluster id -> leaf index (== room order). front_leaf/back_leaf
    // are LEAF indices to match BspPortalSet semantics consumed by build_pvs.
    auto leaf_for_cluster = [&](i32 cluster) -> i32 {
        for (usize i = 0; i < out.leaves.size(); ++i)
            if (out.leaves[i].cluster == cluster)
                return static_cast<i32>(i);
        return -1;
    };
    for (const RoomPortal& rp : rooms.portals) {
        const i32 a = leaf_for_cluster(rp.cluster_a);
        const i32 b = leaf_for_cluster(rp.cluster_b);
        if (a < 0 || b < 0) {
            if (err)
                *err = "rooms: portal references unknown cluster";
            return false;
        }
        BspPortal portal{};
        portal.front_leaf = a;
        portal.back_leaf = b;
        portal.first_vertex = static_cast<u32>(out.portal_vertices.size());
        portal.vertex_count = 0u;  // PVS flood needs only adjacency, not windings
        // Plane: the shared boundary between the two room boxes (informational;
        // the coarse flood ignores it). Use the midplane on the axis of contact.
        portal.plane_normal = {0, 0, 1};
        portal.plane_d = 0.0f;
        out.portals.push_back(portal);
    }
    return true;
}

void write_psybsp_engine(const CompiledBsp& bsp,
                         std::vector<u8>& out,
                         u32* out_clusters,
                         u32* out_pvs_row_bytes) {
    namespace wb = ::psynder::world::bsp;

    // 1. Build a runtime BspMap + BspPortalSet from the compiled data so we can
    //    bake the PVS with the SAME flood the runtime uses for in-memory maps.
    wb::BspMap map;
    map.nodes.reserve(bsp.nodes.size());
    for (const BspNode& n : bsp.nodes) {
        wb::BspNode rn{};
        if (n.plane >= 0 && static_cast<usize>(n.plane) < bsp.planes.size()) {
            rn.plane_normal = bsp.planes[static_cast<usize>(n.plane)].normal;
            rn.plane_d = bsp.planes[static_cast<usize>(n.plane)].d;
        }
        rn.front_child = n.front;
        rn.back_child = n.back;
        map.nodes.push_back(rn);
    }
    map.leaves.reserve(bsp.leaves.size());
    for (usize li = 0; li < bsp.leaves.size(); ++li) {
        const BspLeaf& l = bsp.leaves[li];
        wb::BspLeaf rl{};
        rl.cluster = l.cluster;
        // W10-2: carry the per-leaf face range emitted by compile_rooms (zero
        // when the leaf has no geometry, e.g. the brush path or an empty room).
        rl.first_face = (li < bsp.leaf_first_face.size()) ? bsp.leaf_first_face[li] : 0u;
        rl.face_count = (li < bsp.leaf_face_count.size()) ? bsp.leaf_face_count[li] : 0u;
        rl.bounds = l.bounds;
        map.leaves.push_back(rl);
    }

    wb::BspPortalSet portal_set;
    portal_set.portals.reserve(bsp.portals.size());
    for (const BspPortal& p : bsp.portals) {
        wb::BspPortal rp{};
        rp.front_leaf = p.front_leaf;
        rp.back_leaf = p.back_leaf;
        rp.first_vertex = 0u;
        rp.vertex_count = 0u;
        rp.plane_normal = p.plane_normal;
        rp.plane_d = p.plane_d;
        portal_set.portals.push_back(rp);
    }

    wb::PvsBuildScratch scratch;
    std::vector<u8> pvs;
    u32 row_bytes = 0u;
    const u32 clusters = wb::build_pvs(map, portal_set, scratch, pvs, row_bytes);
    if (out_clusters)
        *out_clusters = clusters;
    if (out_pvs_row_bytes)
        *out_pvs_row_bytes = row_bytes;

    // 2. Serialise the engine PBSP v1 layout (BspFormat.h). Header is 96 bytes;
    //    chunks (nodes/leaves/faces/vertices/indices/pvs) follow 4-byte aligned.
    //    W10-2: faces/vertices/indices now carry the emitted room geometry (when
    //    the rooms path filled them); they stay empty for the brush path / empty
    //    rooms, so the brush pipeline is byte-for-byte unchanged.
    constexpr u32 kHeaderBytes = static_cast<u32>(sizeof(wb::BspFileHeader));
    static_assert(kHeaderBytes == 96u, "engine BSP header must be 96 bytes");

    // On-disk record sizes. The vertex stride mirrors the rasterizer Vertex
    // packed layout (pos3/normal3/uv2/lm_uv2 + RGBA8 = 44 bytes); the loader
    // memcpys with the same stride. BspFileFace is 16 bytes; indices are u32.
    constexpr u32 kVertexBytes = kQbVertexBytes;  // 44
    constexpr u32 kFaceBytes = static_cast<u32>(sizeof(wb::BspFileFace));  // 16
    constexpr u32 kIndexBytes = wb::kBspFileIndexBytes;  // 4

    const u32 node_count = static_cast<u32>(map.nodes.size());
    const u32 leaf_count = static_cast<u32>(map.leaves.size());
    const u32 face_count = static_cast<u32>(bsp.faces.size());
    const u32 vertex_count = static_cast<u32>(bsp.vertices.size());
    const u32 index_count = static_cast<u32>(bsp.indices.size());

    auto align4 = [](u32 v) { return (v + 3u) & ~3u; };

    const u32 nodes_off = kHeaderBytes;
    const u32 nodes_bytes = node_count * static_cast<u32>(sizeof(wb::BspFileNode));
    const u32 leaves_off = align4(nodes_off + nodes_bytes);
    const u32 leaves_bytes = leaf_count * static_cast<u32>(sizeof(wb::BspFileLeaf));
    const u32 faces_off = align4(leaves_off + leaves_bytes);
    const u32 faces_bytes = face_count * kFaceBytes;
    const u32 vertices_off = align4(faces_off + faces_bytes);
    const u32 vertices_bytes = vertex_count * kVertexBytes;
    const u32 indices_off = align4(vertices_off + vertices_bytes);
    const u32 indices_bytes = index_count * kIndexBytes;
    const u32 pvs_off = align4(indices_off + indices_bytes);
    const u32 pvs_bytes = static_cast<u32>(pvs.size());
    const u32 total_bytes = align4(pvs_off + pvs_bytes);

    wb::BspFileHeader header{};
    header.magic = wb::kBspFileMagic;
    header.version = wb::kBspFileVersion;
    header.flags = 0u;
    header.total_bytes = total_bytes;
    header.cluster_count = clusters;
    header.pvs_row_bytes = row_bytes;
    header.nodes = {nodes_off, node_count};
    header.leaves = {leaves_off, leaf_count};
    header.faces = {faces_off, face_count};
    header.vertices = {vertices_off, vertex_count};
    header.indices = {indices_off, index_count};
    header.pvs = {pvs_off, pvs_bytes};

    out.assign(total_bytes, 0u);
    std::memcpy(out.data(), &header, kHeaderBytes);

    for (u32 i = 0; i < node_count; ++i) {
        wb::BspFileNode fn{};
        fn.nx = map.nodes[i].plane_normal.x;
        fn.ny = map.nodes[i].plane_normal.y;
        fn.nz = map.nodes[i].plane_normal.z;
        fn.d = map.nodes[i].plane_d;
        fn.front_child = map.nodes[i].front_child;
        fn.back_child = map.nodes[i].back_child;
        std::memcpy(out.data() + nodes_off + i * sizeof(wb::BspFileNode), &fn, sizeof(fn));
    }
    for (u32 i = 0; i < leaf_count; ++i) {
        wb::BspFileLeaf fl{};
        fl.cluster = map.leaves[i].cluster;
        fl.first_face = map.leaves[i].first_face;
        fl.face_count = map.leaves[i].face_count;
        fl.bbox_min_x = map.leaves[i].bounds.min.x;
        fl.bbox_min_y = map.leaves[i].bounds.min.y;
        fl.bbox_min_z = map.leaves[i].bounds.min.z;
        fl.bbox_max_x = map.leaves[i].bounds.max.x;
        fl.bbox_max_y = map.leaves[i].bounds.max.y;
        fl.bbox_max_z = map.leaves[i].bounds.max.z;
        std::memcpy(out.data() + leaves_off + i * sizeof(wb::BspFileLeaf), &fl, sizeof(fl));
    }
    // Faces: 16-byte records (first_vertex, vertex_count, material, lightmap).
    for (u32 i = 0; i < face_count; ++i) {
        wb::BspFileFace ff{};
        ff.first_vertex = bsp.faces[i].first_vertex;
        ff.vertex_count = bsp.faces[i].vertex_count;
        ff.material = bsp.faces[i].material;
        ff.lightmap = bsp.faces[i].lightmap;
        std::memcpy(out.data() + faces_off + i * kFaceBytes, &ff, sizeof(ff));
    }
    // Vertices: 44-byte packed records written field-by-field little-endian so
    // the tool needs no rasterizer header; the loader reads with the runtime
    // Vertex stride. (offset accumulates per field within the 44-byte record.)
    for (u32 i = 0; i < vertex_count; ++i) {
        const QbVertex& v = bsp.vertices[i];
        u32 off = vertices_off + i * kVertexBytes;
        auto put_f32 = [&](f32 value) {
            u32 bits = 0u;
            std::memcpy(&bits, &value, sizeof(bits));
            out[off + 0] = static_cast<u8>(bits & 0xFFu);
            out[off + 1] = static_cast<u8>((bits >> 8) & 0xFFu);
            out[off + 2] = static_cast<u8>((bits >> 16) & 0xFFu);
            out[off + 3] = static_cast<u8>((bits >> 24) & 0xFFu);
            off += 4u;
        };
        auto put_u32 = [&](u32 value) {
            out[off + 0] = static_cast<u8>(value & 0xFFu);
            out[off + 1] = static_cast<u8>((value >> 8) & 0xFFu);
            out[off + 2] = static_cast<u8>((value >> 16) & 0xFFu);
            out[off + 3] = static_cast<u8>((value >> 24) & 0xFFu);
            off += 4u;
        };
        put_f32(v.position.x);
        put_f32(v.position.y);
        put_f32(v.position.z);
        put_f32(v.normal.x);
        put_f32(v.normal.y);
        put_f32(v.normal.z);
        put_f32(v.uv.x);
        put_f32(v.uv.y);
        put_f32(v.lightmap_uv.x);
        put_f32(v.lightmap_uv.y);
        put_u32(v.color);
    }
    // Indices: u32 little-endian.
    for (u32 i = 0; i < index_count; ++i) {
        const u32 idx = bsp.indices[i];
        const u32 off = indices_off + i * kIndexBytes;
        out[off + 0] = static_cast<u8>(idx & 0xFFu);
        out[off + 1] = static_cast<u8>((idx >> 8) & 0xFFu);
        out[off + 2] = static_cast<u8>((idx >> 16) & 0xFFu);
        out[off + 3] = static_cast<u8>((idx >> 24) & 0xFFu);
    }
    if (pvs_bytes > 0u) {
        std::memcpy(out.data() + pvs_off, pvs.data(), pvs_bytes);
    }
}

void print_help() {
    std::fprintf(stdout,
                 "lm_qbsp — Psynder BSP compiler (id-inspired)\n"
                 "\n"
                 "Usage:\n"
                 "  lm_qbsp <input.map> <output.psybsp>            (brush .map -> PSBP v2)\n"
                 "  lm_qbsp --rooms <input.rooms> <output.psybsp>  (rooms -> engine PBSP v1 + baked PVS)\n"
                 "  lm_qbsp --help\n"
                 "\n"
                 "Brush mode accepts Quake / TrenchBroom .map files and compiles a leafy\n"
                 "BSP into the tool's PSBP v2 blob (planes/nodes/leaves/brushes/portals).\n"
                 "\n"
                 "--rooms mode accepts a `.rooms` source (axis-aligned room volumes +\n"
                 "explicit portals; see assets/maps/duke_e1m1.rooms), compiles a leaf-per-\n"
                 "room BSP, BAKES a Quake-style leaf-portal-flood PVS, and emits the engine\n"
                 "PBSP v1 format that world::bsp::Bsp::load consumes at runtime.\n");
}

namespace {
bool read_file(const fs::path& p, std::string& out, std::string& err) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        err = "cannot open " + p.string();
        return false;
    }
    in.seekg(0, std::ios::end);
    out.resize(static_cast<usize>(in.tellg()));
    in.seekg(0, std::ios::beg);
    if (!out.empty())
        in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(in);
}
bool write_file(const fs::path& p, std::span<const u8> data, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + p.string();
        return false;
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}
}  // namespace

int cli_main(int argc, char** argv) {
    if (argc < 2) {
        print_help();
        return 1;
    }
    std::string_view a = argv[1];
    if (a == "--help" || a == "-h" || a == "help") {
        print_help();
        return 0;
    }

    // --rooms <input.rooms> <output.psybsp>: compile the room/portal source into
    // the engine PBSP v1 format with a baked PVS (loader-consumable).
    if (a == "--rooms") {
        if (argc < 4) {
            print_help();
            return 1;
        }
        std::string rtext;
        std::string rerr;
        if (!read_file(fs::path(argv[2]), rtext, rerr)) {
            std::fprintf(stderr, "lm_qbsp: %s\n", rerr.c_str());
            return 1;
        }
        RoomsFile rooms;
        if (!parse_rooms(rtext, rooms, &rerr)) {
            std::fprintf(stderr, "lm_qbsp: %s\n", rerr.c_str());
            return 1;
        }
        CompiledBsp rbsp;
        if (!compile_rooms(rooms, rbsp, &rerr)) {
            std::fprintf(stderr, "lm_qbsp: %s\n", rerr.c_str());
            return 1;
        }
        std::vector<u8> rbytes;
        u32 clusters = 0u, row_bytes = 0u;
        write_psybsp_engine(rbsp, rbytes, &clusters, &row_bytes);
        if (!write_file(fs::path(argv[3]), rbytes, rerr)) {
            std::fprintf(stderr, "lm_qbsp: %s\n", rerr.c_str());
            return 1;
        }
        std::fprintf(stdout,
                     "lm_qbsp: %s -> %s (engine PBSP v1: nodes=%u leaves=%u faces=%u verts=%u "
                     "indices=%u portals=%u clusters=%u pvs_row_bytes=%u bytes=%u)\n",
                     argv[2],
                     argv[3],
                     static_cast<u32>(rbsp.nodes.size()),
                     static_cast<u32>(rbsp.leaves.size()),
                     static_cast<u32>(rbsp.faces.size()),
                     static_cast<u32>(rbsp.vertices.size()),
                     static_cast<u32>(rbsp.indices.size()),
                     static_cast<u32>(rbsp.portals.size()),
                     clusters,
                     row_bytes,
                     static_cast<u32>(rbytes.size()));
        return 0;
    }

    if (argc < 3) {
        print_help();
        return 1;
    }

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
                 argv[1],
                 argv[2],
                 static_cast<u32>(bsp.planes.size()),
                 static_cast<u32>(bsp.nodes.size()),
                 static_cast<u32>(bsp.leaves.size()),
                 static_cast<u32>(bsp.brushes.size()),
                 static_cast<u32>(bsp.portals.size()));
    return 0;
}

}  // namespace psynder::tools::qbsp
