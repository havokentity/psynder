// SPDX-License-Identifier: MIT
// Psynder — cache-coherent scene graph. Lane 06 owns.

#include "scene/SceneGraph.h"

#include "jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace psynder::scene {
namespace {

inline constexpr u32 kInvalidIndex = std::numeric_limits<u32>::max();

math::Vec3 transform_point(const math::Mat4& m, math::Vec3 p) noexcept {
    const math::Vec4 r = math::mul(m, math::Vec4{p.x, p.y, p.z, 1.0f});
    return {r.x, r.y, r.z};
}

f32 max_column_scale(const math::Mat4& m) noexcept {
    const f32 sx = std::sqrt(m.m[0] * m.m[0] + m.m[1] * m.m[1] + m.m[2] * m.m[2]);
    const f32 sy = std::sqrt(m.m[4] * m.m[4] + m.m[5] * m.m[5] + m.m[6] * m.m[6]);
    const f32 sz = std::sqrt(m.m[8] * m.m[8] + m.m[9] * m.m[9] + m.m[10] * m.m[10]);
    return std::max(sx, std::max(sy, sz));
}

}  // namespace

math::Mat4 local_transform_matrix(const LocalTransform& local) {
    const math::Quat q = math::quat_normalize(local.rotation);
    return math::mul(math::translate(local.translation),
                     math::mul(math::rotate_quat(q), math::scale(local.scale)));
}

SceneNode SceneGraph::make_handle(u32 index, u32 generation) noexcept {
    return SceneNode{((generation & 0xFFu) << 24) | ((index + 1u) & 0x00FFFFFFu)};
}

bool SceneGraph::valid_index(SceneNode node) const noexcept {
    if (!node.valid())
        return false;
    const u32 index = node.index();
    return index < generation_.size() && alive_[index] != 0u && generation_[index] == node.gen();
}

void SceneGraph::clear() {
    generation_.clear();
    alive_.clear();
    parent_.clear();
    first_child_.clear();
    next_sibling_.clear();
    prev_sibling_.clear();
    depth_.clear();
    local_.clear();
    world_.clear();
    local_dirty_.clear();
    effective_dirty_.clear();
    nodes_by_depth_.clear();
    analytic_spheres_.clear();
    live_nodes_ = 0;
}

SceneNode SceneGraph::create_node(SceneNode parent, const LocalTransform& local) {
    const u32 parent_index = valid_index(parent) ? parent.index() : kInvalidIndex;
    const u32 index = static_cast<u32>(generation_.size());
    const u32 generation = 1u;
    generation_.push_back(generation);
    alive_.push_back(1u);
    parent_.push_back(parent_index);
    first_child_.push_back(kInvalidIndex);
    next_sibling_.push_back(kInvalidIndex);
    prev_sibling_.push_back(kInvalidIndex);
    depth_.push_back(parent_index == kInvalidIndex ? 0u : depth_[parent_index] + 1u);
    local_.push_back(local_transform_matrix(local));
    world_.push_back(local_[index]);
    local_dirty_.push_back(1u);
    effective_dirty_.push_back(1u);
    if (nodes_by_depth_.size() <= depth_[index])
        nodes_by_depth_.resize(static_cast<usize>(depth_[index]) + 1u);
    nodes_by_depth_[depth_[index]].push_back(index);
    if (parent_index != kInvalidIndex)
        attach_child(parent_index, index);
    ++live_nodes_;
    return make_handle(index, generation);
}

bool SceneGraph::destroy_node(SceneNode node) {
    if (!valid_index(node))
        return false;

    const u32 index = node.index();
    for (u32 child = first_child_[index]; child != kInvalidIndex;) {
        const u32 next = next_sibling_[child];
        destroy_node(make_handle(child, generation_[child]));
        child = next;
    }
    detach_child(index);
    alive_[index] = 0u;
    generation_[index] = (generation_[index] + 1u) & 0xFFu;
    if (generation_[index] == 0u)
        generation_[index] = 1u;
    first_child_[index] = kInvalidIndex;
    parent_[index] = kInvalidIndex;
    local_dirty_[index] = 0u;
    effective_dirty_[index] = 0u;
    --live_nodes_;
    rebuild_depth_lists();
    return true;
}

bool SceneGraph::alive(SceneNode node) const noexcept {
    return valid_index(node);
}

bool SceneGraph::set_parent(SceneNode node, SceneNode parent) {
    if (!valid_index(node))
        return false;
    const u32 index = node.index();
    const u32 parent_index = valid_index(parent) ? parent.index() : kInvalidIndex;
    if (parent_index == index)
        return false;
    if (parent_index != kInvalidIndex && parent_index > index)
        return false;

    for (u32 p = parent_index; p != kInvalidIndex; p = parent_[p]) {
        if (p == index)
            return false;
    }

    detach_child(index);
    parent_[index] = parent_index;
    if (parent_index != kInvalidIndex)
        attach_child(parent_index, index);
    update_subtree_depths(index, parent_index == kInvalidIndex ? 0u : depth_[parent_index] + 1u);
    rebuild_depth_lists();
    mark_dirty(node);
    return true;
}

SceneNode SceneGraph::parent(SceneNode node) const noexcept {
    if (!valid_index(node))
        return kInvalidSceneNode;
    const u32 p = parent_[node.index()];
    return p == kInvalidIndex ? kInvalidSceneNode : make_handle(p, generation_[p]);
}

void SceneGraph::set_local_transform(SceneNode node, const LocalTransform& local) {
    set_local_matrix(node, local_transform_matrix(local));
}

void SceneGraph::set_local_matrix(SceneNode node, const math::Mat4& local) {
    if (!valid_index(node))
        return;
    const u32 index = node.index();
    local_[index] = local;
    mark_dirty(node);
}

const math::Mat4& SceneGraph::local_matrix(SceneNode node) const noexcept {
    static const math::Mat4 kIdentity = math::identity4();
    return valid_index(node) ? local_[node.index()] : kIdentity;
}

const math::Mat4& SceneGraph::world_matrix(SceneNode node) const noexcept {
    static const math::Mat4 kIdentity = math::identity4();
    return valid_index(node) ? world_[node.index()] : kIdentity;
}

void SceneGraph::mark_dirty(SceneNode node) {
    if (valid_index(node))
        local_dirty_[node.index()] = 1u;
}

SceneGraphUpdateStats SceneGraph::update_world_transforms(u32 parallel_threshold) {
    SceneGraphUpdateStats stats{};
    stats.depth_levels = static_cast<u32>(nodes_by_depth_.size());
    std::atomic<u32> updated{0u};

    auto update_one = [&](u32 index) {
        if (alive_[index] == 0u)
            return;
        const u32 p = parent_[index];
        const bool parent_dirty = p != kInvalidIndex && effective_dirty_[p] != 0u;
        const bool dirty = local_dirty_[index] != 0u || parent_dirty;
        effective_dirty_[index] = dirty ? 1u : 0u;
        if (dirty) {
            world_[index] = p == kInvalidIndex ? local_[index] : math::mul(world_[p], local_[index]);
            local_dirty_[index] = 0u;
            updated.fetch_add(1u, std::memory_order_relaxed);
        }
    };

    for (const std::vector<u32>& level : nodes_by_depth_) {
        stats.nodes_visited += static_cast<u32>(level.size());
        if (level.size() >= parallel_threshold) {
            if (jobs::JobSystem::Get().worker_count() == 0u)
                jobs::JobSystem::Get().start(0);
            jobs::JobSystem::Get().parallel_for(0, level.size(), 64, [&](usize begin, usize end) {
                for (usize i = begin; i < end; ++i)
                    update_one(level[i]);
            });
        } else {
            for (u32 index : level)
                update_one(index);
        }
    }

    stats.transforms_updated = updated.load(std::memory_order_relaxed);
    return stats;
}

u32 SceneGraph::add_analytic_sphere(const AnalyticSphereDesc& desc) {
    if (!valid_index(desc.node))
        return kInvalidIndex;
    analytic_spheres_.push_back(desc);
    return static_cast<u32>(analytic_spheres_.size() - 1u);
}

std::span<const AnalyticSphereDesc> SceneGraph::analytic_spheres() const noexcept {
    return analytic_spheres_;
}

void SceneGraph::gather_analytic_spheres(std::vector<AnalyticSphereInstance>& out) const {
    out.clear();
    out.reserve(analytic_spheres_.size());
    for (const AnalyticSphereDesc& sphere : analytic_spheres_) {
        if (!valid_index(sphere.node) || sphere.local_sphere.radius < 0.0f)
            continue;
        const math::Mat4& world = world_[sphere.node.index()];
        AnalyticSphereInstance inst{};
        inst.node = sphere.node;
        inst.world_sphere.center = transform_point(world, sphere.local_sphere.center);
        inst.world_sphere.radius = sphere.local_sphere.radius * max_column_scale(world);
        inst.material_rgba8 = sphere.material_rgba8;
        out.push_back(inst);
    }
}

u32 SceneGraph::node_count() const noexcept {
    return static_cast<u32>(generation_.size());
}

void SceneGraph::attach_child(u32 parent_index, u32 child_index) noexcept {
    prev_sibling_[child_index] = kInvalidIndex;
    next_sibling_[child_index] = first_child_[parent_index];
    if (first_child_[parent_index] != kInvalidIndex)
        prev_sibling_[first_child_[parent_index]] = child_index;
    first_child_[parent_index] = child_index;
}

void SceneGraph::detach_child(u32 child_index) noexcept {
    const u32 p = parent_[child_index];
    if (p == kInvalidIndex)
        return;
    const u32 prev = prev_sibling_[child_index];
    const u32 next = next_sibling_[child_index];
    if (prev != kInvalidIndex)
        next_sibling_[prev] = next;
    else
        first_child_[p] = next;
    if (next != kInvalidIndex)
        prev_sibling_[next] = prev;
    prev_sibling_[child_index] = kInvalidIndex;
    next_sibling_[child_index] = kInvalidIndex;
}

void SceneGraph::rebuild_depth_lists() {
    nodes_by_depth_.clear();
    for (u32 i = 0; i < alive_.size(); ++i) {
        if (alive_[i] == 0u)
            continue;
        if (nodes_by_depth_.size() <= depth_[i])
            nodes_by_depth_.resize(static_cast<usize>(depth_[i]) + 1u);
        nodes_by_depth_[depth_[i]].push_back(i);
    }
}

void SceneGraph::update_subtree_depths(u32 root_index, u32 depth) {
    depth_[root_index] = depth;
    for (u32 child = first_child_[root_index]; child != kInvalidIndex; child = next_sibling_[child])
        update_subtree_depths(child, depth + 1u);
}

}  // namespace psynder::scene
