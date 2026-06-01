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
    # Pinned to the immutable commit SHA (not the mutable tag) for supply-chain
    # reproducibility. GIT_SHALLOW is FALSE because a shallow fetch of an
    # arbitrary commit is not portable across all git servers/CMake versions.
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG        123913715afeb8a437e6388b4473fcc4753e1c9a  # tag 11.1.4
        GIT_SHALLOW    FALSE
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
            GIT_TAG        fa43b77429ba76c462b1898d6cd2f2d7a9416b14  # tag v3.7.1 (immutable SHA pin)
            GIT_SHALLOW    FALSE
        )
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    endif()
endif()
