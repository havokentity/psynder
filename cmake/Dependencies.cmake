# SPDX-License-Identifier: MIT
# Psynder — third-party dependency wiring.
# Prefers vcpkg manifest mode; falls back to FetchContent when vcpkg
# is not in play (e.g. fresh local dev box without VCPKG_ROOT set).

include(FetchContent)

set(FETCHCONTENT_QUIET FALSE)

# ─── fmt — used by Log / Diag / Console (ripped from dmonte) ──────────────
find_package(fmt CONFIG QUIET)
if(NOT fmt_FOUND)
    message(STATUS "[deps] fmt: fetching via FetchContent")
    # 11.1.x fixes an Apple-Clang consteval issue triggered by C++23 builds
    # (https://github.com/fmtlib/fmt/issues/3849)
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG        11.1.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(fmt)
endif()

# ─── zstd — .lmpak compression ────────────────────────────────────────────
find_package(zstd CONFIG QUIET)
if(NOT zstd_FOUND)
    # Most distros + vcpkg ship zstd::libzstd_static; FetchContent fallback
    # is large, so we just defer to the asset lane to set it up when needed.
    message(STATUS "[deps] zstd: not found via find_package; asset lane will FetchContent on demand")
endif()

# ─── Lua 5.4 ──────────────────────────────────────────────────────────────
find_package(Lua 5.4 QUIET)
if(NOT Lua_FOUND)
    message(STATUS "[deps] Lua 5.4: not found; script lane will FetchContent on demand")
endif()

# ─── Catch2 — tests ───────────────────────────────────────────────────────
if(PSYNDER_BUILD_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        message(STATUS "[deps] Catch2: fetching via FetchContent")
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.7.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    endif()
endif()
