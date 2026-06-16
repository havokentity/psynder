// SPDX-License-Identifier: MIT
// Psynder — cache-coherent scene graph. Lane 06 owns.

#include "scene/SceneGraph.h"

#include <algorithm>
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
    local_translation_.clear();
    local_rotation_.clear();
    local_scale_.clear();
    local_.clear();
    world_.clear();
    local_dirty_.clear();
    local_matrix_dirty_.clear();
    effective_dirty_.clear();
    dirty_queued_.clear();
    dirty_roots_.clear();
    free_nodes_.clear();
    analytic_spheres_.clear();
    live_nodes_ = 0;
    max_depth_ = 0;
}

void SceneGraph::reserve_nodes(u32 count) {
    generation_.reserve(count);
    alive_.reserve(count);
    parent_.reserve(count);
    first_child_.reserve(count);
    next_sibling_.reserve(count);
    prev_sibling_.reserve(count);
    depth_.reserve(count);
    local_translation_.reserve(count);
    local_rotation_.reserve(count);
    local_scale_.reserve(count);
    local_.reserve(count);
    world_.reserve(count);
    local_dirty_.reserve(count);
    local_matrix_dirty_.reserve(count);
    effective_dirty_.reserve(count);
    dirty_queued_.reserve(count);
    dirty_roots_.reserve(count);
    free_nodes_.reserve(count);
    analytic_spheres_.reserve(count);
}

void SceneGraph::reserve_analytic_spheres(u32 count) {
    analytic_spheres_.reserve(count);
}

SceneNode SceneGraph::create_node(SceneNode parent, const LocalTransform& local) {
    const u32 parent_index = valid_index(parent) ? parent.index() : kInvalidIndex;

    u32 index = kInvalidIndex;
    if (!free_nodes_.empty()) {
        auto it = free_nodes_.end();
        if (parent_index == kInvalidIndex) {
            it = free_nodes_.end() - 1;
        } else {
            it = std::find_if(free_nodes_.begin(), free_nodes_.end(), [&](u32 candidate) {
                return candidate > parent_index;
            });
        }
        if (it != free_nodes_.end()) {
            index = *it;
            *it = free_nodes_.back();
            free_nodes_.pop_back();
        }
    }

    if (index == kInvalidIndex) {
        index = static_cast<u32>(generation_.size());
        generation_.push_back(1u);
        alive_.push_back(0u);
        parent_.push_back(kInvalidIndex);
        first_child_.push_back(kInvalidIndex);
        next_sibling_.push_back(kInvalidIndex);
        prev_sibling_.push_back(kInvalidIndex);
        depth_.push_back(0u);
        local_translation_.push_back({});
        local_rotation_.push_back({});
        local_scale_.push_back({});
        local_.push_back(math::identity4());
        world_.push_back(math::identity4());
        local_dirty_.push_back(0u);
        local_matrix_dirty_.push_back(0u);
        effective_dirty_.push_back(0u);
        dirty_queued_.push_back(0u);
    }

    const u32 generation = generation_[index];
    alive_[index] = 1u;
    parent_[index] = parent_index;
    first_child_[index] = kInvalidIndex;
    next_sibling_[index] = kInvalidIndex;
    prev_sibling_[index] = kInvalidIndex;
    depth_[index] = parent_index == kInvalidIndex ? 0u : depth_[parent_index] + 1u;
    local_translation_[index] = local.translation;
    local_rotation_[index] = local.rotation;
    local_scale_[index] = local.scale;
    local_[index] = math::identity4();
    world_[index] = math::identity4();
    local_dirty_[index] = 1u;
    local_matrix_dirty_[index] = 1u;
    effective_dirty_[index] = 1u;
    dirty_queued_[index] = 1u;
    dirty_roots_.push_back(index);
    max_depth_ = std::max(max_depth_, depth_[index]);
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
    local_matrix_dirty_[index] = 0u;
    effective_dirty_[index] = 0u;
    if (index < dirty_queued_.size())
        dirty_queued_[index] = 0u;
    free_nodes_.push_back(index);
    --live_nodes_;
    recompute_depth_bounds();
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
    recompute_depth_bounds();
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
    if (!valid_index(node))
        return;
    const u32 index = node.index();
    local_translation_[index] = local.translation;
    local_rotation_[index] = local.rotation;
    local_scale_[index] = local.scale;
    local_matrix_dirty_[index] = 1u;
    mark_dirty(node);
}

void SceneGraph::set_local_matrix(SceneNode node, const math::Mat4& local) {
    if (!valid_index(node))
        return;
    const u32 index = node.index();
    local_[index] = local;
    local_matrix_dirty_[index] = 0u;
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
    if (!valid_index(node))
        return;
    const u32 index = node.index();
    local_dirty_[index] = 1u;
    if (index < dirty_queued_.size() && dirty_queued_[index] != 0u)
        return;
    if (dirty_queued_.size() <= index)
        dirty_queued_.resize(static_cast<usize>(index) + 1u, 0u);
    dirty_queued_[index] = 1u;
    dirty_roots_.push_back(index);
}

SceneGraphUpdateStats SceneGraph::update_world_transforms(u32 parallel_threshold) {
    (void)parallel_threshold;
    SceneGraphUpdateStats stats{};
    stats.depth_levels = live_nodes_ == 0u ? 0u : max_depth_ + 1u;

    auto has_dirty_ancestor = [&](u32 index) {
        for (u32 p = parent_[index]; p != kInvalidIndex; p = parent_[p]) {
            if (alive_[p] != 0u && local_dirty_[p] != 0u)
                return true;
        }
        return false;
    };

    auto update_subtree = [&](auto& self, u32 index, bool parent_dirty) -> void {
        if (index >= alive_.size() || alive_[index] == 0u)
            return;
        ++stats.nodes_visited;
        const u32 p = parent_[index];
        const bool dirty = local_dirty_[index] != 0u || parent_dirty;
        effective_dirty_[index] = dirty ? 1u : 0u;
        if (dirty) {
            if (local_matrix_dirty_[index] != 0u) {
                local_[index] = local_transform_matrix(LocalTransform{local_translation_[index],
                                                                      local_rotation_[index],
                                                                      local_scale_[index]});
                local_matrix_dirty_[index] = 0u;
            }
            world_[index] =
                p == kInvalidIndex ? local_[index] : math::mul_affine(world_[p], local_[index]);
            local_dirty_[index] = 0u;
            ++stats.transforms_updated;
        }
        for (u32 child = first_child_[index]; child != kInvalidIndex; child = next_sibling_[child])
            self(self, child, dirty);
    };

    for (u32 index : dirty_roots_) {
        if (index >= alive_.size() || alive_[index] == 0u || local_dirty_[index] == 0u)
            continue;
        if (has_dirty_ancestor(index))
            continue;
        update_subtree(update_subtree, index, false);
    }

    for (u32 index : dirty_roots_) {
        if (index < dirty_queued_.size())
            dirty_queued_[index] = 0u;
    }
    dirty_roots_.clear();

    if (stats.transforms_updated == 0u) {
        for (u32 index = 0; index < local_dirty_.size(); ++index) {
            if (alive_[index] != 0u && local_dirty_[index] != 0u)
                mark_dirty(make_handle(index, generation_[index]));
        }
    }
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

void SceneGraph::recompute_depth_bounds() noexcept {
    max_depth_ = 0u;
    for (u32 i = 0; i < alive_.size(); ++i) {
        if (alive_[i] == 0u)
            continue;
        max_depth_ = std::max(max_depth_, depth_[i]);
    }
}

void SceneGraph::update_subtree_depths(u32 root_index, u32 depth) {
    depth_[root_index] = depth;
    for (u32 child = first_child_[root_index]; child != kInvalidIndex; child = next_sibling_[child])
        update_subtree_depths(child, depth + 1u);
}

}  // namespace psynder::scene
