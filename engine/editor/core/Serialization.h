// SPDX-License-Identifier: MIT
// Psynder — internal header. .psylevel + .psyc binary serialisation. The
// Editor.h facade dispatches save_level / load_level / save_contraption
// here. zstd compression is applied when the encoder is available
// (psynder_asset provides one); otherwise the raw payload is emitted
// with a header-flag bit cleared so the loader knows to skip decode.
//
// Format (little-endian, binary):
//
//   psylevel header: 16 bytes
//     u32 magic      'PSLV'         (0x564C5350)
//     u16 version    1
//     u16 flags      bit0 = zstd-compressed
//     u32 payload_sz uncompressed size of the body
//     u32 body_sz    on-disk size of the body
//   body (uncompressed):
//     u32 brush_count;  brush[brush_count]
//     u32 entity_count; entity[entity_count]
//     u32 body_count;   body[body_count]
//     u32 cons_count;   constraint[cons_count]
//     u32 hf_size_x; u32 hf_size_z; f32 hf_spacing; f32 hf_origin[3]; f32 heights[hf_size_x*hf_size_z]
//     u32 splat_size_x; u32 splat_size_z; f32 weights[splat_size_x*splat_size_z*4]
//
// .psyc is the same layout minus heightfield + splat sections (just
// bodies + constraints), with magic 'PSCN'.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include "editor/core/Brush.h"
#include "editor/core/Constraints.h"
#include "editor/core/Sculpt.h"
#include "editor/core/EditorState.h"

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

namespace psynder::editor::serial {

inline constexpr u32 kPsyLevelMagic = 0x564C5350u;  // 'PSLV' little-endian
inline constexpr u32 kPsyConMagic = 0x4E435350u;    // 'PSCN' little-endian
inline constexpr u16 kFormatVersion = 1;
inline constexpr u16 kFlagZstd = 1 << 0;

// Append `n` bytes from `src` to `out`.
PSY_FORCEINLINE void blob_append(std::vector<u8>& out, const void* src, usize n) {
    const u8* p = static_cast<const u8*>(src);
    out.insert(out.end(), p, p + n);
}

// Read `n` bytes into `dst`; advance `cursor`. Returns false on overflow.
PSY_FORCEINLINE bool blob_read(const u8* base, usize size, usize& cursor, void* dst, usize n) {
    if (cursor + n > size)
        return false;
    std::memcpy(dst, base + cursor, n);
    cursor += n;
    return true;
}

// ─── Encoders ─────────────────────────────────────────────────────────────
inline void encode_brush(std::vector<u8>& out, const brush::Brush& b) {
    blob_append(out, &b.id, sizeof(b.id));
    blob_append(out, &b.shape, sizeof(b.shape));
    u8 op = static_cast<u8>(b.op);
    blob_append(out, &op, sizeof(op));
    blob_append(out, &b.sides, sizeof(b.sides));
    u8 pad = 0;
    blob_append(out, &pad, sizeof(pad));
    blob_append(out, &b.origin, sizeof(b.origin));
    blob_append(out, &b.extents, sizeof(b.extents));
    blob_append(out, &b.grid_size, sizeof(b.grid_size));
}

inline bool decode_brush(const u8* base, usize size, usize& cursor, brush::Brush& out) {
    u8 op = 0, pad = 0;
    bool ok = true;
    ok &= blob_read(base, size, cursor, &out.id, sizeof(out.id));
    ok &= blob_read(base, size, cursor, &out.shape, sizeof(out.shape));
    ok &= blob_read(base, size, cursor, &op, sizeof(op));
    ok &= blob_read(base, size, cursor, &out.sides, sizeof(out.sides));
    ok &= blob_read(base, size, cursor, &pad, sizeof(pad));
    ok &= blob_read(base, size, cursor, &out.origin, sizeof(out.origin));
    ok &= blob_read(base, size, cursor, &out.extents, sizeof(out.extents));
    ok &= blob_read(base, size, cursor, &out.grid_size, sizeof(out.grid_size));
    out.op = static_cast<brush::Op>(op);
    return ok;
}

inline void encode_entity(std::vector<u8>& out, const detail::EntityRec& e) {
    blob_append(out, &e.id, sizeof(e.id));
    blob_append(out, &e.prefab_id, sizeof(e.prefab_id));
    blob_append(out, &e.position, sizeof(e.position));
    blob_append(out, &e.rotation, sizeof(e.rotation));
    blob_append(out, &e.scale, sizeof(e.scale));
    u8 alive = e.alive ? 1 : 0;
    blob_append(out, &alive, sizeof(alive));
    u8 pad[3]{};
    blob_append(out, &pad, sizeof(pad));
}

inline bool decode_entity(const u8* base, usize size, usize& cursor, detail::EntityRec& out) {
    u8 alive = 0, pad[3]{};
    bool ok = true;
    ok &= blob_read(base, size, cursor, &out.id, sizeof(out.id));
    ok &= blob_read(base, size, cursor, &out.prefab_id, sizeof(out.prefab_id));
    ok &= blob_read(base, size, cursor, &out.position, sizeof(out.position));
    ok &= blob_read(base, size, cursor, &out.rotation, sizeof(out.rotation));
    ok &= blob_read(base, size, cursor, &out.scale, sizeof(out.scale));
    ok &= blob_read(base, size, cursor, &alive, sizeof(alive));
    ok &= blob_read(base, size, cursor, &pad, sizeof(pad));
    out.alive = (alive != 0);
    return ok;
}

inline void encode_body(std::vector<u8>& out, const detail::BodyRec& b) {
    blob_append(out, &b.id, sizeof(b.id));
    blob_append(out, &b.position, sizeof(b.position));
    blob_append(out, &b.rotation, sizeof(b.rotation));
    blob_append(out, &b.scale, sizeof(b.scale));
    u8 frozen = b.frozen ? 1 : 0;
    u8 alive = b.alive ? 1 : 0;
    blob_append(out, &frozen, sizeof(frozen));
    blob_append(out, &alive, sizeof(alive));
    u8 pad[2]{};
    blob_append(out, &pad, sizeof(pad));
}

inline bool decode_body(const u8* base, usize size, usize& cursor, detail::BodyRec& out) {
    u8 frozen = 0, alive = 0, pad[2]{};
    bool ok = true;
    ok &= blob_read(base, size, cursor, &out.id, sizeof(out.id));
    ok &= blob_read(base, size, cursor, &out.position, sizeof(out.position));
    ok &= blob_read(base, size, cursor, &out.rotation, sizeof(out.rotation));
    ok &= blob_read(base, size, cursor, &out.scale, sizeof(out.scale));
    ok &= blob_read(base, size, cursor, &frozen, sizeof(frozen));
    ok &= blob_read(base, size, cursor, &alive, sizeof(alive));
    ok &= blob_read(base, size, cursor, &pad, sizeof(pad));
    out.frozen = (frozen != 0);
    out.alive = (alive != 0);
    return ok;
}

inline void encode_constraint(std::vector<u8>& out, const constraints::Constraint& c) {
    blob_append(out, &c.id, sizeof(c.id));
    u8 k = static_cast<u8>(c.kind);
    blob_append(out, &k, sizeof(k));
    u8 pad[3]{};
    blob_append(out, &pad, sizeof(pad));
    blob_append(out, &c.body_a, sizeof(c.body_a));
    blob_append(out, &c.body_b, sizeof(c.body_b));
    blob_append(out, &c.anchor_a, sizeof(c.anchor_a));
    blob_append(out, &c.anchor_b, sizeof(c.anchor_b));
    blob_append(out, &c.axis, sizeof(c.axis));
    blob_append(out, &c.rest_length, sizeof(c.rest_length));
    blob_append(out, &c.stiffness, sizeof(c.stiffness));
    blob_append(out, &c.damping, sizeof(c.damping));
    blob_append(out, &c.min_limit, sizeof(c.min_limit));
    blob_append(out, &c.max_limit, sizeof(c.max_limit));
}

inline bool decode_constraint(const u8* base, usize size, usize& cursor, constraints::Constraint& out) {
    u8 kind = 0, pad[3]{};
    bool ok = true;
    ok &= blob_read(base, size, cursor, &out.id, sizeof(out.id));
    ok &= blob_read(base, size, cursor, &kind, sizeof(kind));
    ok &= blob_read(base, size, cursor, &pad, sizeof(pad));
    ok &= blob_read(base, size, cursor, &out.body_a, sizeof(out.body_a));
    ok &= blob_read(base, size, cursor, &out.body_b, sizeof(out.body_b));
    ok &= blob_read(base, size, cursor, &out.anchor_a, sizeof(out.anchor_a));
    ok &= blob_read(base, size, cursor, &out.anchor_b, sizeof(out.anchor_b));
    ok &= blob_read(base, size, cursor, &out.axis, sizeof(out.axis));
    ok &= blob_read(base, size, cursor, &out.rest_length, sizeof(out.rest_length));
    ok &= blob_read(base, size, cursor, &out.stiffness, sizeof(out.stiffness));
    ok &= blob_read(base, size, cursor, &out.damping, sizeof(out.damping));
    ok &= blob_read(base, size, cursor, &out.min_limit, sizeof(out.min_limit));
    ok &= blob_read(base, size, cursor, &out.max_limit, sizeof(out.max_limit));
    out.kind = static_cast<constraints::Kind>(kind);
    return ok;
}

// ─── Full-state encode / decode ───────────────────────────────────────────
// `with_terrain` = true for .psylevel, false for .psyc (contraption only).
inline void encode_state(std::vector<u8>& out, const detail::State& s, bool with_terrain) {
    u32 n = 0;

    n = static_cast<u32>(s.brushes.size());
    blob_append(out, &n, sizeof(n));
    for (const auto& b : s.brushes)
        encode_brush(out, b);

    n = static_cast<u32>(s.entities.size());
    blob_append(out, &n, sizeof(n));
    for (const auto& e : s.entities)
        encode_entity(out, e);

    n = static_cast<u32>(s.bodies.size());
    blob_append(out, &n, sizeof(n));
    for (const auto& b : s.bodies)
        encode_body(out, b);

    n = static_cast<u32>(s.constraint_graph.size());
    blob_append(out, &n, sizeof(n));
    for (usize i = 0; i < s.constraint_graph.size(); ++i) {
        encode_constraint(out, s.constraint_graph.at(i));
    }

    if (with_terrain) {
        blob_append(out, &s.heightfield.size_x, sizeof(s.heightfield.size_x));
        blob_append(out, &s.heightfield.size_z, sizeof(s.heightfield.size_z));
        blob_append(out, &s.heightfield.spacing, sizeof(s.heightfield.spacing));
        blob_append(out, &s.heightfield.origin, sizeof(s.heightfield.origin));
        if (!s.heightfield.heights.empty()) {
            blob_append(out, s.heightfield.heights.data(), s.heightfield.heights.size() * sizeof(f32));
        }
        blob_append(out, &s.splat.size_x, sizeof(s.splat.size_x));
        blob_append(out, &s.splat.size_z, sizeof(s.splat.size_z));
        if (!s.splat.weights.empty()) {
            blob_append(out,
                        s.splat.weights.data(),
                        s.splat.weights.size() * sizeof(std::array<f32, 4>));
        }
    }
}

inline bool decode_state(const u8* base, usize size, detail::State& s, bool with_terrain) {
    usize cursor = 0;
    u32 n = 0;
    bool ok = true;

    ok &= blob_read(base, size, cursor, &n, sizeof(n));
    s.brushes.resize(n);
    for (u32 i = 0; i < n; ++i)
        ok &= decode_brush(base, size, cursor, s.brushes[i]);

    ok &= blob_read(base, size, cursor, &n, sizeof(n));
    s.entities.resize(n);
    for (u32 i = 0; i < n; ++i)
        ok &= decode_entity(base, size, cursor, s.entities[i]);

    ok &= blob_read(base, size, cursor, &n, sizeof(n));
    s.bodies.resize(n);
    for (u32 i = 0; i < n; ++i)
        ok &= decode_body(base, size, cursor, s.bodies[i]);

    ok &= blob_read(base, size, cursor, &n, sizeof(n));
    s.constraint_graph.clear();
    for (u32 i = 0; i < n; ++i) {
        constraints::Constraint c;
        ok &= decode_constraint(base, size, cursor, c);
        s.constraint_graph.mutable_list().push_back(c);
    }

    if (with_terrain) {
        ok &= blob_read(base, size, cursor, &s.heightfield.size_x, sizeof(s.heightfield.size_x));
        ok &= blob_read(base, size, cursor, &s.heightfield.size_z, sizeof(s.heightfield.size_z));
        ok &= blob_read(base, size, cursor, &s.heightfield.spacing, sizeof(s.heightfield.spacing));
        ok &= blob_read(base, size, cursor, &s.heightfield.origin, sizeof(s.heightfield.origin));
        const usize hcount =
            static_cast<usize>(s.heightfield.size_x) * static_cast<usize>(s.heightfield.size_z);
        s.heightfield.heights.assign(hcount, 0.0f);
        if (hcount) {
            ok &= blob_read(base, size, cursor, s.heightfield.heights.data(), hcount * sizeof(f32));
        }
        ok &= blob_read(base, size, cursor, &s.splat.size_x, sizeof(s.splat.size_x));
        ok &= blob_read(base, size, cursor, &s.splat.size_z, sizeof(s.splat.size_z));
        const usize scount = static_cast<usize>(s.splat.size_x) * static_cast<usize>(s.splat.size_z);
        s.splat.weights.assign(scount, std::array<f32, 4>{1.0f, 0.0f, 0.0f, 0.0f});
        if (scount) {
            ok &=
                blob_read(base, size, cursor, s.splat.weights.data(), scount * sizeof(std::array<f32, 4>));
        }
    }
    return ok;
}

}  // namespace psynder::editor::serial
