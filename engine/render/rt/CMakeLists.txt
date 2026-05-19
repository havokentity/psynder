# SPDX-License-Identifier: MIT
# psynder_render_rt — Lane 08. Owns: BVH8 builder, ray packets, refit, denoiser.
psynder_add_lane(render_rt LINKS psynder_core psynder_math psynder_simd psynder_jobs psynder_scene)
