// SPDX-License-Identifier: MIT
// Psynder — cache-coherent scene graph. Lane 06 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>
#include <vector>

namespace psynder::scene {

struct SceneNode {
    u32 raw = 0;
    constexpr bool valid() const noexcept { return raw != 0; }
    constexpr u32 index() const noexcept { return (raw & 0x00FFFFFFu) - 1u; }
    constexpr u32 gen() const noexcept { return raw >> 24; }
    constexpr bool operator==(const SceneNode& o) const noexcept = default;
};
inline constexpr SceneNode kInvalidSceneNode{};

struct LocalTransform {
    math::Vec3 translation{0.0f, 0.0f, 0.0f};
    math::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    math::Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct AnalyticSphereDesc {
    SceneNode node{};
    math::Sphere local_sphere{{0.0f, 0.0f, 0.0f}, 1.0f};
    u32 material_rgba8 = 0xFFFFFFFFu;
};

struct AnalyticSphereInstance {
    SceneNode node{};
    math::Sphere world_sphere{};
    u32 material_rgba8 = 0xFFFFFFFFu;
};

struct SceneGraphUpdateStats {
    u32 nodes_visited = 0;
    u32 transforms_updated = 0;
    u32 depth_levels = 0;
};

class SceneGraph {
   public:
    void clear();

    SceneNode create_node(SceneNode parent = kInvalidSceneNode, const LocalTransform& local = {});
    bool destroy_node(SceneNode node);
    bool alive(SceneNode node) const noexcept;

    // Reparenting preserves the parent-before-child storage invariant. If the
    // requested parent was created after `node`, this returns false instead of
    // degrading the one-pass world-transform update path.
    bool set_parent(SceneNode node, SceneNode parent);
    SceneNode parent(SceneNode node) const noexcept;

    void set_local_transform(SceneNode node, const LocalTransform& local);
    void set_local_matrix(SceneNode node, const math::Mat4& local);
    const math::Mat4& local_matrix(SceneNode node) const noexcept;
    const math::Mat4& world_matrix(SceneNode node) const noexcept;

    void mark_dirty(SceneNode node);
    SceneGraphUpdateStats update_world_transforms(u32 parallel_threshold = 128u);

    u32 add_analytic_sphere(const AnalyticSphereDesc& desc);
    std::span<const AnalyticSphereDesc> analytic_spheres() const noexcept;
    void gather_analytic_spheres(std::vector<AnalyticSphereInstance>& out) const;

    u32 node_count() const noexcept;
    u32 live_node_count() const noexcept { return live_nodes_; }

   private:
    static SceneNode make_handle(u32 index, u32 generation) noexcept;
    bool valid_index(SceneNode node) const noexcept;
    void attach_child(u32 parent_index, u32 child_index) noexcept;
    void detach_child(u32 child_index) noexcept;
    void rebuild_depth_lists();
    void update_subtree_depths(u32 root_index, u32 depth);

    std::vector<u32> generation_;
    std::vector<u8> alive_;
    std::vector<u32> parent_;
    std::vector<u32> first_child_;
    std::vector<u32> next_sibling_;
    std::vector<u32> prev_sibling_;
    std::vector<u32> depth_;
    std::vector<math::Mat4> local_;
    std::vector<math::Mat4> world_;
    std::vector<u8> local_dirty_;
    std::vector<u8> effective_dirty_;
    std::vector<u8> dirty_queued_;
    std::vector<u32> dirty_roots_;
    std::vector<std::vector<u32>> nodes_by_depth_;
    std::vector<AnalyticSphereDesc> analytic_spheres_;
    u32 live_nodes_ = 0;
};

math::Mat4 local_transform_matrix(const LocalTransform& local);

}  // namespace psynder::scene
