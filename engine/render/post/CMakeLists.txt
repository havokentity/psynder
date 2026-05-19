# SPDX-License-Identifier: MIT
# psynder_render_post — Lane 09. Owns: resolve, dither, tonemap, bloom, scanline filter.
psynder_add_lane(render_post LINKS psynder_core psynder_math psynder_simd)
