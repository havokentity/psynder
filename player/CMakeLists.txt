# SPDX-License-Identifier: MIT
# Psynder Player — generic cooked-game runtime shell.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
add_executable(psynder_player ${SRC})
target_link_libraries(psynder_player PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_scene psynder_render_raster psynder_render_post psynder_asset psynder_platform
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)
