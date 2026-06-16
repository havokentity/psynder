# SPDX-License-Identifier: MIT
# psynder_ai — M-AI. Owns: the reusable DOTS enemy-AI module
# (agent / perception / patrol components + perceive/think/act systems) for the
# shooter targets (Quake/Duke3D/Delta Force).
#
# Engine-side + host-agnostic: no rendering, no host, no net. Side effects
# (line-of-sight, fire, movement-apply) are injected as host hooks on the
# AiContext, so this lane depends only on scene (ECS + components), gameplay
# (combat components: Health/Faction reuse), and math — never the reverse.
psynder_add_lane(ai LINKS psynder_core psynder_math psynder_scene psynder_gameplay)
