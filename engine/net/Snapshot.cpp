// SPDX-License-Identifier: MIT
// Psynder — snapshot encode / decode / compose / interpolate. Lane 14 internal.

#include "Snapshot.h"

#include <algorithm>
#include <cstring>

namespace psynder::net {

namespace {

PSY_FORCEINLINE void write_u32_le(u8* p, u32 v) noexcept {
    p[0] = u8(v       & 0xFFu);
    p[1] = u8((v >> 8)  & 0xFFu);
    p[2] = u8((v >> 16) & 0xFFu);
    p[3] = u8((v >> 24) & 0xFFu);
}
PSY_FORCEINLINE u32 read_u32_le(const u8* p) noexcept {
    return u32(p[0])
         | (u32(p[1]) << 8)
         | (u32(p[2]) << 16)
         | (u32(p[3]) << 24);
}

PSY_FORCEINLINE void write_f32_le(u8* p, f32 v) noexcept {
    u32 raw;
    std::memcpy(&raw, &v, sizeof(raw));
    write_u32_le(p, raw);
}
PSY_FORCEINLINE f32 read_f32_le(const u8* p) noexcept {
    u32 raw = read_u32_le(p);
    f32 v;
    std::memcpy(&v, &raw, sizeof(v));
    return v;
}

}  // namespace

usize encode_snapshot(const SnapshotFrame& f, std::span<u8> out) noexcept {
    const usize need = kSnapshotHeaderBytes + f.entities.size() * kSnapshotEntityBytes;
    if (out.size() < need) return 0;
    u8* p = out.data();
    write_u32_le(p + 0, f.tick);
    write_u32_le(p + 4, u32(f.entities.size()));
    p += kSnapshotHeaderBytes;
    for (const SnapshotEntity& e : f.entities) {
        write_u32_le(p + 0,  e.entity_id);
        write_f32_le(p + 4,  e.position.x);
        write_f32_le(p + 8,  e.position.y);
        write_f32_le(p + 12, e.position.z);
        write_u32_le(p + 16, e.state_bits);
        p += kSnapshotEntityBytes;
    }
    return need;
}

bool decode_snapshot(std::span<const u8> in, SnapshotFrame& out) noexcept {
    if (in.size() < kSnapshotHeaderBytes) return false;
    out.tick   = read_u32_le(in.data() + 0);
    u32 count  = read_u32_le(in.data() + 4);
    const usize need = kSnapshotHeaderBytes + usize(count) * kSnapshotEntityBytes;
    if (in.size() < need) return false;
    out.entities.clear();
    out.entities.reserve(count);
    const u8* p = in.data() + kSnapshotHeaderBytes;
    for (u32 i = 0; i < count; ++i) {
        SnapshotEntity e{};
        e.entity_id  = read_u32_le(p + 0);
        e.position.x = read_f32_le(p + 4);
        e.position.y = read_f32_le(p + 8);
        e.position.z = read_f32_le(p + 12);
        e.state_bits = read_u32_le(p + 16);
        out.entities.push_back(e);
        p += kSnapshotEntityBytes;
    }
    return true;
}

void compose_for_peer(const SnapshotFrame& world,
                      const AoiFilter&     aoi,
                      PeerId               p,
                      SnapshotFrame&       out_frame) noexcept {
    out_frame.tick = world.tick;
    out_frame.entities.clear();
    out_frame.entities.reserve(world.entities.size());
    for (const SnapshotEntity& e : world.entities) {
        if (aoi.visible(p, e.position)) {
            out_frame.entities.push_back(e);
        }
    }
}

void interpolate(const SnapshotFrame& a, const SnapshotFrame& b, f32 t,
                 SnapshotFrame& out) noexcept {
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    out.tick = b.tick;
    out.entities.clear();

    // Build a tiny lookup over `b` keyed by entity_id (snapshots are small).
    // For typical 16-32 player FPS the entity count per peer is in the low
    // hundreds at most — linear scan is fine.
    for (const SnapshotEntity& ea : a.entities) {
        auto it = std::find_if(b.entities.begin(), b.entities.end(),
                               [&](const SnapshotEntity& x) {
                                   return x.entity_id == ea.entity_id;
                               });
        if (it == b.entities.end()) {
            // Only in `a` — emit as-is (will fade out next snapshot).
            out.entities.push_back(ea);
            continue;
        }
        SnapshotEntity merged{};
        merged.entity_id = ea.entity_id;
        merged.position.x = ea.position.x + (it->position.x - ea.position.x) * t;
        merged.position.y = ea.position.y + (it->position.y - ea.position.y) * t;
        merged.position.z = ea.position.z + (it->position.z - ea.position.z) * t;
        // State bits aren't interpolated — they snap to the newer frame.
        merged.state_bits = (t >= 0.5f) ? it->state_bits : ea.state_bits;
        out.entities.push_back(merged);
    }
    // Entities only in `b` — append.
    for (const SnapshotEntity& eb : b.entities) {
        auto it = std::find_if(a.entities.begin(), a.entities.end(),
                               [&](const SnapshotEntity& x) {
                                   return x.entity_id == eb.entity_id;
                               });
        if (it == a.entities.end()) {
            out.entities.push_back(eb);
        }
    }
}

}  // namespace psynder::net
