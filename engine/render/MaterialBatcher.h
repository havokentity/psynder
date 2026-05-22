// SPDX-License-Identifier: MIT
// Psynder — cache-linear material batching for scene render work.

#pragma once

#include "render/Material.h"
#include "scene/SceneEcs.h"

#include <algorithm>
#include <span>
#include <vector>

namespace psynder::render {

struct MaterialBatch {
    MaterialId material{};
    u32 material_slot = 0;
    MaterialCpuEffect cpu_effect = MaterialCpuEffect::None;
    u32 first_index = 0;
    u32 count = 0;
};

class MaterialBatcher {
   public:
    void clear() {
        batches_.clear();
        item_indices_.clear();
        counts_.clear();
        offsets_.clear();
        cursors_.clear();
        material_ids_.clear();
    }

    void reserve(u32 material_slots, u32 item_count) {
        batches_.reserve(material_slots);
        item_indices_.reserve(item_count);
        counts_.reserve(material_slots);
        offsets_.reserve(material_slots + 1u);
        cursors_.reserve(material_slots);
        material_ids_.reserve(material_slots);
    }

    void build(std::span<const scene::SceneRenderItem> items, const MaterialLibrary& materials) {
        const u32 slot_count = materials.slot_count();
        counts_.assign(slot_count, 0u);
        offsets_.assign(static_cast<usize>(slot_count) + 1u, 0u);
        cursors_.assign(slot_count, 0u);
        material_ids_.assign(slot_count, {});

        for (const scene::SceneRenderItem& item : items) {
            u32 slot = 0;
            if (!materials.slot(item.material, slot))
                continue;
            if (counts_[slot] == 0u)
                material_ids_[slot] = item.material;
            ++counts_[slot];
        }

        u32 total = 0;
        for (u32 slot = 0; slot < slot_count; ++slot) {
            offsets_[slot] = total;
            total += counts_[slot];
        }
        offsets_[slot_count] = total;
        item_indices_.assign(total, 0u);

        for (u32 item_index = 0; item_index < static_cast<u32>(items.size()); ++item_index) {
            u32 slot = 0;
            if (!materials.slot(items[item_index].material, slot))
                continue;
            item_indices_[offsets_[slot] + cursors_[slot]++] = item_index;
        }

        batches_.clear();
        batches_.reserve(std::min(slot_count, total));
        const MaterialView view = materials.view();
        for (u32 slot = 0; slot < slot_count; ++slot) {
            const u32 count = counts_[slot];
            if (count == 0u)
                continue;
            batches_.push_back(
                MaterialBatch{material_ids_[slot], slot, view.cpu_effect[slot], offsets_[slot], count});
        }
    }

    [[nodiscard]] std::span<const MaterialBatch> batches() const noexcept {
        return {batches_.data(), batches_.size()};
    }
    [[nodiscard]] std::span<const u32> item_indices() const noexcept {
        return {item_indices_.data(), item_indices_.size()};
    }
    [[nodiscard]] std::span<const u32> indices_for(const MaterialBatch& batch) const noexcept {
        return {item_indices_.data() + batch.first_index, batch.count};
    }
    [[nodiscard]] u32 batch_count() const noexcept { return static_cast<u32>(batches_.size()); }
    [[nodiscard]] u32 item_count() const noexcept { return static_cast<u32>(item_indices_.size()); }

   private:
    std::vector<MaterialBatch> batches_;
    std::vector<u32> item_indices_;
    std::vector<u32> counts_;
    std::vector<u32> offsets_;
    std::vector<u32> cursors_;
    std::vector<MaterialId> material_ids_;
};

}  // namespace psynder::render
