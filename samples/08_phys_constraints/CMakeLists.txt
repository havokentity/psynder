# SPDX-License-Identifier: MIT
# Sample 08 — constraints / ragdoll demo. A static anchor, a swinging
# rope+elastic chain, and a jointed ragdoll (weld / ball-socket / slider /
# axis). Authors the joint graph via psynder_editor_core's constraints API
# and drives it with a deterministic in-sample PBD solver.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_08_phys_constraints ${SRC})
target_link_libraries(sample_08_phys_constraints PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_scene psynder_physics psynder_platform
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)
