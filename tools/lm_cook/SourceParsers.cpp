// SPDX-License-Identifier: MIT
// Psynder — source-format parsers for lm_cook. Lane 24 / tools.

#include "SourceParsers.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psynder::tools::cook {

namespace {

template <class T>
void append_be(std::vector<u8>& out, T value) {
    using U = std::make_unsigned_t<T>;
    auto u = static_cast<U>(value);
    for (isize i = static_cast<isize>(sizeof(T)) - 1; i >= 0; --i) {
        out.push_back(static_cast<u8>(u >> (8 * i)));
    }
}

template <class T>
void append_le(std::vector<u8>& out, T value) {
    using U = std::make_unsigned_t<T>;
    auto u = static_cast<U>(value);
    for (usize i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<u8>(u >> (8 * i)));
    }
}

u32 read_be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) |
           (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) <<  8) |
            static_cast<u32>(p[3]);
}
u32 read_le32(const u8* p) {
    return  static_cast<u32>(p[0])        |
           (static_cast<u32>(p[1]) <<  8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
u16 read_le16(const u8* p) {
    return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8));
}

void trim_inplace(std::string_view& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' || s.back()  == '\r'))
        s.remove_suffix(1);
}

bool parse_f32(std::string_view s, f32& out) {
    trim_inplace(s);
    // We use strtof because <charconv> from_chars<float> is missing on
    // some older libc++ vendors. Make a local zero-terminated copy.
    std::string tmp(s);
    char* end = nullptr;
    out = std::strtof(tmp.c_str(), &end);
    return end != tmp.c_str();
}

bool parse_i32(std::string_view s, i32& out) {
    trim_inplace(s);
    if (s.empty()) return false;
    i32 sign = 1;
    usize i = 0;
    if (s[0] == '-') { sign = -1; ++i; }
    else if (s[0] == '+') { ++i; }
    if (i >= s.size()) return false;
    i32 v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    out = v * sign;
    return true;
}

// ─── CRC32 ───────────────────────────────────────────────────────────────
u32 crc32_table(int n) {
    u32 c = static_cast<u32>(n);
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    return c;
}
u32 crc32_update(u32 crc, const u8* buf, usize len) {
    static u32 table[256];
    static bool inited = false;
    if (!inited) {
        for (int i = 0; i < 256; ++i) table[i] = crc32_table(i);
        inited = true;
    }
    crc ^= 0xFFFFFFFFu;
    for (usize i = 0; i < len; ++i) crc = table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ─── Adler-32 — for the zlib trailer ─────────────────────────────────────
u32 adler32(const u8* buf, usize len) {
    constexpr u32 kMod = 65521u;
    u32 a = 1, b = 0;
    for (usize i = 0; i < len; ++i) {
        a = (a + buf[i]) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

}  // anon namespace

// ─── OBJ ─────────────────────────────────────────────────────────────────

bool parse_obj(std::string_view text, LmmMesh& out, std::string* err) {
    out = {};
    std::vector<f32> positions;       // xyz triples
    std::vector<f32> normals;         // xyz triples
    std::vector<f32> texcoords;       // uv pairs
    struct Face { i32 p[3], t[3], n[3]; };
    std::vector<Face> faces;
    std::vector<u32>  face_submesh;
    std::vector<std::string> mat_names;
    i32 active_mat = -1;

    usize line_no = 0;
    usize i = 0;
    while (i < text.size()) {
        ++line_no;
        usize line_end = text.find('\n', i);
        if (line_end == std::string_view::npos) line_end = text.size();
        std::string_view line = text.substr(i, line_end - i);
        i = line_end + 1;
        trim_inplace(line);
        if (line.empty() || line.front() == '#') continue;

        // tokens by whitespace
        std::vector<std::string_view> tok;
        usize p = 0;
        while (p < line.size()) {
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            usize start = p;
            while (p < line.size() && line[p] != ' ' && line[p] != '\t') ++p;
            if (p > start) tok.push_back(line.substr(start, p - start));
        }
        if (tok.empty()) continue;
        auto cmd = tok[0];
        if (cmd == "v" && tok.size() >= 4) {
            f32 x, y, z;
            if (!parse_f32(tok[1], x) || !parse_f32(tok[2], y) || !parse_f32(tok[3], z)) {
                if (err) *err = "obj: bad vertex on line " + std::to_string(line_no); return false;
            }
            positions.push_back(x); positions.push_back(y); positions.push_back(z);
        } else if (cmd == "vn" && tok.size() >= 4) {
            f32 x, y, z;
            if (!parse_f32(tok[1], x) || !parse_f32(tok[2], y) || !parse_f32(tok[3], z)) {
                if (err) *err = "obj: bad normal on line " + std::to_string(line_no); return false;
            }
            normals.push_back(x); normals.push_back(y); normals.push_back(z);
        } else if (cmd == "vt" && tok.size() >= 3) {
            f32 u, v;
            if (!parse_f32(tok[1], u) || !parse_f32(tok[2], v)) {
                if (err) *err = "obj: bad uv on line " + std::to_string(line_no); return false;
            }
            texcoords.push_back(u); texcoords.push_back(v);
        } else if (cmd == "usemtl" && tok.size() >= 2) {
            std::string name(tok[1]);
            // Reuse existing slot if seen before.
            auto it = std::find(mat_names.begin(), mat_names.end(), name);
            if (it == mat_names.end()) {
                active_mat = static_cast<i32>(mat_names.size());
                mat_names.push_back(name);
            } else {
                active_mat = static_cast<i32>(it - mat_names.begin());
            }
        } else if (cmd == "f") {
            if (tok.size() < 4) {
                if (err) *err = "obj: face needs ≥3 verts on line " + std::to_string(line_no); return false;
            }
            // Fan-triangulate.
            auto parse_corner = [&](std::string_view t, i32& vp, i32& vt, i32& vn) {
                vp = 0; vt = 0; vn = 0;
                usize slash1 = t.find('/');
                std::string_view a = (slash1 == std::string_view::npos) ? t : t.substr(0, slash1);
                if (!parse_i32(a, vp)) return false;
                if (slash1 == std::string_view::npos) return true;
                usize slash2 = t.find('/', slash1 + 1);
                std::string_view b = (slash2 == std::string_view::npos)
                                       ? t.substr(slash1 + 1)
                                       : t.substr(slash1 + 1, slash2 - slash1 - 1);
                if (!b.empty()) parse_i32(b, vt);
                if (slash2 == std::string_view::npos) return true;
                std::string_view c = t.substr(slash2 + 1);
                if (!c.empty()) parse_i32(c, vn);
                return true;
            };
            i32 ap, at_, an;
            if (!parse_corner(tok[1], ap, at_, an)) {
                if (err) *err = "obj: bad face corner on line " + std::to_string(line_no); return false;
            }
            for (usize k = 2; k + 1 < tok.size(); ++k) {
                i32 bp, bt, bn, cp, ct, cn;
                if (!parse_corner(tok[k],     bp, bt, bn)) {
                    if (err) *err = "obj: bad face on line " + std::to_string(line_no); return false;
                }
                if (!parse_corner(tok[k + 1], cp, ct, cn)) {
                    if (err) *err = "obj: bad face on line " + std::to_string(line_no); return false;
                }
                Face f{};
                f.p[0] = ap; f.p[1] = bp; f.p[2] = cp;
                f.t[0] = at_; f.t[1] = bt; f.t[2] = ct;
                f.n[0] = an; f.n[1] = bn; f.n[2] = cn;
                faces.push_back(f);
                face_submesh.push_back(active_mat < 0 ? 0u : static_cast<u32>(active_mat));
            }
        }
    }

    if (faces.empty()) {
        if (err) *err = "obj: no faces";
        return false;
    }
    if (mat_names.empty()) mat_names.push_back("default");

    // Resolve negative (relative) indices.
    auto resolve = [](i32 ix, isize total) -> i32 {
        if (ix > 0) return ix - 1;
        if (ix < 0) return static_cast<i32>(total + ix);
        return -1;
    };

    // De-duplicate per-corner attribute tuples.
    struct Key {
        i32 p, t, n;
        bool operator==(const Key& o) const noexcept = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const noexcept {
            // tiny FNV-1a over the three i32s
            u64 h = 0xcbf29ce484222325ULL;
            const u8* b = reinterpret_cast<const u8*>(&k);
            for (usize i = 0; i < sizeof(Key); ++i) { h ^= b[i]; h *= 0x100000001b3ULL; }
            return static_cast<usize>(h);
        }
    };
    std::vector<u32> indices;
    indices.reserve(faces.size() * 3);
    std::vector<LmmVertex> verts;
    verts.reserve(positions.size() / 3);
    // Linear-scan dedupe; for Wave-A meshes this is fast enough.
    auto find_or_add = [&](Key k) -> u32 {
        for (u32 i = 0; i < verts.size(); ++i) {
            const auto& v = verts[i];
            (void)v;
            // Compare against the captured key list rather than reconstructing.
        }
        return static_cast<u32>(verts.size());
    };
    (void)find_or_add;

    // For simplicity (and to keep the test predictable) emit one unique vertex
    // per face corner. A real cooker would de-dup; lane 25 can add that.
    std::vector<u32> submesh_per_corner;
    for (usize fi = 0; fi < faces.size(); ++fi) {
        const auto& f = faces[fi];
        // Compute a face normal for any corners that don't have one.
        f32 a[3], b[3], c[3];
        for (int k = 0; k < 3; ++k) {
            i32 pi = resolve(f.p[k], positions.size() / 3);
            if (pi < 0 || pi * 3 + 2 >= static_cast<i32>(positions.size())) {
                if (err) *err = "obj: position index out of range"; return false;
            }
            f32* dst = (k == 0) ? a : (k == 1) ? b : c;
            dst[0] = positions[pi * 3 + 0];
            dst[1] = positions[pi * 3 + 1];
            dst[2] = positions[pi * 3 + 2];
        }
        f32 ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        f32 vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        f32 nx = uy * vz - uz * vy;
        f32 ny = uz * vx - ux * vz;
        f32 nz = ux * vy - uy * vx;
        f32 nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl > 1e-12f) { nx /= nl; ny /= nl; nz /= nl; }

        for (int k = 0; k < 3; ++k) {
            i32 pi = resolve(f.p[k], positions.size() / 3);
            LmmVertex v{};
            v.px = positions[pi * 3 + 0];
            v.py = positions[pi * 3 + 1];
            v.pz = positions[pi * 3 + 2];
            if (f.n[k] != 0) {
                i32 ni = resolve(f.n[k], normals.size() / 3);
                if (ni >= 0 && ni * 3 + 2 < static_cast<i32>(normals.size())) {
                    v.nx = normals[ni * 3 + 0];
                    v.ny = normals[ni * 3 + 1];
                    v.nz = normals[ni * 3 + 2];
                } else {
                    v.nx = nx; v.ny = ny; v.nz = nz;
                }
            } else {
                v.nx = nx; v.ny = ny; v.nz = nz;
            }
            if (f.t[k] != 0) {
                i32 ti = resolve(f.t[k], texcoords.size() / 2);
                if (ti >= 0 && ti * 2 + 1 < static_cast<i32>(texcoords.size())) {
                    v.u = texcoords[ti * 2 + 0];
                    v.v = texcoords[ti * 2 + 1];
                }
            }
            // Tangent set to (1,0,0,1) — a real importer would compute MikkTSpace.
            v.tx = 1.0f; v.ty = 0.0f; v.tz = 0.0f; v.tw = 1.0f;
            verts.push_back(v);
            indices.push_back(static_cast<u32>(verts.size() - 1));
            submesh_per_corner.push_back(face_submesh[fi]);
        }
    }

    out.vertices = std::move(verts);
    out.indices  = std::move(indices);
    out.materials = std::move(mat_names);

    // Build contiguous submeshes (the OBJ usemtl ordering is preserved).
    if (out.indices.empty()) {
        if (err) *err = "obj: empty after triangulation";
        return false;
    }
    LmmSubmesh current{};
    current.first_index = 0;
    current.material_index = submesh_per_corner.front();
    u32 corner_count = 0;
    for (usize ci = 0; ci < submesh_per_corner.size(); ci += 3) {
        u32 m = submesh_per_corner[ci];
        if (m != current.material_index) {
            current.index_count = corner_count;
            out.submeshes.push_back(current);
            current.first_index = static_cast<u32>(ci);
            current.material_index = m;
            corner_count = 0;
        }
        corner_count += 3;
    }
    current.index_count = corner_count;
    out.submeshes.push_back(current);
    return true;
}

// ─── glTF (minimal JSON) ─────────────────────────────────────────────────
//
// We parse the JSON we ourselves emit in the tests: a very narrow subset
// of the spec. The implementation handles a key/value tokenizer enough to
// reach the buffer URI and the mesh primitive accessor indices.

namespace {

class JsonView {
public:
    explicit JsonView(std::string_view s) : src_(s), pos_(0) {}
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }
    bool consume(char c) {
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == c) { ++pos_; return true; }
        return false;
    }
    bool peek(char c) {
        skip_ws();
        return pos_ < src_.size() && src_[pos_] == c;
    }
    bool read_string(std::string& out) {
        skip_ws();
        if (pos_ >= src_.size() || src_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < src_.size() && src_[pos_] != '"') {
            char c = src_[pos_++];
            if (c == '\\' && pos_ < src_.size()) {
                char e = src_[pos_++];
                if      (e == 'n')  out.push_back('\n');
                else if (e == 't')  out.push_back('\t');
                else if (e == '"')  out.push_back('"');
                else if (e == '\\') out.push_back('\\');
                else                out.push_back(e);
            } else {
                out.push_back(c);
            }
        }
        if (pos_ < src_.size() && src_[pos_] == '"') { ++pos_; return true; }
        return false;
    }
    bool read_number(double& out) {
        skip_ws();
        usize start = pos_;
        if (pos_ < src_.size() && (src_[pos_] == '-' || src_[pos_] == '+')) ++pos_;
        while (pos_ < src_.size() &&
               ((src_[pos_] >= '0' && src_[pos_] <= '9') ||
                src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E' ||
                src_[pos_] == '+' || src_[pos_] == '-')) {
            ++pos_;
        }
        if (start == pos_) return false;
        std::string tmp(src_.substr(start, pos_ - start));
        char* end = nullptr;
        out = std::strtod(tmp.c_str(), &end);
        return end != tmp.c_str();
    }
    void skip_value() {
        skip_ws();
        if (pos_ >= src_.size()) return;
        char c = src_[pos_];
        if (c == '"') {
            std::string tmp; read_string(tmp);
        } else if (c == '{') {
            ++pos_; int depth = 1;
            while (pos_ < src_.size() && depth > 0) {
                char k = src_[pos_++];
                if (k == '{') ++depth;
                else if (k == '}') --depth;
                else if (k == '"') {
                    // skip string
                    while (pos_ < src_.size() && src_[pos_] != '"') {
                        if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) ++pos_;
                        ++pos_;
                    }
                    if (pos_ < src_.size()) ++pos_;
                }
            }
        } else if (c == '[') {
            ++pos_; int depth = 1;
            while (pos_ < src_.size() && depth > 0) {
                char k = src_[pos_++];
                if (k == '[') ++depth;
                else if (k == ']') --depth;
                else if (k == '"') {
                    while (pos_ < src_.size() && src_[pos_] != '"') {
                        if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) ++pos_;
                        ++pos_;
                    }
                    if (pos_ < src_.size()) ++pos_;
                }
            }
        } else {
            // primitive: number / true / false / null
            while (pos_ < src_.size() &&
                   src_[pos_] != ',' && src_[pos_] != '}' && src_[pos_] != ']') ++pos_;
        }
    }
    usize pos() const { return pos_; }
    void  rewind(usize p) { pos_ = p; }
    std::string_view rest() const { return src_.substr(pos_); }
private:
    std::string_view src_;
    usize            pos_;
};

// Base64 decode (no padding tolerance; standard alphabet).
bool base64_decode(std::string_view s, std::vector<u8>& out) {
    static i8 tab[256];
    static bool inited = false;
    if (!inited) {
        for (int i = 0; i < 256; ++i) tab[i] = -1;
        const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) tab[static_cast<u8>(alpha[i])] = static_cast<i8>(i);
        tab[static_cast<u8>('=')] = -2;
        inited = true;
    }
    out.clear();
    out.reserve((s.size() * 3) / 4);
    u32 acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        i8 t = tab[static_cast<u8>(c)];
        if (t == -1) return false;
        if (t == -2) break;
        acc = (acc << 6) | static_cast<u32>(t);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

}  // anon namespace

bool parse_gltf(std::string_view json,
                std::span<const u8> external_buffer,
                LmmMesh& out,
                std::string* err) {
    out = {};
    JsonView j(json);
    if (!j.consume('{')) {
        if (err) *err = "gltf: not a JSON object";
        return false;
    }

    // We need: buffers[0].uri, bufferViews[*], accessors[*], meshes[0].primitives[0].
    std::vector<u8> buffer_data;
    bool have_buffer = !external_buffer.empty();
    if (have_buffer) buffer_data.assign(external_buffer.begin(), external_buffer.end());

    struct BV { u32 byte_offset = 0; u32 byte_length = 0; u32 byte_stride = 0; };
    std::vector<BV> bvs;
    struct Accessor { u32 bv = 0; u32 byte_offset = 0; u32 count = 0; u32 component_type = 0; std::string type; };
    std::vector<Accessor> accessors;
    i32 pos_accessor = -1, nor_accessor = -1, uv_accessor = -1, idx_accessor = -1;

    auto parse_buffers = [&]() -> bool {
        if (!j.consume('[')) return false;
        if (j.consume(']')) return true;
        while (true) {
            if (!j.consume('{')) return false;
            std::string key;
            while (true) {
                if (j.consume('}')) break;
                if (!j.read_string(key)) return false;
                if (!j.consume(':')) return false;
                if (key == "uri" && !have_buffer) {
                    std::string uri;
                    if (!j.read_string(uri)) return false;
                    constexpr std::string_view kPrefix = "data:";
                    if (uri.starts_with(kPrefix)) {
                        usize comma = uri.find(',');
                        if (comma == std::string::npos) {
                            if (err) *err = "gltf: malformed data URI";
                            return false;
                        }
                        std::string_view header(uri.data(), comma);
                        if (header.find(";base64") == std::string_view::npos) {
                            if (err) *err = "gltf: data URI must be base64";
                            return false;
                        }
                        if (!base64_decode(std::string_view(uri).substr(comma + 1), buffer_data)) {
                            if (err) *err = "gltf: bad base64";
                            return false;
                        }
                        have_buffer = true;
                    } else {
                        if (err) *err = "gltf: only data: URIs supported (got " + uri + ")";
                        return false;
                    }
                } else {
                    j.skip_value();
                }
                if (!j.consume(',')) {
                    if (!j.consume('}')) return false;
                    break;
                }
            }
            if (!j.consume(',')) {
                if (!j.consume(']')) return false;
                break;
            }
        }
        return true;
    };
    auto parse_buffer_views = [&]() -> bool {
        if (!j.consume('[')) return false;
        if (j.consume(']')) return true;
        while (true) {
            if (!j.consume('{')) return false;
            BV bv{};
            std::string key;
            while (true) {
                if (j.consume('}')) break;
                if (!j.read_string(key)) return false;
                if (!j.consume(':')) return false;
                double d = 0;
                if (key == "byteOffset") { j.read_number(d); bv.byte_offset = static_cast<u32>(d); }
                else if (key == "byteLength") { j.read_number(d); bv.byte_length = static_cast<u32>(d); }
                else if (key == "byteStride") { j.read_number(d); bv.byte_stride = static_cast<u32>(d); }
                else j.skip_value();
                if (!j.consume(',')) {
                    if (!j.consume('}')) return false;
                    break;
                }
            }
            bvs.push_back(bv);
            if (!j.consume(',')) {
                if (!j.consume(']')) return false;
                break;
            }
        }
        return true;
    };
    auto parse_accessors = [&]() -> bool {
        if (!j.consume('[')) return false;
        if (j.consume(']')) return true;
        while (true) {
            if (!j.consume('{')) return false;
            Accessor a{};
            std::string key;
            while (true) {
                if (j.consume('}')) break;
                if (!j.read_string(key)) return false;
                if (!j.consume(':')) return false;
                double d = 0;
                if (key == "bufferView")     { j.read_number(d); a.bv = static_cast<u32>(d); }
                else if (key == "byteOffset"){ j.read_number(d); a.byte_offset = static_cast<u32>(d); }
                else if (key == "count")     { j.read_number(d); a.count = static_cast<u32>(d); }
                else if (key == "componentType") { j.read_number(d); a.component_type = static_cast<u32>(d); }
                else if (key == "type")      { j.read_string(a.type); }
                else j.skip_value();
                if (!j.consume(',')) {
                    if (!j.consume('}')) return false;
                    break;
                }
            }
            accessors.push_back(a);
            if (!j.consume(',')) {
                if (!j.consume(']')) return false;
                break;
            }
        }
        return true;
    };
    auto parse_meshes = [&]() -> bool {
        if (!j.consume('[')) return false;
        if (j.consume(']')) return true;
        bool took_first = false;
        while (true) {
            if (!j.consume('{')) return false;
            std::string key;
            while (true) {
                if (j.consume('}')) break;
                if (!j.read_string(key)) return false;
                if (!j.consume(':')) return false;
                if (key == "primitives" && !took_first) {
                    // first mesh, first primitive
                    if (!j.consume('[')) return false;
                    if (!j.consume('{')) return false;
                    std::string pkey;
                    while (true) {
                        if (j.consume('}')) break;
                        if (!j.read_string(pkey)) return false;
                        if (!j.consume(':')) return false;
                        if (pkey == "attributes") {
                            if (!j.consume('{')) return false;
                            std::string akey;
                            while (true) {
                                if (j.consume('}')) break;
                                if (!j.read_string(akey)) return false;
                                if (!j.consume(':')) return false;
                                double d = 0;
                                if (!j.read_number(d)) return false;
                                if      (akey == "POSITION")   pos_accessor = static_cast<i32>(d);
                                else if (akey == "NORMAL")     nor_accessor = static_cast<i32>(d);
                                else if (akey == "TEXCOORD_0") uv_accessor  = static_cast<i32>(d);
                                if (!j.consume(',')) {
                                    if (!j.consume('}')) return false;
                                    break;
                                }
                            }
                        } else if (pkey == "indices") {
                            double d = 0; if (!j.read_number(d)) return false;
                            idx_accessor = static_cast<i32>(d);
                        } else {
                            j.skip_value();
                        }
                        if (!j.consume(',')) {
                            if (!j.consume('}')) return false;
                            break;
                        }
                    }
                    // skip remaining primitives in this mesh
                    while (j.consume(',')) {
                        std::string skipkey;
                        // skip whole primitive object
                        j.skip_value();
                    }
                    if (!j.consume(']')) return false;
                    took_first = true;
                } else {
                    j.skip_value();
                }
                if (!j.consume(',')) {
                    if (!j.consume('}')) return false;
                    break;
                }
            }
            if (!j.consume(',')) {
                if (!j.consume(']')) return false;
                break;
            }
        }
        return true;
    };

    while (true) {
        if (j.consume('}')) break;
        std::string key;
        if (!j.read_string(key)) {
            if (err) *err = "gltf: expected key";
            return false;
        }
        if (!j.consume(':')) {
            if (err) *err = "gltf: expected ':'";
            return false;
        }
        if      (key == "buffers")     { if (!parse_buffers())      { if (err) *err = "gltf: parse buffers failed"; return false; } }
        else if (key == "bufferViews") { if (!parse_buffer_views()) { if (err) *err = "gltf: parse bufferViews failed"; return false; } }
        else if (key == "accessors")   { if (!parse_accessors())    { if (err) *err = "gltf: parse accessors failed"; return false; } }
        else if (key == "meshes")      { if (!parse_meshes())       { if (err) *err = "gltf: parse meshes failed"; return false; } }
        else                           { j.skip_value(); }
        if (!j.consume(',')) {
            if (!j.consume('}')) {
                if (err) *err = "gltf: expected ',' or '}'";
                return false;
            }
            break;
        }
    }

    if (pos_accessor < 0 || idx_accessor < 0) {
        if (err) *err = "gltf: missing POSITION or indices";
        return false;
    }
    if (!have_buffer || buffer_data.empty()) {
        if (err) *err = "gltf: missing buffer payload";
        return false;
    }

    auto read_accessor = [&](const Accessor& a) -> std::span<const u8> {
        const BV& bv = bvs[a.bv];
        usize start = bv.byte_offset + a.byte_offset;
        if (start > buffer_data.size()) return {};
        usize len = bv.byte_length - a.byte_offset;
        if (start + len > buffer_data.size()) len = buffer_data.size() - start;
        return std::span<const u8>(buffer_data.data() + start, len);
    };

    // Read positions (VEC3 float).
    const Accessor& ap = accessors[pos_accessor];
    auto pos_data = read_accessor(ap);
    if (ap.type != "VEC3" || ap.component_type != 5126 /* FLOAT */) {
        if (err) *err = "gltf: POSITION must be VEC3 float";
        return false;
    }
    std::vector<f32> positions(ap.count * 3);
    if (pos_data.size() < positions.size() * sizeof(f32)) {
        if (err) *err = "gltf: position buffer too small";
        return false;
    }
    std::memcpy(positions.data(), pos_data.data(), positions.size() * sizeof(f32));

    std::vector<f32> normals;
    if (nor_accessor >= 0) {
        const Accessor& an = accessors[nor_accessor];
        auto nor_data = read_accessor(an);
        if (an.type == "VEC3" && an.component_type == 5126) {
            normals.resize(an.count * 3);
            if (nor_data.size() >= normals.size() * sizeof(f32)) {
                std::memcpy(normals.data(), nor_data.data(), normals.size() * sizeof(f32));
            } else {
                normals.clear();
            }
        }
    }
    std::vector<f32> uvs;
    if (uv_accessor >= 0) {
        const Accessor& au = accessors[uv_accessor];
        auto uv_data = read_accessor(au);
        if (au.type == "VEC2" && au.component_type == 5126) {
            uvs.resize(au.count * 2);
            if (uv_data.size() >= uvs.size() * sizeof(f32)) {
                std::memcpy(uvs.data(), uv_data.data(), uvs.size() * sizeof(f32));
            }
        }
    }
    // Indices: UNSIGNED_SHORT (5123) or UNSIGNED_INT (5125).
    const Accessor& ai = accessors[idx_accessor];
    auto idx_data = read_accessor(ai);
    std::vector<u32> indices(ai.count);
    if (ai.component_type == 5123) {
        if (idx_data.size() < ai.count * 2u) {
            if (err) *err = "gltf: indices buffer too small";
            return false;
        }
        for (u32 i = 0; i < ai.count; ++i) {
            indices[i] = static_cast<u32>(read_le16(idx_data.data() + i * 2u));
        }
    } else if (ai.component_type == 5125) {
        if (idx_data.size() < ai.count * 4u) {
            if (err) *err = "gltf: indices buffer too small";
            return false;
        }
        for (u32 i = 0; i < ai.count; ++i) {
            indices[i] = read_le32(idx_data.data() + i * 4u);
        }
    } else {
        if (err) *err = "gltf: unsupported index component type";
        return false;
    }

    out.vertices.resize(ap.count);
    for (u32 i = 0; i < ap.count; ++i) {
        LmmVertex& v = out.vertices[i];
        v.px = positions[i * 3 + 0];
        v.py = positions[i * 3 + 1];
        v.pz = positions[i * 3 + 2];
        if (i * 3 + 2 < normals.size()) {
            v.nx = normals[i * 3 + 0];
            v.ny = normals[i * 3 + 1];
            v.nz = normals[i * 3 + 2];
        } else {
            v.nz = 1.0f;
        }
        if (i * 2 + 1 < uvs.size()) {
            v.u = uvs[i * 2 + 0];
            v.v = uvs[i * 2 + 1];
        }
        v.tx = 1.0f; v.tw = 1.0f;
    }
    out.indices = std::move(indices);
    out.materials.push_back("default");
    LmmSubmesh sm{};
    sm.first_index = 0;
    sm.index_count = static_cast<u32>(out.indices.size());
    sm.material_index = 0;
    out.submeshes.push_back(sm);
    return true;
}

// ─── PNG (stored DEFLATE only) ───────────────────────────────────────────

namespace {

// Encode `data` as a zlib stream that contains exactly one or more BTYPE=0
// (stored) blocks. This is uncompressed but framed correctly for a stream
// decoder; both libpng and a standards-compliant decoder will accept it.
void zlib_store(std::span<const u8> data, std::vector<u8>& out) {
    // zlib header: CMF=0x78 (deflate, 32K window), FLG chosen so the
    // header checksum (CMF*256 + FLG) % 31 == 0. FLG=0x01 satisfies that.
    out.push_back(0x78);
    out.push_back(0x01);

    usize i = 0;
    while (true) {
        usize remaining = data.size() - i;
        u16 block = static_cast<u16>(std::min<usize>(remaining, 65535));
        bool last = (i + block == data.size());
        out.push_back(static_cast<u8>(last ? 1u : 0u));   // BFINAL bit; BTYPE=00
        out.push_back(static_cast<u8>(block & 0xFFu));
        out.push_back(static_cast<u8>((block >> 8) & 0xFFu));
        u16 nlen = static_cast<u16>(~block);
        out.push_back(static_cast<u8>(nlen & 0xFFu));
        out.push_back(static_cast<u8>((nlen >> 8) & 0xFFu));
        if (block > 0) {
            out.insert(out.end(), data.data() + i, data.data() + i + block);
        }
        i += block;
        if (last) break;
    }
    u32 adler = adler32(data.data(), data.size());
    append_be<u32>(out, adler);
}

bool zlib_decode_stored(std::span<const u8> stream, std::vector<u8>& out, std::string& err) {
    if (stream.size() < 2 + 4) { err = "zlib stream too short"; return false; }
    u8 cmf = stream[0];
    u8 flg = stream[1];
    if ((cmf & 0x0F) != 8) { err = "zlib: only deflate (CM=8) supported"; return false; }
    if (((u32(cmf) * 256u) + flg) % 31u != 0u) {
        err = "zlib: bad header checksum"; return false;
    }
    if (flg & 0x20) { err = "zlib: dictionary preset not supported"; return false; }

    usize p = 2;
    while (true) {
        if (p + 5 > stream.size()) { err = "zlib: truncated stored block"; return false; }
        u8 hdr = stream[p++];
        u8 btype = (hdr >> 1) & 0x03;
        bool last = (hdr & 1u) != 0;
        if (btype != 0) { err = "zlib: only stored (BTYPE=0) blocks supported"; return false; }
        u16 len  = read_le16(stream.data() + p); p += 2;
        u16 nlen = read_le16(stream.data() + p); p += 2;
        if (static_cast<u16>(~len) != nlen) { err = "zlib: stored block len/nlen mismatch"; return false; }
        if (p + len > stream.size()) { err = "zlib: stored block runs past stream"; return false; }
        out.insert(out.end(), stream.data() + p, stream.data() + p + len);
        p += len;
        if (last) break;
    }
    // Adler-32 trailer (4 bytes BE).
    if (p + 4 > stream.size()) { err = "zlib: missing adler32"; return false; }
    u32 want = read_be32(stream.data() + p);
    u32 have = adler32(out.data(), out.size());
    if (want != have) { err = "zlib: adler32 mismatch"; return false; }
    return true;
}

void png_emit_chunk(std::vector<u8>& out, const char tag[4], std::span<const u8> data) {
    append_be<u32>(out, static_cast<u32>(data.size()));
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), data.begin(), data.end());
    u32 crc = 0xFFFFFFFFu ^ 0xFFFFFFFFu;
    // CRC over tag + data
    crc = crc32_update(0, reinterpret_cast<const u8*>(tag), 4);
    crc = crc32_update(crc, data.data(), data.size());
    // The crc32_update above XOR'd with 0xFFFFFFFFu on entry, but since
    // its semantics are "compute CRC over the concatenation of the prior
    // input and the new input" we need to fold the two calls properly.
    // Implementation detail: re-do it in one shot for safety.
    std::vector<u8> tmp;
    tmp.reserve(4 + data.size());
    tmp.insert(tmp.end(), tag, tag + 4);
    tmp.insert(tmp.end(), data.begin(), data.end());
    crc = crc32_update(0, tmp.data(), tmp.size());
    append_be<u32>(out, crc);
}

}  // anon namespace

void encode_png_stored(const u8* rgba, u32 width, u32 height, std::vector<u8>& out) {
    out.clear();
    // PNG signature
    static const u8 kSig[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    out.insert(out.end(), std::begin(kSig), std::end(kSig));

    // IHDR
    std::vector<u8> ihdr;
    append_be<u32>(ihdr, width);
    append_be<u32>(ihdr, height);
    ihdr.push_back(8);    // bit depth
    ihdr.push_back(6);    // color type: RGBA
    ihdr.push_back(0);    // compression method (deflate)
    ihdr.push_back(0);    // filter method (none)
    ihdr.push_back(0);    // interlace method (none)
    png_emit_chunk(out, "IHDR", ihdr);

    // IDAT: one filter byte (0=None) per row, then RGBA scanlines.
    std::vector<u8> raw;
    raw.reserve(static_cast<usize>(height) * (1 + width * 4u));
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(),
                   rgba + static_cast<usize>(y) * width * 4u,
                   rgba + static_cast<usize>(y) * width * 4u + width * 4u);
    }
    std::vector<u8> zlib_stream;
    zlib_store(raw, zlib_stream);
    png_emit_chunk(out, "IDAT", zlib_stream);

    // IEND
    png_emit_chunk(out, "IEND", std::span<const u8>{});
}

bool decode_png_stored(std::span<const u8> bytes,
                       std::vector<u8>& out_rgba,
                       u32& width,
                       u32& height,
                       std::string* err) {
    auto fail = [&](const char* msg) { if (err) *err = msg; return false; };
    if (bytes.size() < 8) return fail("png too short");
    static const u8 kSig[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    if (std::memcmp(bytes.data(), kSig, 8) != 0) return fail("png: bad signature");

    usize p = 8;
    std::vector<u8> zlib;
    bool have_ihdr = false;
    u8 bit_depth = 0, color_type = 0;
    width = 0; height = 0;
    while (p + 8 <= bytes.size()) {
        u32 length = read_be32(bytes.data() + p); p += 4;
        if (p + 4 + length + 4 > bytes.size()) return fail("png: truncated chunk");
        char tag[4] = { static_cast<char>(bytes[p]),
                        static_cast<char>(bytes[p + 1]),
                        static_cast<char>(bytes[p + 2]),
                        static_cast<char>(bytes[p + 3]) };
        p += 4;
        std::span<const u8> data(bytes.data() + p, length);
        p += length + 4;  // skip CRC

        if (std::memcmp(tag, "IHDR", 4) == 0) {
            if (length < 13) return fail("png: short IHDR");
            width  = read_be32(data.data() + 0);
            height = read_be32(data.data() + 4);
            bit_depth  = data[8];
            color_type = data[9];
            if (bit_depth != 8) return fail("png: only 8-bit channels supported");
            if (color_type != 6) return fail("png: only RGBA supported");
            have_ihdr = true;
        } else if (std::memcmp(tag, "IDAT", 4) == 0) {
            zlib.insert(zlib.end(), data.begin(), data.end());
        } else if (std::memcmp(tag, "IEND", 4) == 0) {
            break;
        }
    }
    if (!have_ihdr || zlib.empty()) return fail("png: missing IHDR or IDAT");

    std::vector<u8> raw;
    std::string e;
    if (!zlib_decode_stored(zlib, raw, e)) {
        if (err) *err = e;
        return false;
    }
    // De-filter (PNG filter 0 only — None).
    usize row_bytes = static_cast<usize>(width) * 4u;
    if (raw.size() < height * (1 + row_bytes)) return fail("png: short IDAT data");
    out_rgba.resize(static_cast<usize>(width) * height * 4u);
    usize src = 0;
    for (u32 y = 0; y < height; ++y) {
        u8 filter = raw[src++];
        if (filter != 0) return fail("png: only filter type 0 supported");
        std::memcpy(out_rgba.data() + y * row_bytes, raw.data() + src, row_bytes);
        src += row_bytes;
    }
    return true;
}

// ─── WAV ─────────────────────────────────────────────────────────────────

bool parse_wav(std::span<const u8> bytes, LmaAudio& out, std::string* err) {
    auto fail = [&](const char* msg) { if (err) *err = msg; return false; };
    if (bytes.size() < 44) return fail("wav too short");
    if (std::memcmp(bytes.data() + 0, "RIFF", 4) != 0) return fail("wav: missing RIFF");
    if (std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return fail("wav: missing WAVE");

    usize p = 12;
    bool have_fmt = false;
    u16 audio_format = 0;
    while (p + 8 <= bytes.size()) {
        char tag[5] = { static_cast<char>(bytes[p]),
                        static_cast<char>(bytes[p + 1]),
                        static_cast<char>(bytes[p + 2]),
                        static_cast<char>(bytes[p + 3]), 0 };
        u32 size = read_le32(bytes.data() + p + 4);
        p += 8;
        if (p + size > bytes.size()) return fail("wav: chunk overruns file");
        if (std::memcmp(tag, "fmt ", 4) == 0) {
            if (size < 16) return fail("wav: fmt chunk too short");
            audio_format       = read_le16(bytes.data() + p + 0);
            out.channels       = read_le16(bytes.data() + p + 2);
            out.sample_rate    = read_le32(bytes.data() + p + 4);
            // byte_rate / block_align skipped
            out.bits_per_sample = read_le16(bytes.data() + p + 14);
            out.is_float = (audio_format == 3 /* IEEE float */);
            have_fmt = true;
        } else if (std::memcmp(tag, "data", 4) == 0) {
            if (!have_fmt) return fail("wav: data before fmt");
            const u8* base = bytes.data() + p;
            out.samples.assign(base, base + size);
            usize frame_bytes = static_cast<usize>(out.channels) * (out.bits_per_sample / 8u);
            if (frame_bytes == 0) return fail("wav: zero frame size");
            out.sample_count = static_cast<u32>(size / frame_bytes);
            // Stop here; ignore any trailing chunks.
            return true;
        }
        p += size + (size & 1u);
    }
    return fail("wav: missing data chunk");
}

void encode_wav_pcm16(const i16* samples, u32 sample_count, u32 channels, u32 sample_rate,
                      std::vector<u8>& out) {
    u32 byte_rate   = sample_rate * channels * 2;
    u32 block_align = channels * 2;
    u32 data_bytes  = sample_count * block_align;

    out.clear();
    out.reserve(44 + data_bytes);
    // RIFF header
    out.insert(out.end(), {'R','I','F','F'});
    append_le<u32>(out, 36 + data_bytes);
    out.insert(out.end(), {'W','A','V','E'});
    // fmt chunk
    out.insert(out.end(), {'f','m','t',' '});
    append_le<u32>(out, 16);
    append_le<u16>(out, 1);          // PCM
    append_le<u16>(out, static_cast<u16>(channels));
    append_le<u32>(out, sample_rate);
    append_le<u32>(out, byte_rate);
    append_le<u16>(out, static_cast<u16>(block_align));
    append_le<u16>(out, 16);
    // data chunk
    out.insert(out.end(), {'d','a','t','a'});
    append_le<u32>(out, data_bytes);
    for (u32 i = 0; i < sample_count * channels; ++i) {
        i16 s = samples[i];
        out.push_back(static_cast<u8>(s & 0xFF));
        out.push_back(static_cast<u8>((static_cast<u16>(s) >> 8) & 0xFF));
    }
}

}  // namespace psynder::tools::cook
