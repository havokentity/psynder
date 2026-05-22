// SPDX-License-Identifier: MIT
// Psynder — shared material contract for raster + RT renderers.

#pragma once

#include "core/Types.h"

#include <algorithm>
#include <span>
#include <vector>

namespace psynder::render {

struct MaterialTag {};
using MaterialId = Handle<MaterialTag>;

enum class MaterialWinding : u8 {
    Ccw = 0,
    Cw = 1,
    DoubleSided = 2,
};

enum class MaterialBlendMode : u8 {
    Opaque = 0,
    AlphaTest = 1,
    AlphaBlend = 2,
};

enum MaterialFlags : u32 {
    Material_RasterVisible = 1u << 0,
    Material_RtVisible = 1u << 1,
    Material_CastsRtShadow = 1u << 2,
    Material_ReceivesRtShadow = 1u << 3,
    Material_CastsRasterShadow = 1u << 4,
    Material_ReceivesRasterShadow = 1u << 5,
    Material_Editable = 1u << 6,

    Material_DefaultFlags = Material_RasterVisible | Material_RtVisible | Material_CastsRtShadow |
                            Material_ReceivesRtShadow | Material_CastsRasterShadow |
                            Material_ReceivesRasterShadow | Material_Editable,
};

struct MaterialDesc {
    u32 albedo_rgba8 = 0xFFFFFFFFu;
    u32 base_color_texture = 0u;
    f32 alpha_cutoff = 0.5f;
    f32 reflectivity = 0.0f;
    f32 roughness = 1.0f;
    f32 emissive = 0.0f;
    MaterialWinding winding = MaterialWinding::Ccw;
    MaterialBlendMode blend = MaterialBlendMode::Opaque;
    u32 flags = Material_DefaultFlags;
};

struct MaterialView {
    std::span<const u32> albedo_rgba8;
    std::span<const u32> base_color_texture;
    std::span<const f32> alpha_cutoff;
    std::span<const f32> reflectivity;
    std::span<const f32> roughness;
    std::span<const f32> emissive;
    std::span<const MaterialWinding> winding;
    std::span<const MaterialBlendMode> blend;
    std::span<const u32> flags;
};

class MaterialLibrary {
   public:
    void clear() {
        generation_.clear();
        alive_.clear();
        free_.clear();
        albedo_rgba8_.clear();
        base_color_texture_.clear();
        alpha_cutoff_.clear();
        reflectivity_.clear();
        roughness_.clear();
        emissive_.clear();
        winding_.clear();
        blend_.clear();
        flags_.clear();
    }

    MaterialId create(const MaterialDesc& desc) {
        u32 index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = static_cast<u32>(generation_.size());
            generation_.push_back(1u);
            alive_.push_back(0u);
            albedo_rgba8_.push_back(0u);
            base_color_texture_.push_back(0u);
            alpha_cutoff_.push_back(0.0f);
            reflectivity_.push_back(0.0f);
            roughness_.push_back(0.0f);
            emissive_.push_back(0.0f);
            winding_.push_back(MaterialWinding::Ccw);
            blend_.push_back(MaterialBlendMode::Opaque);
            flags_.push_back(0u);
        }

        alive_[index] = 1u;
        write_slot(index, desc);
        return make_handle(index, generation_[index]);
    }

    bool destroy(MaterialId id) {
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

    bool update(MaterialId id, const MaterialDesc& desc) {
        if (!valid(id))
            return false;
        write_slot(handle_index(id), desc);
        return true;
    }

    [[nodiscard]] bool valid(MaterialId id) const noexcept {
        if (!id.valid())
            return false;
        const u32 index = handle_index(id);
        return index < generation_.size() && alive_[index] != 0u &&
               generation_[index] == handle_gen(id);
    }

    [[nodiscard]] MaterialDesc get(MaterialId id, const MaterialDesc& fallback = {}) const noexcept {
        if (!valid(id))
            return fallback;
        const u32 i = handle_index(id);
        MaterialDesc out{};
        out.albedo_rgba8 = albedo_rgba8_[i];
        out.base_color_texture = base_color_texture_[i];
        out.alpha_cutoff = alpha_cutoff_[i];
        out.reflectivity = reflectivity_[i];
        out.roughness = roughness_[i];
        out.emissive = emissive_[i];
        out.winding = winding_[i];
        out.blend = blend_[i];
        out.flags = flags_[i];
        return out;
    }

    [[nodiscard]] MaterialView view() const noexcept {
        return {albedo_rgba8_,
                base_color_texture_,
                alpha_cutoff_,
                reflectivity_,
                roughness_,
                emissive_,
                winding_,
                blend_,
                flags_};
    }

    [[nodiscard]] bool slot(MaterialId id, u32& out) const noexcept {
        if (!valid(id))
            return false;
        out = handle_index(id);
        return true;
    }

    [[nodiscard]] u32 slot_count() const noexcept { return static_cast<u32>(generation_.size()); }
    [[nodiscard]] u32 live_count() const noexcept {
        u32 count = 0;
        for (u8 alive : alive_)
            count += alive != 0u ? 1u : 0u;
        return count;
    }

   private:
    static constexpr u32 handle_index(MaterialId id) noexcept {
        return (id.raw & 0x00FFFFFFu) - 1u;
    }
    static constexpr u32 handle_gen(MaterialId id) noexcept { return id.raw >> 24; }
    static constexpr MaterialId make_handle(u32 index, u32 generation) noexcept {
        return MaterialId{((generation & 0xFFu) << 24) | ((index + 1u) & 0x00FFFFFFu)};
    }

    void write_slot(u32 index, const MaterialDesc& desc) noexcept {
        albedo_rgba8_[index] = desc.albedo_rgba8;
        base_color_texture_[index] = desc.base_color_texture;
        alpha_cutoff_[index] = std::clamp(desc.alpha_cutoff, 0.0f, 1.0f);
        reflectivity_[index] = std::clamp(desc.reflectivity, 0.0f, 1.0f);
        roughness_[index] = std::clamp(desc.roughness, 0.0f, 1.0f);
        emissive_[index] = std::max(0.0f, desc.emissive);
        winding_[index] = desc.winding;
        blend_[index] = desc.blend;
        flags_[index] = desc.flags;
    }

    std::vector<u8> generation_;
    std::vector<u8> alive_;
    std::vector<u32> free_;
    std::vector<u32> albedo_rgba8_;
    std::vector<u32> base_color_texture_;
    std::vector<f32> alpha_cutoff_;
    std::vector<f32> reflectivity_;
    std::vector<f32> roughness_;
    std::vector<f32> emissive_;
    std::vector<MaterialWinding> winding_;
    std::vector<MaterialBlendMode> blend_;
    std::vector<u32> flags_;
};

PSY_FORCEINLINE bool material_flag_enabled(const MaterialDesc& material, MaterialFlags flag) noexcept {
    return (material.flags & static_cast<u32>(flag)) != 0u;
}

}  // namespace psynder::render
