# SPDX-License-Identifier: MIT
# psynder_psygraph — PsyGraph visual-scripting core (M-PSYGRAPH).
#
# A standalone, dependency-light static library: a node-graph data model that
# compiles to a flat pooled bytecode VM. It depends only on psynder_core (for
# the shared scalar types) and psynder_scene (for the thin, optional ECS
# binding). Gameplay / render / audio are reached only through host hooks, so
# this library never links those subsystems.
#
# This file is additively included by engine/script/CMakeLists.txt.

file(GLOB PSYGRAPH_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(psynder_psygraph STATIC ${PSYGRAPH_SOURCES})
add_library(psynder::psygraph ALIAS psynder_psygraph)

target_link_libraries(psynder_psygraph PUBLIC
    psynder_common
    psynder_core
    psynder_scene
)

# Lane libraries see the engine root so headers include via "<lane>/Header.h".
target_include_directories(psynder_psygraph PUBLIC ${CMAKE_SOURCE_DIR}/engine)

set_target_properties(psynder_psygraph PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)
