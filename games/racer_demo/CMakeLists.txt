# SPDX-License-Identifier: MIT
# psynder_racer_demo — DEMO GAME 2. A standalone NFS2SE-style spline-road racing
# demo that composes the vehicle physics, a closed-loop Bezier track extruded to
# a scene mesh, an NFS-style chase camera, and lap timing into one drivable game.
# Player drives with WASD (W throttle, S brake/reverse, A/D steer); a minimal
# ui::imm HUD shows the current lap / time / speed / best lap.
#
# Standalone exe modelled on games/shooter_demo + samples/04_nfs_track: it reuses
# the shared samples::fix_winding helper + the app::WindowApp host boilerplate,
# and links ONLY public engine libs. Renders in RASTER mode with scene lights.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(psynder_racer_demo ${SRC})
target_link_libraries(psynder_racer_demo PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_render_rt psynder_render_post psynder_render_hybrid
    psynder_scene psynder_physics psynder_asset psynder_platform
    psynder_world
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)
