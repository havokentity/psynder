# SPDX-License-Identifier: MIT
# psynder_mp_demo - DEMO GAME 5 (the M-NET reference). A pure-headless
# networked-multiplayer demo: one authoritative server + two clients sharing a
# single replicated world, in one process, over the in-process LoopbackBus.
#
# It composes the whole netcode stack (engine/net): reliable-UDP HostImpl,
# AOI-gated snapshot+delta replication, client prediction + server
# reconciliation, and snapshot interpolation. No window / renderer - it links
# only the minimal libs the net + ECS path needs:
#   psynder_net    - the transport + replication + prediction layers,
#   psynder_scene  - the ECS registry + Transform/SceneNode components,
#   plus core/math/jobs (transitive deps of net + scene).
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(psynder_mp_demo ${SRC})
target_link_libraries(psynder_mp_demo PRIVATE
    psynder_core psynder_math psynder_jobs
    psynder_scene psynder_net
)
