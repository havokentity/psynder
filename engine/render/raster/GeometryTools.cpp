// SPDX-License-Identifier: MIT
// Psynder — fixed-storage basic and playful geometry descriptors.

#include "render/GeometryTools.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace psynder::render::geometry_tools {

namespace {

constexpr u32 kWhite = 0xFFFFFFFFu;
constexpr u32 kConeSegments = 24;
constexpr u32 kSphereSlices = 24;
constexpr u32 kSphereStacks = 12;
constexpr u32 kCarpetLevel = 2;
constexpr u32 kCarpetTiles = 64;  // 8^level
constexpr u32 kTerrainCells = 32;
constexpr u32 kTerrainSide = kTerrainCells + 1u;

Vertex make_vertex(math::Vec3 p, math::Vec3 n, math::Vec2 uv, u32 color = kWhite) noexcept {
    return Vertex{p, n, uv, {0, 0}, color};
}

math::Vec2 spherical_uv(math::Vec3 p) noexcept {
    const f32 u = 0.5f + std::atan2(p.z, p.x) / math::kTwoPi;
    const f32 v = 0.5f - std::asin(std::clamp(p.y, -1.0f, 1.0f)) / math::kPi;
    return {u, v};
}

MeshDesc describe(const Vertex* vertices,
                  u32 vertex_count,
                  const u32* indices,
                  u32 index_count,
                  math::Aabb bounds,
                  const TextureAsset* base_color_asset) noexcept {
    MeshDesc desc{};
    desc.vertices = vertices;
    desc.vertex_count = vertex_count;
    desc.indices = indices;
    desc.index_count = index_count;
    desc.base_color_asset = base_color_asset;
    desc.local_bounds = bounds;
    return desc;
}

void emit_triangle(std::array<Vertex, 240>& vertices,
                   std::array<u32, 240>& indices,
                   u32& cursor,
                   math::Vec3 a,
                   math::Vec3 b,
                   math::Vec3 c) noexcept {
    const math::Vec3 n = math::normalize(math::cross(math::sub(b, a), math::sub(c, a)));
    vertices[cursor + 0u] = make_vertex(a, n, spherical_uv(math::normalize(a)));
    vertices[cursor + 1u] = make_vertex(b, n, spherical_uv(math::normalize(b)));
    vertices[cursor + 2u] = make_vertex(c, n, spherical_uv(math::normalize(c)));
    indices[cursor + 0u] = cursor + 0u;
    indices[cursor + 1u] = cursor + 1u;
    indices[cursor + 2u] = cursor + 2u;
    cursor += 3u;
}

void emit_subdivided_icosa_face(std::array<Vertex, 240>& vertices,
                                std::array<u32, 240>& indices,
                                u32& cursor,
                                math::Vec3 a,
                                math::Vec3 b,
                                math::Vec3 c) noexcept {
    const math::Vec3 ab = math::normalize(math::mul(math::add(a, b), 0.5f));
    const math::Vec3 bc = math::normalize(math::mul(math::add(b, c), 0.5f));
    const math::Vec3 ca = math::normalize(math::mul(math::add(c, a), 0.5f));
    emit_triangle(vertices, indices, cursor, a, ab, ca);
    emit_triangle(vertices, indices, cursor, ab, b, bc);
    emit_triangle(vertices, indices, cursor, ca, bc, c);
    emit_triangle(vertices, indices, cursor, ab, bc, ca);
}

struct SphereStorage {
    std::array<Vertex, (kSphereStacks + 1u) * (kSphereSlices + 1u)> vertices{};
    std::array<u32, kSphereStacks * kSphereSlices * 6u> indices{};
};

SphereStorage make_uv_sphere_storage() noexcept {
    SphereStorage out{};
    u32 vertex_cursor = 0;
    for (u32 stack = 0; stack <= kSphereStacks; ++stack) {
        const f32 v = static_cast<f32>(stack) / static_cast<f32>(kSphereStacks);
        const f32 phi = v * math::kPi;
        const f32 y = std::cos(phi);
        const f32 ring = std::sin(phi);
        for (u32 slice = 0; slice <= kSphereSlices; ++slice) {
            const f32 u = static_cast<f32>(slice) / static_cast<f32>(kSphereSlices);
            const f32 theta = u * math::kTwoPi;
            const math::Vec3 n{ring * std::cos(theta), y, ring * std::sin(theta)};
            out.vertices[vertex_cursor++] = make_vertex(n, n, {u, v});
        }
    }

    u32 index_cursor = 0;
    const u32 stride = kSphereSlices + 1u;
    for (u32 stack = 0; stack < kSphereStacks; ++stack) {
        for (u32 slice = 0; slice < kSphereSlices; ++slice) {
            const u32 a = stack * stride + slice;
            const u32 b = a + 1u;
            const u32 c = a + stride;
            const u32 d = c + 1u;
            out.indices[index_cursor++] = a;
            out.indices[index_cursor++] = c;
            out.indices[index_cursor++] = b;
            out.indices[index_cursor++] = b;
            out.indices[index_cursor++] = c;
            out.indices[index_cursor++] = d;
        }
    }
    return out;
}

struct GeodesicStorage {
    std::array<Vertex, 240> vertices{};
    std::array<u32, 240> indices{};
};

GeodesicStorage make_geodesic_storage() noexcept {
    GeodesicStorage out{};
    constexpr f32 t = 1.61803398875f;
    const math::Vec3 v[] = {
        math::normalize({-1, t, 0}),  math::normalize({1, t, 0}),
        math::normalize({-1, -t, 0}), math::normalize({1, -t, 0}),
        math::normalize({0, -1, t}),  math::normalize({0, 1, t}),
        math::normalize({0, -1, -t}), math::normalize({0, 1, -t}),
        math::normalize({t, 0, -1}),  math::normalize({t, 0, 1}),
        math::normalize({-t, 0, -1}), math::normalize({-t, 0, 1}),
    };
    constexpr u32 faces[] = {
        0, 11, 5,  0, 5, 1,   0, 1, 7,   0, 7, 10,  0, 10, 11,
        1, 5, 9,   5, 11, 4,  11, 10, 2, 10, 7, 6,  7, 1, 8,
        3, 9, 4,   3, 4, 2,   3, 2, 6,   3, 6, 8,   3, 8, 9,
        4, 9, 5,   2, 4, 11,  6, 2, 10,  8, 6, 7,   9, 8, 1,
    };

    u32 cursor = 0;
    for (u32 i = 0; i < static_cast<u32>(sizeof(faces) / sizeof(faces[0])); i += 3u)
        emit_subdivided_icosa_face(out.vertices, out.indices, cursor, v[faces[i]], v[faces[i + 1u]], v[faces[i + 2u]]);
    return out;
}

struct CarpetStorage {
    std::array<Vertex, kCarpetTiles * 4u> vertices{};
    std::array<u32, kCarpetTiles * 6u> indices{};
};

struct TerrainStorage {
    std::array<Vertex, kTerrainSide * kTerrainSide> vertices{};
    std::array<u32, kTerrainCells * kTerrainCells * 6u> indices{};
};

enum class TerrainKind : u8 {
    Island,
    Mountainous,
    Desert,
    RollingHills,
    Canyon,
    Volcanic,
    Arctic,
};

TerrainKind to_kind(TerrainPreset preset) noexcept {
    switch (preset) {
        case TerrainPreset::Island:
            return TerrainKind::Island;
        case TerrainPreset::Mountainous:
            return TerrainKind::Mountainous;
        case TerrainPreset::Desert:
            return TerrainKind::Desert;
        case TerrainPreset::RollingHills:
            return TerrainKind::RollingHills;
        case TerrainPreset::Canyon:
            return TerrainKind::Canyon;
        case TerrainPreset::Volcanic:
            return TerrainKind::Volcanic;
        case TerrainPreset::Arctic:
            return TerrainKind::Arctic;
    }
    return TerrainKind::Island;
}

void emit_carpet_tile(CarpetStorage& out, u32& tile, f32 x, f32 y, f32 s) noexcept {
    const f32 z = 0.0f;
    const f32 x0 = x;
    const f32 y0 = y;
    const f32 x1 = x + s;
    const f32 y1 = y + s;
    const math::Vec3 n{0, 0, 1};
    const u32 v = tile * 4u;
    out.vertices[v + 0u] = make_vertex({x0, y0, z}, n, {(x0 + 1.0f) * 0.5f, 1.0f - (y0 + 1.0f) * 0.5f});
    out.vertices[v + 1u] = make_vertex({x1, y0, z}, n, {(x1 + 1.0f) * 0.5f, 1.0f - (y0 + 1.0f) * 0.5f});
    out.vertices[v + 2u] = make_vertex({x1, y1, z}, n, {(x1 + 1.0f) * 0.5f, 1.0f - (y1 + 1.0f) * 0.5f});
    out.vertices[v + 3u] = make_vertex({x0, y1, z}, n, {(x0 + 1.0f) * 0.5f, 1.0f - (y1 + 1.0f) * 0.5f});
    const u32 i = tile * 6u;
    out.indices[i + 0u] = v + 0u;
    out.indices[i + 1u] = v + 3u;
    out.indices[i + 2u] = v + 1u;
    out.indices[i + 3u] = v + 1u;
    out.indices[i + 4u] = v + 3u;
    out.indices[i + 5u] = v + 2u;
    ++tile;
}

void emit_carpet(CarpetStorage& out, u32& tile, u32 level, f32 x, f32 y, f32 s) noexcept {
    if (level == 0u) {
        emit_carpet_tile(out, tile, x, y, s);
        return;
    }
    const f32 child = s / 3.0f;
    for (u32 yy = 0; yy < 3u; ++yy) {
        for (u32 xx = 0; xx < 3u; ++xx) {
            if (xx == 1u && yy == 1u)
                continue;
            emit_carpet(out, tile, level - 1u, x + child * static_cast<f32>(xx), y + child * static_cast<f32>(yy), child);
        }
    }
}

CarpetStorage make_carpet_storage() noexcept {
    CarpetStorage out{};
    u32 tile = 0;
    emit_carpet(out, tile, kCarpetLevel, -1.0f, -1.0f, 2.0f);
    return out;
}

f32 terrain_height(TerrainKind kind, f32 x, f32 z) noexcept {
    switch (kind) {
        case TerrainKind::Island: {
            const f32 r2 = x * x + z * z;
            const f32 falloff = std::clamp(1.0f - r2 * 0.72f, 0.0f, 1.0f);
            const f32 hills = 0.18f * std::sin(7.0f * x + 1.3f) * std::cos(6.0f * z);
            return falloff * (0.16f + hills + 0.34f * std::exp(-3.0f * r2)) - 0.18f;
        }
        case TerrainKind::Mountainous: {
            const f32 ridges = std::abs(std::sin(8.0f * x + 2.0f * std::cos(3.0f * z)));
            const f32 peaks = 0.50f * ridges + 0.18f * std::sin(13.0f * z + 1.7f);
            return peaks + 0.20f * std::exp(-5.0f * ((x - 0.2f) * (x - 0.2f) + (z + 0.25f) * (z + 0.25f)));
        }
        case TerrainKind::Desert: {
            const f32 dunes = 0.20f * std::sin(7.5f * x + 2.2f * std::sin(2.5f * z));
            const f32 ripples = 0.035f * std::sin(35.0f * (x * 0.65f + z * 0.35f));
            return dunes + ripples - 0.10f;
        }
        case TerrainKind::RollingHills: {
            const f32 broad = 0.22f * std::sin(2.8f * x + 0.5f) * std::cos(2.2f * z);
            const f32 folds = 0.08f * std::sin(5.0f * (x + z));
            return broad + folds + 0.02f * z;
        }
        case TerrainKind::Canyon: {
            const f32 river = x + 0.20f * std::sin(2.8f * z);
            const f32 cut = std::exp(-20.0f * river * river);
            const f32 shelves = 0.08f * std::floor(4.0f * std::abs(river)) / 4.0f;
            return 0.18f + shelves - 0.60f * cut + 0.04f * std::sin(10.0f * z);
        }
        case TerrainKind::Volcanic: {
            const f32 r = std::sqrt(x * x + z * z);
            const f32 cone = std::max(0.0f, 0.72f - r * 0.58f);
            const f32 crater = 0.52f * std::exp(-28.0f * (r - 0.34f) * (r - 0.34f));
            const f32 lava = 0.05f * std::sin(18.0f * std::atan2(z, x) + 7.0f * r);
            return cone - crater + lava - 0.18f;
        }
        case TerrainKind::Arctic: {
            const f32 shelf = 0.10f * std::sin(2.0f * x) + 0.10f * std::cos(2.6f * z);
            const f32 drift = 0.05f * std::sin(12.0f * (x * 0.4f - z * 0.7f));
            const f32 ridge = 0.24f * std::exp(-10.0f * (z + 0.18f) * (z + 0.18f));
            return shelf + drift + ridge - 0.05f;
        }
    }
    return 0.0f;
}

u32 terrain_color(TerrainKind kind, f32 y) noexcept {
    switch (kind) {
        case TerrainKind::Island:
            return y < -0.03f ? 0xFF6CB7D8u : (y > 0.22f ? 0xFFE8E0B0u : 0xFF5EAD58u);
        case TerrainKind::Mountainous:
            return y > 0.45f ? 0xFFE8EDF0u : (y > 0.25f ? 0xFF8B8174u : 0xFF4E7D4Bu);
        case TerrainKind::Desert:
            return y > 0.08f ? 0xFFE9C46Au : 0xFFDDAA5Fu;
        case TerrainKind::RollingHills:
            return y > 0.18f ? 0xFFB7D88Du : 0xFF6FA85Fu;
        case TerrainKind::Canyon:
            return y < -0.12f ? 0xFF5A87A0u : (y > 0.22f ? 0xFFC48755u : 0xFF9C5F3Du);
        case TerrainKind::Volcanic:
            return y < -0.04f ? 0xFF2A2220u : (y > 0.22f ? 0xFF7A3730u : 0xFF3A3432u);
        case TerrainKind::Arctic:
            return y > 0.16f ? 0xFFFFFFFFu : (y > 0.02f ? 0xFFE4F1F7u : 0xFFA9CDE1u);
    }
    return kWhite;
}

TerrainStorage make_terrain_storage(TerrainKind kind) noexcept {
    TerrainStorage out{};
    constexpr f32 extent = 3.0f;
    constexpr f32 step = (extent * 2.0f) / static_cast<f32>(kTerrainCells);
    for (u32 zc = 0; zc < kTerrainSide; ++zc) {
        const f32 v = static_cast<f32>(zc) / static_cast<f32>(kTerrainCells);
        const f32 z = -extent + v * extent * 2.0f;
        for (u32 xc = 0; xc < kTerrainSide; ++xc) {
            const f32 u = static_cast<f32>(xc) / static_cast<f32>(kTerrainCells);
            const f32 x = -extent + u * extent * 2.0f;
            const f32 sx = x / extent;
            const f32 sz = z / extent;
            const f32 y = terrain_height(kind, sx, sz);
            const f32 hx0 = terrain_height(kind, (x - step) / extent, sz);
            const f32 hx1 = terrain_height(kind, (x + step) / extent, sz);
            const f32 hz0 = terrain_height(kind, sx, (z - step) / extent);
            const f32 hz1 = terrain_height(kind, sx, (z + step) / extent);
            const math::Vec3 normal = math::normalize({hx0 - hx1, 2.0f * step, hz0 - hz1});
            out.vertices[zc * kTerrainSide + xc] =
                make_vertex({x, y, z}, normal, {u, v}, terrain_color(kind, y));
        }
    }

    u32 cursor = 0;
    for (u32 zc = 0; zc < kTerrainCells; ++zc) {
        for (u32 xc = 0; xc < kTerrainCells; ++xc) {
            const u32 a = zc * kTerrainSide + xc;
            const u32 b = a + 1u;
            const u32 c = a + kTerrainSide;
            const u32 d = c + 1u;
            out.indices[cursor++] = a;
            out.indices[cursor++] = c;
            out.indices[cursor++] = b;
            out.indices[cursor++] = b;
            out.indices[cursor++] = c;
            out.indices[cursor++] = d;
        }
    }
    return out;
}

}  // namespace

MeshDesc textured_triangle(const TextureAsset* base_color_asset) noexcept {
    static constexpr Vertex kVertices[] = {
        {{-0.6f, -0.4f, 0.0f}, {0, 0, 1}, {0.0f, 1.0f}, {0, 0}, kWhite},
        {{0.6f, -0.4f, 0.0f}, {0, 0, 1}, {1.0f, 1.0f}, {0, 0}, kWhite},
        {{0.0f, 0.6f, 0.0f}, {0, 0, 1}, {0.5f, 0.0f}, {0, 0}, kWhite},
    };
    static constexpr u32 kIndices[] = {0, 2, 1};
    return describe(kVertices, 3u, kIndices, 3u, {{-0.6f, -0.4f, 0.0f}, {0.6f, 0.6f, 0.0f}}, base_color_asset);
}

MeshDesc unit_cube(const TextureAsset* base_color_asset) noexcept {
    static constexpr Vertex kVertices[] = {
        {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0, 1}, {0, 0}, kWhite},
        {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0, 0}, {0, 0}, kWhite},
        {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {1, 0}, {0, 0}, kWhite},
        {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {1, 1}, {0, 0}, kWhite},
        {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0, 1}, {0, 0}, kWhite},
        {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0, 0}, {0, 0}, kWhite},
        {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {1, 0}, {0, 0}, kWhite},
        {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {1, 1}, {0, 0}, kWhite},
        {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0, 1}, {0, 0}, kWhite},
        {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0, 0}, {0, 0}, kWhite},
        {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {1, 0}, {0, 0}, kWhite},
        {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {1, 1}, {0, 0}, kWhite},
        {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0, 1}, {0, 0}, kWhite},
        {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0, 0}, {0, 0}, kWhite},
        {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {1, 0}, {0, 0}, kWhite},
        {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {1, 1}, {0, 0}, kWhite},
        {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0, 1}, {0, 0}, kWhite},
        {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {1, 1}, {0, 0}, kWhite},
        {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {1, 0}, {0, 0}, kWhite},
        {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0, 0}, {0, 0}, kWhite},
        {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0, 1}, {0, 0}, kWhite},
        {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {1, 1}, {0, 0}, kWhite},
        {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {1, 0}, {0, 0}, kWhite},
        {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0, 0}, {0, 0}, kWhite},
    };
    static constexpr u32 kIndices[] = {
        0,  2,  1,  0,  3,  2,  4,  6,  5,  4,  7,  6,  8,  10, 9,  8,  11, 10,
        12, 14, 13, 12, 15, 14, 16, 18, 17, 16, 19, 18, 20, 22, 21, 20, 23, 22,
    };
    return describe(kVertices,
                    24u,
                    kIndices,
                    36u,
                    {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
                    base_color_asset);
}

MeshDesc pyramid(const TextureAsset* base_color_asset) noexcept {
    static constexpr Vertex kVertices[] = {
        {{-0.5f, 0.0f, -0.5f}, {0, -1, 0}, {0, 1}, {0, 0}, kWhite},
        {{0.5f, 0.0f, -0.5f}, {0, -1, 0}, {1, 1}, {0, 0}, kWhite},
        {{0.5f, 0.0f, 0.5f}, {0, -1, 0}, {1, 0}, {0, 0}, kWhite},
        {{-0.5f, 0.0f, 0.5f}, {0, -1, 0}, {0, 0}, {0, 0}, kWhite},
        {{-0.5f, 0.0f, -0.5f}, {-0.707f, 0.5f, -0.707f}, {0, 1}, {0, 0}, kWhite},
        {{0.5f, 0.0f, -0.5f}, {0.707f, 0.5f, -0.707f}, {1, 1}, {0, 0}, kWhite},
        {{0.0f, 0.9f, 0.0f}, {0, 1, 0}, {0.5f, 0}, {0, 0}, kWhite},
        {{0.5f, 0.0f, 0.5f}, {0.707f, 0.5f, 0.707f}, {0, 1}, {0, 0}, kWhite},
        {{-0.5f, 0.0f, 0.5f}, {-0.707f, 0.5f, 0.707f}, {1, 1}, {0, 0}, kWhite},
    };
    static constexpr u32 kIndices[] = {0, 3, 1, 1, 3, 2, 4, 6, 5, 5, 6, 7, 7, 6, 8, 8, 6, 4};
    return describe(kVertices, 9u, kIndices, 18u, {{-0.5f, 0.0f, -0.5f}, {0.5f, 0.9f, 0.5f}}, base_color_asset);
}

MeshDesc cone(const TextureAsset* base_color_asset) noexcept {
    struct Storage {
        std::array<Vertex, kConeSegments * 2u + 2u> vertices{};
        std::array<u32, kConeSegments * 6u> indices{};
    };
    static const Storage storage = [] {
        Storage out{};
        const math::Vec3 apex{0, 0.9f, 0};
        out.vertices[0] = make_vertex({0, 0, 0}, {0, -1, 0}, {0.5f, 0.5f});
        out.vertices[1] = make_vertex(apex, {0, 1, 0}, {0.5f, 0.0f});
        for (u32 i = 0; i < kConeSegments; ++i) {
            const f32 u = static_cast<f32>(i) / static_cast<f32>(kConeSegments);
            const f32 a = u * math::kTwoPi;
            const math::Vec3 p{0.5f * std::cos(a), 0.0f, 0.5f * std::sin(a)};
            out.vertices[2u + i] = make_vertex(p, {0, -1, 0}, {0.5f + p.x, 0.5f - p.z});
            out.vertices[2u + kConeSegments + i] =
                make_vertex(p, math::normalize({p.x, 0.35f, p.z}), {u, 1.0f});
        }
        u32 cursor = 0;
        for (u32 i = 0; i < kConeSegments; ++i) {
            const u32 n = (i + 1u) % kConeSegments;
            out.indices[cursor++] = 0u;
            out.indices[cursor++] = 2u + n;
            out.indices[cursor++] = 2u + i;
            out.indices[cursor++] = 1u;
            out.indices[cursor++] = 2u + kConeSegments + i;
            out.indices[cursor++] = 2u + kConeSegments + n;
        }
        return out;
    }();
    return describe(storage.vertices.data(),
                    static_cast<u32>(storage.vertices.size()),
                    storage.indices.data(),
                    static_cast<u32>(storage.indices.size()),
                    {{-0.5f, 0.0f, -0.5f}, {0.5f, 0.9f, 0.5f}},
                    base_color_asset);
}

MeshDesc uv_sphere(const TextureAsset* base_color_asset) noexcept {
    static const SphereStorage storage = make_uv_sphere_storage();
    return describe(storage.vertices.data(),
                    static_cast<u32>(storage.vertices.size()),
                    storage.indices.data(),
                    static_cast<u32>(storage.indices.size()),
                    {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
                    base_color_asset);
}

MeshDesc geodesic_sphere(const TextureAsset* base_color_asset) noexcept {
    static const GeodesicStorage storage = make_geodesic_storage();
    return describe(storage.vertices.data(), 240u, storage.indices.data(), 240u, {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}}, base_color_asset);
}

MeshDesc sierpinski_tetrahedron(const TextureAsset* base_color_asset) noexcept {
    static const GeodesicStorage storage = [] {
        GeodesicStorage out{};
        const math::Vec3 a{0.0f, 1.0f, 0.0f};
        const math::Vec3 b{-0.95f, -0.45f, 0.55f};
        const math::Vec3 c{0.95f, -0.45f, 0.55f};
        const math::Vec3 d{0.0f, -0.45f, -1.0f};
        u32 cursor = 0;
        const auto emit_tetra = [&](math::Vec3 p0, math::Vec3 p1, math::Vec3 p2, math::Vec3 p3) {
            emit_triangle(out.vertices, out.indices, cursor, p0, p2, p1);
            emit_triangle(out.vertices, out.indices, cursor, p0, p1, p3);
            emit_triangle(out.vertices, out.indices, cursor, p1, p2, p3);
            emit_triangle(out.vertices, out.indices, cursor, p2, p0, p3);
        };
        const math::Vec3 ab = math::mul(math::add(a, b), 0.5f);
        const math::Vec3 ac = math::mul(math::add(a, c), 0.5f);
        const math::Vec3 ad = math::mul(math::add(a, d), 0.5f);
        const math::Vec3 bc = math::mul(math::add(b, c), 0.5f);
        const math::Vec3 bd = math::mul(math::add(b, d), 0.5f);
        const math::Vec3 cd = math::mul(math::add(c, d), 0.5f);
        emit_tetra(a, ab, ac, ad);
        emit_tetra(ab, b, bc, bd);
        emit_tetra(ac, bc, c, cd);
        emit_tetra(ad, bd, cd, d);
        return out;
    }();
    return describe(storage.vertices.data(), 48u, storage.indices.data(), 48u, {{-0.95f, -0.45f, -1.0f}, {0.95f, 1.0f, 0.55f}}, base_color_asset);
}

MeshDesc sierpinski_carpet(const TextureAsset* base_color_asset) noexcept {
    static const CarpetStorage storage = make_carpet_storage();
    return describe(storage.vertices.data(),
                    static_cast<u32>(storage.vertices.size()),
                    storage.indices.data(),
                    static_cast<u32>(storage.indices.size()),
                    {{-1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}},
                    base_color_asset);
}

const char* terrain_preset_name(TerrainPreset preset) noexcept {
    switch (preset) {
        case TerrainPreset::Island:
            return "island";
        case TerrainPreset::Mountainous:
            return "mountainous";
        case TerrainPreset::Desert:
            return "desert";
        case TerrainPreset::RollingHills:
            return "rolling_hills";
        case TerrainPreset::Canyon:
            return "canyon";
        case TerrainPreset::Volcanic:
            return "volcanic";
        case TerrainPreset::Arctic:
            return "arctic";
    }
    return "island";
}

MeshDesc terrain(TerrainPreset preset, const TextureAsset* base_color_asset) noexcept {
    switch (to_kind(preset)) {
        case TerrainKind::Island:
            return island_terrain(base_color_asset);
        case TerrainKind::Mountainous:
            return mountainous_terrain(base_color_asset);
        case TerrainKind::Desert:
            return desert_terrain(base_color_asset);
        case TerrainKind::RollingHills:
            return rolling_hills_terrain(base_color_asset);
        case TerrainKind::Canyon:
            return canyon_terrain(base_color_asset);
        case TerrainKind::Volcanic:
            return volcanic_terrain(base_color_asset);
        case TerrainKind::Arctic:
            return arctic_terrain(base_color_asset);
    }
    return island_terrain(base_color_asset);
}

MeshDesc island_terrain(const TextureAsset* base_color_asset) noexcept {
    static const TerrainStorage storage = make_terrain_storage(TerrainKind::Island);
    return describe(storage.vertices.data(),
                    static_cast<u32>(storage.vertices.size()),
                    storage.indices.data(),
                    static_cast<u32>(storage.indices.size()),
                    {{-3.0f, -0.25f, -3.0f}, {3.0f, 0.55f, 3.0f}},
                    base_color_asset);
}

MeshDesc mountainous_terrain(const TextureAsset* base_color_asset) noexcept {
    static const TerrainStorage storage = make_terrain_storage(TerrainKind::Mountainous);
    return describe(storage.vertices.data(),
                    static_cast<u32>(storage.vertices.size()),
                    storage.indices.data(),
                    static_cast<u32>(storage.indices.size()),
                    {{-3.0f, -0.30f, -3.0f}, {3.0f, 0.90f, 3.0f}},
                    base_color_asset);
}

MeshDesc desert_terrain(const TextureAsset* base_color_asset) noexcept {
    static const TerrainStorage storage = make_terrain_storage(TerrainKind::Desert);
    return describe(storage.vertices.data(),
                    static_cast<u32>(storage.vertices.size()),
                    storage.indices.data(),
                    static_cast<u32>(storage.indices.size()),
                    {{-3.0f, -0.35f, -3.0f}, {3.0f, 0.25f, 3.0f}},
                    base_color_asset);
}

MeshDesc rolling_hills_terrain(const TextureAsset* base_color_asset) noexcept {
    static const TerrainStorage storage = make_terrain_storage(TerrainKind::RollingHills);
    return describe(storage.vertices.data(),
                    static_cast<u32>(storage.vertices.size()),
                    storage.indices.data(),
                    static_cast<u32>(storage.indices.size()),
                    {{-3.0f, -0.35f, -3.0f}, {3.0f, 0.35f, 3.0f}},
                    base_color_asset);
}

MeshDesc canyon_terrain(const TextureAsset* base_color_asset) noexcept {
    static const TerrainStorage storage = make_terrain_storage(TerrainKind::Canyon);
    return describe(storage.vertices.data(),
                    static_cast<u32>(storage.vertices.size()),
                    storage.indices.data(),
                    static_cast<u32>(storage.indices.size()),
                    {{-3.0f, -0.45f, -3.0f}, {3.0f, 0.40f, 3.0f}},
                    base_color_asset);
}

MeshDesc volcanic_terrain(const TextureAsset* base_color_asset) noexcept {
    static const TerrainStorage storage = make_terrain_storage(TerrainKind::Volcanic);
    return describe(storage.vertices.data(),
                    static_cast<u32>(storage.vertices.size()),
                    storage.indices.data(),
                    static_cast<u32>(storage.indices.size()),
                    {{-3.0f, -0.45f, -3.0f}, {3.0f, 0.55f, 3.0f}},
                    base_color_asset);
}

MeshDesc arctic_terrain(const TextureAsset* base_color_asset) noexcept {
    static const TerrainStorage storage = make_terrain_storage(TerrainKind::Arctic);
    return describe(storage.vertices.data(),
                    static_cast<u32>(storage.vertices.size()),
                    storage.indices.data(),
                    static_cast<u32>(storage.indices.size()),
                    {{-3.0f, -0.25f, -3.0f}, {3.0f, 0.35f, 3.0f}},
                    base_color_asset);
}

}  // namespace psynder::render::geometry_tools
