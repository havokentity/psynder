# SPDX-License-Identifier: MIT
# psynder_render_hybrid — M-HYB. Owns: hybrid-shadow occluder builder bridging
# the raster fragment stage to the software raytracer (raster primary visibility
# + traced shadow rays in the same frame; DESIGN.md §8). Links both raster and
# rt — this is the deliberate raster->rt edge; the raster core stays rt-free.
psynder_add_lane(render_hybrid LINKS
    psynder_render_core
    psynder_render_raster
    psynder_render_rt
    psynder_core
    psynder_math
    psynder_scene
)
