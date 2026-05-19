# SPDX-License-Identifier: MIT
# Sample 04 — M4 demo. NFS-style chase-cam lap of an oval test track with
# vehicle physics (Pacejka tires, 6-speed gearbox). See main.cpp for the
# scene + driver. Lane 25 (samples-tests) owns the target.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_04_nfs_track ${SRC})
target_link_libraries(sample_04_nfs_track PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster
    psynder_world_outdoor psynder_scene psynder_physics
    psynder_asset psynder_audio psynder_platform
    psynder_samples_common
)
