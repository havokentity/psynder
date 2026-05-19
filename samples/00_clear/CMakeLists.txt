# SPDX-License-Identifier: MIT
# Sample 00 — M0 demo. Open a window, animate the clear color, blit each frame.
# Smoke add_test() lives in tests/CMakeLists.txt because the root project only
# calls enable_testing() after add_subdirectory(samples).
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
add_executable(sample_00_clear ${SRC})
target_link_libraries(sample_00_clear PRIVATE
    psynder_core psynder_math psynder_render_raster psynder_platform
    psynder_samples_common
)
