// SPDX-License-Identifier: MIT
// Psynder — BSP / PVS / portal indoor world. Lane 10 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "scene/World.h"

#include <span>
#include <string_view>
#include <vector>

namespace psynder::world::bsp {

struct BspNode {
    math::Vec3 plane_normal;
    f32 plane_d;
    i32 front_child;  // negative = leaf index
    i32 back_child;
};

struct BspLeaf {
    i32 cluster;  // PVS cluster id
    u32 first_face;
    u32 face_count;
    math::Aabb bounds;
};

struct BspFace {
    u32 first_vertex;
    u32 vertex_count;
    u32 material;
    u32 lightmap;
};

struct BspMap {
    std::vector<BspNode> nodes;
    std::vector<BspLeaf> leaves;
    std::vector<BspFace> faces;
    std::vector<u8> pvs;  // bit vectors per cluster
};

bool load(std::string_view virtual_path, BspMap& out);
BspLeaf locate(const BspMap& map, math::Vec3 point);
void walk_visible_leaves(const BspMap& map, math::Vec3 eye, void (*emit)(const BspLeaf&, void*), void* user);

}  // namespace psynder::world::bsp
