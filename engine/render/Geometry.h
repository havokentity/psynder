// SPDX-License-Identifier: MIT
// Psynder — render geometry handles shared by the hybrid scene renderer.

#pragma once

#include "core/Types.h"
#include "math/Bounds.h"
#include "render/Texture.h"
#include "render/raster/Raster.h"

#include <algorithm>
#include <span>
#include <vector>

namespace psynder::render {

struct MeshTag {};
using MeshId = Handle<MeshTag>;

struct MeshDesc {
    const raster::Vertex* vertices = nullptr;
    u32 vertex_count = 0;
    const u32* indices = nullptr;
    u32 index_count = 0;
    TextureView base_color{};
    const TextureAsset* base_color_asset = nullptr;
    raster::CullMode cull = raster::CullMode::Back;
    math::Aabb local_bounds = math::aabb_empty();
};

struct MeshView {
    std::span<const raster::Vertex* const> vertices;
    std::span<const u32> vertex_count;
    std::span<const u32* const> indices;
    std::span<const u32> index_count;
    std::span<const TextureView> base_color;
    std::span<const TextureAsset* const> base_color_asset;
    std::span<const raster::CullMode> cull;
    std::span<const math::Aabb> local_bounds;
};

class MeshLibrary {
   public:
    void reserve(u32 count) {
        generation_.reserve(count);
        alive_.reserve(count);
        vertices_.reserve(count);
        vertex_count_.reserve(count);
        indices_.reserve(count);
        index_count_.reserve(count);
        base_color_.reserve(count);
        base_color_asset_.reserve(count);
        cull_.reserve(count);
        local_bounds_.reserve(count);
    }

    void clear() {
        generation_.clear();
        alive_.clear();
        free_.clear();
        vertices_.clear();
        vertex_count_.clear();
        indices_.clear();
        index_count_.clear();
        base_color_.clear();
        base_color_asset_.clear();
        cull_.clear();
        local_bounds_.clear();
    }

    MeshId create_mesh(const MeshDesc& desc) {
        u32 index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = static_cast<u32>(generation_.size());
            generation_.push_back(1u);
            alive_.push_back(0u);
            vertices_.push_back(nullptr);
            vertex_count_.push_back(0u);
            indices_.push_back(nullptr);
            index_count_.push_back(0u);
            base_color_.push_back({});
            base_color_asset_.push_back(nullptr);
            cull_.push_back(raster::CullMode::Back);
            local_bounds_.push_back(math::aabb_empty());
        }

        alive_[index] = 1u;
        write_slot(index, desc);
        return make_handle(index, generation_[index]);
    }

    bool destroy(MeshId id) {
        if (!valid(id))
            return false;
        const u32 index = handle_index(id);
        alive_[index] = 0u;
        generation_[index] = (generation_[index] + 1u) & 0xFFu;
        if (generation_[index] == 0u)
            generation_[index] = 1u;
        free_.push_back(index);
        return true;
    }

    bool update(MeshId id, const MeshDesc& desc) {
        if (!valid(id))
            return false;
        write_slot(handle_index(id), desc);
        return true;
    }

    bool update_base_color(MeshId id, TextureView texture) {
        if (!valid(id))
            return false;
        base_color_[handle_index(id)] = texture;
        return true;
    }

    [[nodiscard]] bool valid(MeshId id) const noexcept {
        if (!id.valid())
            return false;
        const u32 index = handle_index(id);
        return index < generation_.size() && alive_[index] != 0u &&
               generation_[index] == handle_gen(id);
    }

    [[nodiscard]] bool slot(MeshId id, u32& out) const noexcept {
        if (!valid(id))
            return false;
        out = handle_index(id);
        return true;
    }

    [[nodiscard]] MeshDesc get(MeshId id, const MeshDesc& fallback = {}) const noexcept {
        if (!valid(id))
            return fallback;
        const u32 i = handle_index(id);
        return {vertices_[i],
                vertex_count_[i],
                indices_[i],
                index_count_[i],
                base_color_[i],
                base_color_asset_[i],
                cull_[i],
                local_bounds_[i]};
    }

    [[nodiscard]] MeshView view() const noexcept {
        return {vertices_,
                vertex_count_,
                indices_,
                index_count_,
                base_color_,
                base_color_asset_,
                cull_,
                local_bounds_};
    }

    [[nodiscard]] u32 slot_count() const noexcept { return static_cast<u32>(generation_.size()); }
    [[nodiscard]] u32 live_count() const noexcept {
        u32 count = 0;
        for (u8 alive : alive_)
            count += alive != 0u ? 1u : 0u;
        return count;
    }
    [[nodiscard]] u32 free_count() const noexcept { return static_cast<u32>(free_.size()); }

   private:
    static constexpr u32 handle_index(MeshId id) noexcept { return (id.raw & 0x00FFFFFFu) - 1u; }
    static constexpr u32 handle_gen(MeshId id) noexcept { return id.raw >> 24; }
    static constexpr MeshId make_handle(u32 index, u32 generation) noexcept {
        return MeshId{((generation & 0xFFu) << 24) | ((index + 1u) & 0x00FFFFFFu)};
    }

    void write_slot(u32 index, const MeshDesc& desc) noexcept {
        vertices_[index] = desc.vertices;
        vertex_count_[index] = desc.vertex_count;
        indices_[index] = desc.indices;
        index_count_[index] = desc.index_count;
        base_color_[index] = desc.base_color;
        base_color_asset_[index] = desc.base_color_asset;
        cull_[index] = desc.cull;
        local_bounds_[index] = desc.local_bounds;
    }

    std::vector<u8> generation_;
    std::vector<u8> alive_;
    std::vector<u32> free_;
    std::vector<const raster::Vertex*> vertices_;
    std::vector<u32> vertex_count_;
    std::vector<const u32*> indices_;
    std::vector<u32> index_count_;
    std::vector<TextureView> base_color_;
    std::vector<const TextureAsset*> base_color_asset_;
    std::vector<raster::CullMode> cull_;
    std::vector<math::Aabb> local_bounds_;
};

[[nodiscard]] constexpr MeshId mesh_id_from_raw(u32 raw) noexcept {
    return MeshId{raw};
}

}  // namespace psynder::render
