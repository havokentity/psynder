# SPDX-License-Identifier: MIT
# Sample 10 — mixed-shape rigid-body sandbox + camera physgun. A static ground
# box, an assortment of dynamic boxes / spheres / capsules / a convex hull
# dropped from height (SAP broadphase + GJK/EPA narrowphase + PGS contact
# solver), plus a camera-aimed physgun that grabs the nearest body and flings
# it via the physics body-mutation writers. See main.cpp.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_10_shape_sandbox ${SRC})
target_link_libraries(sample_10_shape_sandbox PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster
    psynder_scene psynder_physics psynder_platform
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)
