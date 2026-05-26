# SPDX-License-Identifier: MIT
# psynder_editor_world — Lane 18 (editor). Owns: "level source" loaders that
# populate a scene::Scene from a BSP map (engine/world/bsp) or a heightfield
# (engine/world/outdoor) so the editor can open indoor maps / terrains as
# scenes, plus the in-memory demo BSP / heightmap generators.
psynder_add_lane(editor_world
    LINKS
        psynder_core
        psynder_math
        psynder_scene
        psynder_render_raster
        psynder_world_bsp
        psynder_world_outdoor
)
