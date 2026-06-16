// SPDX-License-Identifier: MIT
// Psynder — cache-coherent scene graph. Lane 06 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <limits>
#include <new>
#include <span>
#include <type_traits>
#include <vector>

namespace psynder::scene {
namespace detail {

template <class T, usize Alignment = kCacheLine>
class CacheAlignedAllocator {
   public:
    using value_type = T;
    using is_always_equal = std::true_type;

    static_assert(Alignment >= alignof(T), "allocator alignment must satisfy T");

    CacheAlignedAllocator() noexcept = default;

    template <class U>
    constexpr CacheAlignedAllocator(const CacheAlignedAllocator<U, Alignment>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_array_new_length{};
        if (count == 0)
            return nullptr;
        return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{Alignment}));
    }

    void deallocate(T* ptr, std::size_t) noexcept {
        ::operator delete(ptr, std::align_val_t{Alignment});
    }

    template <class U>
    struct rebind {
        using other = CacheAlignedAllocator<U, Alignment>;
    };
};

template <class A, class B, usize Alignment>
constexpr bool operator==(const CacheAlignedAllocator<A, Alignment>&,
                          const CacheAlignedAllocator<B, Alignment>&) noexcept {
    return true;
}

template <class A, class B, usize Alignment>
constexpr bool operator!=(const CacheAlignedAllocator<A, Alignment>&,
                          const CacheAlignedAllocator<B, Alignment>&) noexcept {
    return false;
}

template <class T>
using AlignedVector = std::vector<T, CacheAlignedAllocator<T>>;

}  // namespace detail

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
    void reserve_nodes(u32 count);
    void reserve_analytic_spheres(u32 count);

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
    u32 node_capacity() const noexcept { return static_cast<u32>(generation_.capacity()); }
    u32 free_node_count() const noexcept { return static_cast<u32>(free_nodes_.size()); }
    u32 dirty_root_capacity() const noexcept { return static_cast<u32>(dirty_roots_.capacity()); }
    u32 analytic_sphere_capacity() const noexcept {
        return static_cast<u32>(analytic_spheres_.capacity());
    }

   private:
    static SceneNode make_handle(u32 index, u32 generation) noexcept;
    bool valid_index(SceneNode node) const noexcept;
    void attach_child(u32 parent_index, u32 child_index) noexcept;
    void detach_child(u32 child_index) noexcept;
    void recompute_depth_bounds() noexcept;
    void update_subtree_depths(u32 root_index, u32 depth);

    detail::AlignedVector<u32> generation_;
    detail::AlignedVector<u8> alive_;
    detail::AlignedVector<u32> parent_;
    detail::AlignedVector<u32> first_child_;
    detail::AlignedVector<u32> next_sibling_;
    detail::AlignedVector<u32> prev_sibling_;
    detail::AlignedVector<u32> depth_;
    detail::AlignedVector<math::Vec3> local_translation_;
    detail::AlignedVector<math::Quat> local_rotation_;
    detail::AlignedVector<math::Vec3> local_scale_;
    detail::AlignedVector<math::Mat4> local_;
    detail::AlignedVector<math::Mat4> world_;
    detail::AlignedVector<u8> local_dirty_;
    detail::AlignedVector<u8> local_matrix_dirty_;
    detail::AlignedVector<u8> effective_dirty_;
    detail::AlignedVector<u8> dirty_queued_;
    detail::AlignedVector<u32> dirty_roots_;
    detail::AlignedVector<u32> free_nodes_;
    detail::AlignedVector<AnalyticSphereDesc> analytic_spheres_;
    u32 live_nodes_ = 0;
    u32 max_depth_ = 0;
};

math::Mat4 local_transform_matrix(const LocalTransform& local);

}  // namespace psynder::scene
