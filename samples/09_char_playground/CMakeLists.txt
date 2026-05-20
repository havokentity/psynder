# SPDX-License-Identifier: MIT
# Sample 09 — kinematic capsule "obstacle playground". A capsule character
# walks a course of static physics boxes (flat ground, a walkable ramp, a
# steep slide-off wedge, a 4-step staircase, and a ledge), driven through the
# public physics::character controller (sweep-step-slide). Mouse-look comes
# from samples_common's CharacterController; movement (step-up + slope-slide)
# is owned by the engine character. See main.cpp. Lane 25 owns the target.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_09_char_playground ${SRC})
target_link_libraries(sample_09_char_playground PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster
    psynder_scene psynder_physics psynder_platform
    psynder_samples_common
)
