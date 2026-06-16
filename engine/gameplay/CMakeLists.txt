# SPDX-License-Identifier: MIT
# psynder_gameplay — M-COMBAT. Owns: reusable DOTS gameplay-combat module
# (health / weapon-runtime / projectile / hitbox / faction components +
# fire/projectile/damage/death systems) for the shooter targets.
#
# Engine-side + editor-agnostic: no rendering, no host. Depends on scene
# (ECS + components), physics (public query surface), and math — never the
# reverse (no scene/physics -> gameplay dependency).
psynder_add_lane(gameplay LINKS psynder_core psynder_math psynder_scene psynder_physics)
