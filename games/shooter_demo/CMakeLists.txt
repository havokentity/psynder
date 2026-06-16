# SPDX-License-Identifier: MIT
# psynder_shooter_demo — first DEMO GAME. A standalone Duke3D/Quake-style indoor
# first-person shooter that composes the whole engine stack: an indoor room
# (scene mesh geometry), a first-person CharacterController player, AI enemies
# that perceive/chase/shoot (engine/ai), hitscan combat (engine/gameplay), our
# own physics raycast for LOS + hitscan-vs-geometry (engine/physics), and the
# hybrid render path (raster primary visibility + traced shadow rays).
#
# Standalone exe modelled on samples/13_rt_quake: it reuses the shared
# samples::CharacterController + the app::WindowApp host boilerplate, and links
# ONLY public engine libs.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(psynder_shooter_demo ${SRC})
target_link_libraries(psynder_shooter_demo PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_render_rt psynder_render_post psynder_render_hybrid
    psynder_scene psynder_physics psynder_asset psynder_platform
    psynder_gameplay psynder_ai
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)
