# SPDX-License-Identifier: MIT
# psynder_core — Lane 01. Owns: allocators, console, log, diag, hardware, RNG, Tracy hookup.

# ─── Optional: vendor Tracy via FetchContent ─────────────────────────────
# Tracy.h is header-only and key off PSYNDER_ENABLE_TRACY; when off, every
# zone macro collapses to do{}while(0). When the option is on (debug /
# relwithdebinfo presets), we fetch the upstream and link it into the lane.
# We deliberately fetch under engine/core's scope so the rest of the
# engine doesn't have to know about the dependency: any TU that #includes
# "core/Tracy.h" implicitly inherits the dependency through psynder_core.
if(PSYNDER_ENABLE_TRACY)
    include(FetchContent)
    if(NOT TARGET Tracy::TracyClient)
        message(STATUS "[deps] Tracy: fetching via FetchContent")
        # v0.11.1 is the most recent stable tag known to build clean
        # against C++23 on clang/MSVC/apple-clang. Tracy_INSTALL=OFF
        # keeps it out of any `cmake --install`.
        set(TRACY_ENABLE        ON  CACHE BOOL "" FORCE)
        set(TRACY_ON_DEMAND     ON  CACHE BOOL "" FORCE)
        set(TRACY_NO_BROADCAST  OFF CACHE BOOL "" FORCE)
        set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)
        set(TRACY_INSTALL       OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(tracy
            GIT_REPOSITORY https://github.com/wolfpld/tracy.git
            GIT_TAG        v0.11.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(tracy)
    endif()
endif()

psynder_add_lane(core LINKS fmt::fmt)

# Tracy linkage + propagate the enable define. The PUBLIC propagation is
# what lets every downstream lane just #include "core/Tracy.h" and have
# the macros expand correctly when the option is on -- no per-lane
# CMake plumbing required.
if(PSYNDER_ENABLE_TRACY)
    target_compile_definitions(psynder_core PUBLIC PSYNDER_ENABLE_TRACY=1)
    if(TARGET Tracy::TracyClient)
        target_link_libraries(psynder_core PUBLIC Tracy::TracyClient)
    elseif(TARGET TracyClient)
        target_link_libraries(psynder_core PUBLIC TracyClient)
    endif()
endif()
