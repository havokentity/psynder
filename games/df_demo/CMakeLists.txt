# SPDX-License-Identifier: MIT
# psynder_df_demo - DEMO GAME 3. A standalone Delta Force-style OUTDOOR tactical
# shooter that composes the whole engine stack on heightmap terrain: the
# sample-06 procedural heightfield brought into the scene as a grid mesh
# (engine/editor/world LevelSource), a terrain-grounded first-person
# CharacterController player, AI soldiers that perceive/chase/shoot across long
# sightlines (engine/ai), hitscan combat (engine/gameplay), a terrain heightmap
# march + physics raycast for LOS (engine/render/rt + engine/physics), and the
# hybrid render path (raster primary visibility + traced shadow rays, terrain
# heightmap shadow occluder + directional sun).
#
# Standalone exe modelled on games/shooter_demo: it reuses the shared
# samples::CharacterController + the app::WindowApp host boilerplate, and links
# ONLY public engine libs. The extra libs over shooter_demo are
# psynder_editor_world (the terrain LevelSource loader + demo heightmap
# generator) and psynder_world_outdoor (the HeightmapDesc type).
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(psynder_df_demo ${SRC})
target_link_libraries(psynder_df_demo PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_render_rt psynder_render_post psynder_render_hybrid
    psynder_scene psynder_physics psynder_asset psynder_platform
    psynder_gameplay psynder_ai
    psynder_world_outdoor psynder_editor_world
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)
