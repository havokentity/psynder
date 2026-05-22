// SPDX-License-Identifier: MIT
// Psynder — shared scene-to-renderer queue builder for hybrid raster + RT.

#pragma once

#include "render/Material.h"
#include "scene/SceneEcs.h"

#include <vector>

namespace psynder::render {

struct SceneRenderQueues {
    std::vector<scene::SceneRenderItem> all;
    std::vector<scene::SceneRenderItem> raster_opaque;
    std::vector<scene::SceneRenderItem> raster_transparent;
    std::vector<scene::SceneRenderItem> rt_visible;
    std::vector<scene::SceneRenderItem> rt_shadow_casters;

    void clear() {
        all.clear();
        raster_opaque.clear();
        raster_transparent.clear();
        rt_visible.clear();
        rt_shadow_casters.clear();
    }

    void reserve_like_all() {
        raster_opaque.reserve(all.size());
        raster_transparent.reserve(all.size());
        rt_visible.reserve(all.size());
        rt_shadow_casters.reserve(all.size());
    }
};

inline void build_scene_render_queues(scene::RuntimeScene& scene, SceneRenderQueues& queues) {
    queues.clear();
    scene.update_transforms();
    scene.gather_render_items(queues.all);
    queues.reserve_like_all();

    const MaterialLibrary& materials = scene.materials();
    for (const scene::SceneRenderItem& item : queues.all) {
        if (!materials.valid(item.material))
            continue;
        const MaterialDesc material = materials.get(item.material);
        if ((material.flags & Material_RasterVisible) != 0u) {
            if (material.blend == MaterialBlendMode::AlphaBlend)
                queues.raster_transparent.push_back(item);
            else
                queues.raster_opaque.push_back(item);
        }
        if ((material.flags & Material_RtVisible) != 0u)
            queues.rt_visible.push_back(item);
        if ((material.flags & Material_CastsRtShadow) != 0u)
            queues.rt_shadow_casters.push_back(item);
    }
}

}  // namespace psynder::render
