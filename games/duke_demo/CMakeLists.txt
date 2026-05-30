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
