# SPDX-License-Identifier: MIT
# psynder_editor_play - editor PLAY MODE physics runtime.
#
# Engine-side bridge the editor host drives on entering / leaving Play mode:
# scans the scene's RigidBody / CharacterController components, builds a
# physics::World, steps it, and writes resolved poses back into
# TransformComponents column-at-a-time. Links scene (ECS components +
# set_transform / update_world_transforms) and physics (rigid bodies +
# character controller). All per-entity sim linkage lives in pooled ECS
# components (DOTS), never a side-table.
#
# The Play tick also runs the GAMEPLAY phase (combat -> AI -> PsyGraph) each
# frame, so it links those lanes too: gameplay (combat systems + fire_weapon /
# apply_damage / resolve_deaths), ai (perceive/think/act), and psygraph (the
# visual-script GraphRuntime + host hooks). These are additive to the original
# scene/physics deps.
psynder_add_lane(editor_play
    LINKS
        psynder_core
        psynder_math
        psynder_scene
        psynder_physics
        psynder_gameplay
        psynder_ai
        psynder_psygraph
)
