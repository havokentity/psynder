# SPDX-License-Identifier: MIT
# World aggregator — adds bsp + outdoor sublanes.
add_subdirectory(bsp)
add_subdirectory(outdoor)

add_library(psynder_world INTERFACE)
target_link_libraries(psynder_world INTERFACE psynder_world_bsp psynder_world_outdoor)
add_library(psynder::world ALIAS psynder_world)
