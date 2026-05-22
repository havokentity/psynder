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

enum class MaterialCpuEffect : u8 {
    None = 0,
    UvScroll = 1,
};

enum class MaterialRasterShadowMode : u8 {
    None = 0,
    ProjectedDecal = 1,
    BakedLightmap = 2,
};

enum class MaterialShadowAlphaMode : u8 {
    Opaque = 0,
    AlphaTest = 1,
    AlphaBlend = 2,
    Disabled = 3,
};

struct MaterialCpuEffectDesc {
    MaterialCpuEffect type = MaterialCpuEffect::None;
    f32 uv_scroll_u = 0.0f;
    f32 uv_scroll_v = 0.0f;
    f32 phase = 0.0f;
    f32 rate = 1.0f;
};

enum MaterialFlags : u32 {
    Material_RasterVisible = 1u << 0,
    Material_RtVisible = 1u << 1,
    Material_CastsRtShadow = 1u << 2,
    Material_ReceivesRtShadow = 1u << 3,
    Material_CastsRasterShadow = 1u << 4,
    Material_ReceivesRasterShadow = 1u << 5,
    Material_Editable = 1u << 6,
    Material_BakeVisible = 1u << 7,
    Material_CastsBakedShadow = 1u << 8,
    Material_ReceivesBakedShadow = 1u << 9,
    Material_EmissiveBakes = 1u << 10,

    Material_DefaultFlags = Material_RasterVisible | Material_RtVisible | Material_CastsRtShadow |
                            Material_ReceivesRtShadow | Material_CastsRasterShadow |
                            Material_ReceivesRasterShadow | Material_Editable,
};

inline constexpr u32 Material_BakedLightingMask =
    Material_BakeVisible | Material_CastsBakedShadow | Material_ReceivesBakedShadow |
    Material_EmissiveBakes;

struct MaterialDesc {
    u32 albedo_rgba8 = 0xFFFFFFFFu;
    u32 base_color_texture = 0u;
    f32 alpha_cutoff = 0.5f;
    f32 reflectivity = 0.0f;
    f32 roughness = 1.0f;
    f32 emissive = 0.0f;
    MaterialCpuEffectDesc cpu_effect{};
    MaterialWinding winding = MaterialWinding::Ccw;
    MaterialBlendMode blend = MaterialBlendMode::Opaque;
    MaterialRasterShadowMode raster_shadow_mode = MaterialRasterShadowMode::None;
    MaterialShadowAlphaMode shadow_alpha = MaterialShadowAlphaMode::Opaque;
    f32 shadow_opacity = 0.5f;
    f32 shadow_softness = 0.5f;
    u32 flags = Material_DefaultFlags;
};

struct MaterialView {
    std::span<const u32> albedo_rgba8;
    std::span<const u32> base_color_texture;
    std::span<const f32> alpha_cutoff;
    std::span<const f32> reflectivity;
    std::span<const f32> roughness;
    std::span<const f32> emissive;
    std::span<const MaterialCpuEffect> cpu_effect;
    std::span<const f32> uv_scroll_u;
    std::span<const f32> uv_scroll_v;
    std::span<const f32> effect_phase;
    std::span<const f32> effect_rate;
    std::span<const MaterialWinding> winding;
    std::span<const MaterialBlendMode> blend;
    std::span<const MaterialRasterShadowMode> raster_shadow_mode;
    std::span<const MaterialShadowAlphaMode> shadow_alpha;
    std::span<const f32> shadow_opacity;
    std::span<const f32> shadow_softness;
    std::span<const u32> flags;
};

class MaterialLibrary {
   public:
    void reserve(u32 count) {
        generation_.reserve(count);
        alive_.reserve(count);
        albedo_rgba8_.reserve(count);
        base_color_texture_.reserve(count);
        alpha_cutoff_.reserve(count);
        reflectivity_.reserve(count);
        roughness_.reserve(count);
        emissive_.reserve(count);
        cpu_effect_.reserve(count);
        uv_scroll_u_.reserve(count);
        uv_scroll_v_.reserve(count);
        effect_phase_.reserve(count);
        effect_rate_.reserve(count);
        winding_.reserve(count);
        blend_.reserve(count);
        raster_shadow_mode_.reserve(count);
        shadow_alpha_.reserve(count);
        shadow_opacity_.reserve(count);
        shadow_softness_.reserve(count);
        flags_.reserve(count);
    }

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
        cpu_effect_.clear();
        uv_scroll_u_.clear();
        uv_scroll_v_.clear();
        effect_phase_.clear();
        effect_rate_.clear();
        winding_.clear();
        blend_.clear();
        raster_shadow_mode_.clear();
        shadow_alpha_.clear();
        shadow_opacity_.clear();
        shadow_softness_.clear();
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
            cpu_effect_.push_back(MaterialCpuEffect::None);
            uv_scroll_u_.push_back(0.0f);
            uv_scroll_v_.push_back(0.0f);
            effect_phase_.push_back(0.0f);
            effect_rate_.push_back(1.0f);
            winding_.push_back(MaterialWinding::Ccw);
            blend_.push_back(MaterialBlendMode::Opaque);
            raster_shadow_mode_.push_back(MaterialRasterShadowMode::None);
            shadow_alpha_.push_back(MaterialShadowAlphaMode::Opaque);
            shadow_opacity_.push_back(0.5f);
            shadow_softness_.push_back(0.5f);
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
        out.cpu_effect.type = cpu_effect_[i];
        out.cpu_effect.uv_scroll_u = uv_scroll_u_[i];
        out.cpu_effect.uv_scroll_v = uv_scroll_v_[i];
        out.cpu_effect.phase = effect_phase_[i];
        out.cpu_effect.rate = effect_rate_[i];
        out.winding = winding_[i];
        out.blend = blend_[i];
        out.raster_shadow_mode = raster_shadow_mode_[i];
        out.shadow_alpha = shadow_alpha_[i];
        out.shadow_opacity = shadow_opacity_[i];
        out.shadow_softness = shadow_softness_[i];
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
                cpu_effect_,
                uv_scroll_u_,
                uv_scroll_v_,
                effect_phase_,
                effect_rate_,
                winding_,
                blend_,
                raster_shadow_mode_,
                shadow_alpha_,
                shadow_opacity_,
                shadow_softness_,
                flags_};
    }

    [[nodiscard]] bool slot(MaterialId id, u32& out) const noexcept {
        if (!valid(id))
            return false;
        out = handle_index(id);
        return true;
    }

    [[nodiscard]] u32 slot_count() const noexcept { return static_cast<u32>(generation_.size()); }
    [[nodiscard]] u32 free_count() const noexcept { return static_cast<u32>(free_.size()); }
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
        cpu_effect_[index] = desc.cpu_effect.type;
        uv_scroll_u_[index] = desc.cpu_effect.uv_scroll_u;
        uv_scroll_v_[index] = desc.cpu_effect.uv_scroll_v;
        effect_phase_[index] = desc.cpu_effect.phase;
        effect_rate_[index] = desc.cpu_effect.rate;
        winding_[index] = desc.winding;
        blend_[index] = desc.blend;
        raster_shadow_mode_[index] = desc.raster_shadow_mode;
        shadow_alpha_[index] = desc.shadow_alpha;
        shadow_opacity_[index] = std::clamp(desc.shadow_opacity, 0.0f, 1.0f);
        shadow_softness_[index] = std::clamp(desc.shadow_softness, 0.0f, 1.0f);
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
    std::vector<MaterialCpuEffect> cpu_effect_;
    std::vector<f32> uv_scroll_u_;
    std::vector<f32> uv_scroll_v_;
    std::vector<f32> effect_phase_;
    std::vector<f32> effect_rate_;
    std::vector<MaterialWinding> winding_;
    std::vector<MaterialBlendMode> blend_;
    std::vector<MaterialRasterShadowMode> raster_shadow_mode_;
    std::vector<MaterialShadowAlphaMode> shadow_alpha_;
    std::vector<f32> shadow_opacity_;
    std::vector<f32> shadow_softness_;
    std::vector<u32> flags_;
};

PSY_FORCEINLINE bool material_flag_enabled(const MaterialDesc& material, MaterialFlags flag) noexcept {
    return (material.flags & static_cast<u32>(flag)) != 0u;
}

}  // namespace psynder::render
