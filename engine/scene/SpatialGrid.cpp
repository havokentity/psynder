// SPDX-License-Identifier: MIT
// Psynder — hashed-grid backend (lane-06 internal, Wave B).
//
// Sparse 3D grid with chained buckets. Each AABB occupies one or more
// integer cells; on insert/update we add the slot to every overlapping
// cell. Cell coords hash into a fixed-size bucket table; collisions
// chain via a per-bucket vector. Radius queries iterate the integer
// cells touched by (center ± radius) and emit unique entities.
//
// Used for AI / audio nearest-neighbour and as a fallback broadphase at
// very high body counts (DESIGN.md §9.4).

#include "Spatial.h"
#include "Spatial_Internal.h"

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace psynder::scene::detail {

namespace {

PSY_CACHELINE_ALIGN GridState g_grid;

PSY_FORCEINLINE SpatialKey pack_key(u32 slot) noexcept {
    return SpatialKey{slot + 1u};
}
PSY_FORCEINLINE u32 unpack_slot(SpatialKey k) noexcept {
    return k.raw == 0 ? 0xFFFFFFFFu : (k.raw - 1u);
}

PSY_FORCEINLINE u32 hash_cell(i32 x, i32 y, i32 z) noexcept {
    // Murmur-style mix of three 32-bit ints into a bucket index. The
    // mask folds the result into the bucket-count range (power of two).
    const auto ux = static_cast<u32>(x);
    const auto uy = static_cast<u32>(y);
    const auto uz = static_cast<u32>(z);
    u32 h = ux * 0x9E3779B1u;
    h ^= (uy * 0x85EBCA77u);
    h += (uz * 0xC2B2AE3Du);
    h ^= (h >> 16);
    h *= 0x7FEB352Du;
    h ^= (h >> 15);
    return h & (kGridBucketCount - 1u);
}

void ensure_buckets() noexcept {
    if (g_grid.buckets.size() != kGridBucketCount) {
        g_grid.buckets.assign(kGridBucketCount, {});
    }
}

// Integer cell range covering an AABB.
struct CellRange {
    i32 x0, y0, z0;
    i32 x1, y1, z1;
};

CellRange cell_range(const math::Aabb& b, f32 cell_size) noexcept {
    const f32 inv = 1.0f / cell_size;
    CellRange r;
    r.x0 = static_cast<i32>(std::floor(b.min.x * inv));
    r.y0 = static_cast<i32>(std::floor(b.min.y * inv));
    r.z0 = static_cast<i32>(std::floor(b.min.z * inv));
    r.x1 = static_cast<i32>(std::floor(b.max.x * inv));
    r.y1 = static_cast<i32>(std::floor(b.max.y * inv));
    r.z1 = static_cast<i32>(std::floor(b.max.z * inv));
    return r;
}

void insert_slot_into_cells(u32 slot, const math::Aabb& b) noexcept {
    ensure_buckets();
    const CellRange r = cell_range(b, g_grid.cell_size);
    for (i32 cz = r.z0; cz <= r.z1; ++cz) {
        for (i32 cy = r.y0; cy <= r.y1; ++cy) {
            for (i32 cx = r.x0; cx <= r.x1; ++cx) {
                auto& bucket = g_grid.buckets[hash_cell(cx, cy, cz)];
                bucket.push_back(GridCellRef{cx, cy, cz, slot});
            }
        }
    }
}

void remove_slot_from_cells(u32 slot, const math::Aabb& b) noexcept {
    ensure_buckets();
    const CellRange r = cell_range(b, g_grid.cell_size);
    for (i32 cz = r.z0; cz <= r.z1; ++cz) {
        for (i32 cy = r.y0; cy <= r.y1; ++cy) {
            for (i32 cx = r.x0; cx <= r.x1; ++cx) {
                auto& bucket = g_grid.buckets[hash_cell(cx, cy, cz)];
                bucket.erase(std::remove_if(bucket.begin(),
                                            bucket.end(),
                                            [&](const GridCellRef& e) noexcept {
                                                return e.slot == slot && e.x == cx && e.y == cy &&
                                                       e.z == cz;
                                            }),
                             bucket.end());
            }
        }
    }
}

class GridBackend final : public ISpatialIndex {
   public:
    SpatialKey insert(u32 entity_index, const math::Aabb& bounds) override {
        u32 slot;
        if (!g_grid.free_slots.empty()) {
            slot = g_grid.free_slots.back();
            g_grid.free_slots.pop_back();
            g_grid.slots[slot] = GridSlot{GridEntry{entity_index, bounds}, true};
        } else {
            slot = static_cast<u32>(g_grid.slots.size());
            g_grid.slots.push_back(GridSlot{GridEntry{entity_index, bounds}, true});
        }
        insert_slot_into_cells(slot, bounds);
        return pack_key(slot);
    }

    void update(SpatialKey key, const math::Aabb& bounds) override {
        const u32 slot = unpack_slot(key);
        if (slot >= g_grid.slots.size() || !g_grid.slots[slot].alive)
            return;
        const auto& old = g_grid.slots[slot].entry.bounds;
        const CellRange r_old = cell_range(old, g_grid.cell_size);
        const CellRange r_new = cell_range(bounds, g_grid.cell_size);
        if (r_old.x0 != r_new.x0 || r_old.y0 != r_new.y0 || r_old.z0 != r_new.z0 ||
            r_old.x1 != r_new.x1 || r_old.y1 != r_new.y1 || r_old.z1 != r_new.z1) {
            remove_slot_from_cells(slot, old);
            g_grid.slots[slot].entry.bounds = bounds;
            insert_slot_into_cells(slot, bounds);
        } else {
            g_grid.slots[slot].entry.bounds = bounds;
        }
    }

    void remove(SpatialKey key) override {
        const u32 slot = unpack_slot(key);
        if (slot >= g_grid.slots.size() || !g_grid.slots[slot].alive)
            return;
        remove_slot_from_cells(slot, g_grid.slots[slot].entry.bounds);
        g_grid.slots[slot].alive = false;
        g_grid.free_slots.push_back(slot);
    }

    void query_aabb(const math::Aabb& q, std::span<u32> out_entities) const override {
        if (out_entities.empty() || g_grid.buckets.empty())
            return;
        const CellRange r = cell_range(q, g_grid.cell_size);
        usize written = 0;
        // Dedup tracker (entities can occupy multiple cells).
        for (i32 cz = r.z0; cz <= r.z1 && written < out_entities.size(); ++cz) {
            for (i32 cy = r.y0; cy <= r.y1 && written < out_entities.size(); ++cy) {
                for (i32 cx = r.x0; cx <= r.x1 && written < out_entities.size(); ++cx) {
                    const auto& bucket = g_grid.buckets[hash_cell(cx, cy, cz)];
                    for (const auto& e : bucket) {
                        if (e.x != cx || e.y != cy || e.z != cz)
                            continue;
                        const auto& slot = g_grid.slots[e.slot];
                        if (!slot.alive)
                            continue;
                        if (!aabb_overlap(slot.entry.bounds, q))
                            continue;
                        // Linear dedup — out_entities is small (caller-
                        // bounded), and the radius query also dedups in
                        // grid_radius_query(). This branch is for editor /
                        // debug uses where exact uniqueness isn't required.
                        bool dup = false;
                        for (usize k = 0; k < written; ++k) {
                            if (out_entities[k] == slot.entry.entity_index) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup)
                            out_entities[written++] = slot.entry.entity_index;
                        if (written >= out_entities.size())
                            return;
                    }
                }
            }
        }
    }
};

PSY_CACHELINE_ALIGN GridBackend g_grid_backend{};

}  // namespace

GridState& grid_state() noexcept {
    return g_grid;
}

ISpatialIndex* grid_backend() noexcept {
    return &g_grid_backend;
}

u32 grid_radius_query(math::Vec3 center, f32 radius, std::span<u32> out) noexcept {
    if (out.empty() || g_grid.buckets.empty())
        return 0;

    const math::Aabb q{
        {center.x - radius, center.y - radius, center.z - radius},
        {center.x + radius, center.y + radius, center.z + radius},
    };
    const CellRange r = cell_range(q, g_grid.cell_size);
    const f32 r2 = radius * radius;

    u32 written = 0;
    for (i32 cz = r.z0; cz <= r.z1; ++cz) {
        for (i32 cy = r.y0; cy <= r.y1; ++cy) {
            for (i32 cx = r.x0; cx <= r.x1; ++cx) {
                const auto& bucket = g_grid.buckets[hash_cell(cx, cy, cz)];
                for (const auto& e : bucket) {
                    if (e.x != cx || e.y != cy || e.z != cz)
                        continue;
                    const auto& slot = g_grid.slots[e.slot];
                    if (!slot.alive)
                        continue;
                    // Distance from sphere center to AABB.
                    const auto& b = slot.entry.bounds;
                    const f32 dx =
                        std::max(b.min.x - center.x, 0.0f) + std::max(center.x - b.max.x, 0.0f);
                    const f32 dy =
                        std::max(b.min.y - center.y, 0.0f) + std::max(center.y - b.max.y, 0.0f);
                    const f32 dz =
                        std::max(b.min.z - center.z, 0.0f) + std::max(center.z - b.max.z, 0.0f);
                    if (dx * dx + dy * dy + dz * dz > r2)
                        continue;

                    // Dedup before writing — entities may sit in many
                    // cells. The caller's `out` is the dedup target.
                    bool dup = false;
                    for (u32 k = 0; k < written; ++k) {
                        if (out[k] == slot.entry.entity_index) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        out[written++] = slot.entry.entity_index;
                        if (written >= out.size())
                            return written;
                    }
                }
            }
        }
    }
    return written;
}

}  // namespace psynder::scene::detail
