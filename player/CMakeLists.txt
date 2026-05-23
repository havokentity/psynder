# SPDX-License-Identifier: MIT
# Psynder Arcade — generic cooked-game runtime shell.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
add_executable(psynder_player ${SRC})
set_target_properties(psynder_player PROPERTIES OUTPUT_NAME psynder_arcade)
target_link_libraries(psynder_player PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_scene psynder_render_raster psynder_render_post psynder_asset psynder_platform psynder_audio
    psynder_ui_imm
)

target_compile_definitions(psynder_player PRIVATE
    PSYNDER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

if(PSYNDER_EDITOR)
    target_link_libraries(psynder_player PRIVATE psynder_editor_core)
endif()
