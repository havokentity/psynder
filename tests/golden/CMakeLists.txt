# SPDX-License-Identifier: MIT
# Golden-image renderer tests. Compare CPU rasterizer output vs reference
# images (PPM on disk — see runner.cpp for rationale). Tolerance: 0.5%
# per-pixel mismatch, per DESIGN.md §14.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(NOT SRC)
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(psynder_golden ${SRC})
target_link_libraries(psynder_golden PRIVATE
    psynder_core psynder_math psynder_render_raster psynder_render_rt psynder_render_post
)

# Point the runner at the in-tree cases + references. The runner also has a
# cwd-walking fallback, so this is just an optimization.
add_test(NAME psynder_golden
         COMMAND psynder_golden
                 --cases-dir=${CMAKE_CURRENT_SOURCE_DIR}/cases
                 --references-dir=${CMAKE_CURRENT_SOURCE_DIR}/references
                 --tolerance-pct=0.5)
set_tests_properties(psynder_golden PROPERTIES
    TIMEOUT 60
)

# ─── Wave-B golden cells (per-sample frame capture) ──────────────────────
# These cells drive the sample binaries with --smoke-frames + a deterministic
# frame-indexed clock, capture the final framebuffer as PNG, and (when an
# imgdiff target ships from a downstream lane) compare against a committed
# PNG baseline under tests/golden/${CMAKE_SYSTEM_NAME}/. Today no `imgdiff`
# target exists, so psynder_add_golden_cell() registers only the _render
# half; the cell still validates that every sample can produce a capture-
# mode PNG end-to-end, which is the half lane 25 owns.
#
# The cells share a RESOURCE_LOCK with the existing sample smokes
# (tests/CMakeLists.txt → psynder_platform_singleton) so ctest -j never
# tries to open two windows simultaneously on the shared Mac runner — see
# AGENTS.md's mac-serialization note.
if(PSYNDER_BUILD_SAMPLES)
    psynder_add_golden_cell(SCENE clear      BACKEND sw FRAMES 4)
    psynder_add_golden_cell(SCENE triangle   BACKEND sw FRAMES 4)
    psynder_add_golden_cell(SCENE crate_room BACKEND sw FRAMES 4)

    # Serialize windowed renders on the Mac runner.
    foreach(_cell golden_clear__sw_render
                  golden_triangle__sw_render
                  golden_crate_room__sw_render)
        if(TEST ${_cell})
            set_tests_properties(${_cell} PROPERTIES
                RESOURCE_LOCK "psynder_platform_singleton")
        endif()
    endforeach()
endif()
