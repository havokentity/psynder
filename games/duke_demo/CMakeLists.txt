# SPDX-License-Identifier: MIT
# psynder_duke_demo - DEMO GAME 4. A standalone Duke3D/Quake-style INDOOR BSP
# shooter that demonstrates portal/PVS visibility culling over a multi-room
# level: rooms + corridors authored in code as scene mesh boxes with matching
# static physics slabs, a PARALLEL in-memory BSP + portal set (engine/world/bsp),
# and a runtime PVS built by world::bsp::build_pvs (Quake-style leaf-portal
# flood). Each frame the camera's leaf PVS row culls rooms it cannot see before
# the renderer rasterises them. A first-person CharacterController player, AI
# enemies that perceive/chase/shoot (engine/ai), hitscan combat (engine/gameplay),
# physics raycast LOS (engine/physics), and the hybrid render path
# (raster + traced shadow rays) round out the stack.
#
# Standalone exe modelled on games/shooter_demo: it reuses the shared
# samples::CharacterController + the app::WindowApp host boilerplate and links
# ONLY public engine libs. The extra lib over shooter_demo is psynder_world_bsp
# (the BSP map / portal set / PVS builder).
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(psynder_duke_demo ${SRC})
target_link_libraries(psynder_duke_demo PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_render_rt psynder_render_post psynder_render_hybrid
    psynder_scene psynder_physics psynder_asset psynder_platform
    psynder_gameplay psynder_ai
    psynder_world_bsp
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)

# ─── Offline BSP asset bake (W9-1) ──────────────────────────────────────────
# Author the indoor level as an on-disk `.rooms` source (assets/maps/) and
# compile it OFFLINE at build time with the lm_qbsp tool into an engine-format
# `.psybsp` (PBSP v1: leaves + clusters + portals + a BAKED leaf-portal-flood
# PVS) that the runtime loader `world::bsp::Bsp::load` consumes. This proves the
# full BSP asset pipeline (source -> qbsp -> .psybsp -> runtime load + PVS cull)
# end to end, replacing the demo's old in-memory BspMap authoring.
#
# Bake-at-build (not checked in): the .psybsp is a derived binary, so we follow
# the repo's established asset-bake convention (tools/lm_pak cooks fixtures into
# ${CMAKE_BINARY_DIR}/assets) and emit it under the build tree. The exe mounts
# that directory at startup via the DUKE_BSP_ASSET_DIR compile definition, with
# a source-tree fallback so a hand-run lm_qbsp also works.
set(_duke_rooms_src "${CMAKE_SOURCE_DIR}/assets/maps/duke_e1m1.rooms")
set(_duke_bsp_dir   "${CMAKE_BINARY_DIR}/assets/maps")
set(_duke_bsp_out   "${_duke_bsp_dir}/duke_e1m1.psybsp")

add_custom_command(
    OUTPUT  "${_duke_bsp_out}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_duke_bsp_dir}"
    COMMAND $<TARGET_FILE:lm_qbsp> --rooms "${_duke_rooms_src}" "${_duke_bsp_out}"
    DEPENDS lm_qbsp "${_duke_rooms_src}"
    COMMENT "Baking duke_e1m1.rooms -> ${_duke_bsp_out} (engine PBSP v1 + PVS)"
    VERBATIM
)
add_custom_target(psynder_duke_bsp ALL DEPENDS "${_duke_bsp_out}")
add_dependencies(psynder_duke_demo psynder_duke_bsp)

target_compile_definitions(psynder_duke_demo PRIVATE
    DUKE_BSP_ASSET_DIR="${CMAKE_BINARY_DIR}/assets"
    DUKE_BSP_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets"
    DUKE_BSP_VPATH="maps/duke_e1m1.psybsp"
)
