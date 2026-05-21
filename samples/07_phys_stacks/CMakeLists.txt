# SPDX-License-Identifier: MIT
# Sample 07 — rigid-body "stacks & restitution" showcase. A static ground box,
# a pyramid of dynamic boxes (SAP broadphase + PGS contact solver + friction),
# and a few high-restitution spheres dropped from height that bounce and settle.
# See main.cpp. Lane 25 (samples-tests) owns the target.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_07_phys_stacks ${SRC})
target_link_libraries(sample_07_phys_stacks PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster
    psynder_scene psynder_physics psynder_platform
    psynder_editor_core
    psynder_samples_common
)
