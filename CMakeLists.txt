# SPDX-License-Identifier: MIT
# Psynder — Cache, Scanline, Renderer (PCSR).
# Root CMakeLists. The 25 lane subdirectories are added in dependency order
# per DESIGN.md §19.5. Each subdir owns its own CMakeLists.txt; this file is
# owned by the build-system maintainer (root) and orchestrates dependencies.

cmake_minimum_required(VERSION 3.28)

# ─── Project ──────────────────────────────────────────────────────────────
project(psynder
    VERSION     0.1.0
    DESCRIPTION "Pure-CPU game engine with software rasterizer and software raytracer"
    LANGUAGES   C CXX
)

# ─── Project-level options ────────────────────────────────────────────────
option(PSYNDER_EDITOR                "Build editor / sandbox mode + IPC server + React panels"  ON)
option(PSYNDER_BUILD_PLAYER          "Build the generic cooked-game player executable"          ON)
option(PSYNDER_BUILD_TOOLS           "Build offline tools (lm_pak, lm_cook, lm_qbsp, lm_bake)"   ON)
option(PSYNDER_BUILD_SAMPLES         "Build sample programs under samples/"                     ON)
option(PSYNDER_BUILD_TESTS           "Build unit / golden / bench tests under tests/"           ON)
option(PSYNDER_ENABLE_TRACY          "Enable Tracy profiler integration (debug builds default)" OFF)
option(PSYNDER_ENABLE_SANITIZERS     "Enable ASan + UBSan (debug Linux/Mac only)"               OFF)
option(PSYNDER_ENABLE_AVX2           "Build x86-64 AVX2 fast-path kernels"                       ON)
option(PSYNDER_ENABLE_AVX512         "Build x86-64 AVX-512 wide-path kernels (opportunistic)"   OFF)
option(PSYNDER_ENABLE_LTO            "Link-time optimization for release builds"                ON)

# ─── C++23, no exceptions / no RTTI in hot subsystems (per §3 DOTS rules) ─
set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)

# Psynder does not use C++20 modules today. CMake 3.28+ may otherwise add a
# per-TU module dependency scan edge under Ninja/MSVC, which bloats CI builds.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Always-on per-target compile features
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS   ON)

# Place all binaries under build/${preset}/bin so samples + tests find them
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ─── Module path ──────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ─── Compiler warnings & sanitizers ───────────────────────────────────────
include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)
include(Lanes)
include(Goldens)
include(PsySceneAssets)

# ─── Platform detection helpers ───────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PSYNDER_PLATFORM_MACOS  TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PSYNDER_PLATFORM_WIN32  TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PSYNDER_PLATFORM_LINUX  TRUE)
else()
    message(FATAL_ERROR "Psynder targets Win/Linux/macOS only; unsupported: ${CMAKE_SYSTEM_NAME}")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(PSYNDER_ARCH_ARM64 TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(PSYNDER_ARCH_X64   TRUE)
else()
    message(WARNING "Untested CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

message(STATUS "Psynder target: ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Psynder editor mode: ${PSYNDER_EDITOR}")

# ─── psynder_common interface target — propagates settings to every lib ──
add_library(psynder_common INTERFACE)
target_compile_features(psynder_common INTERFACE cxx_std_23)
psynder_set_warnings(psynder_common)
target_include_directories(psynder_common INTERFACE
    ${CMAKE_SOURCE_DIR}/engine
)
if(PSYNDER_EDITOR)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_EDITOR=1)
endif()
if(PSYNDER_PLATFORM_MACOS)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_PLATFORM_MACOS=1)
endif()
if(PSYNDER_PLATFORM_WIN32)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_PLATFORM_WIN32=1)
endif()
if(PSYNDER_PLATFORM_LINUX)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_PLATFORM_LINUX=1)
endif()
if(PSYNDER_ARCH_X64)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_ARCH_X64=1)
endif()
if(PSYNDER_ARCH_ARM64)
    target_compile_definitions(psynder_common INTERFACE PSYNDER_ARCH_ARM64=1)
endif()

# ─── Engine subdirs — strict dependency order per DESIGN.md §19.5 ────────
# core → math → simd → jobs → asset → scene → render → audio
#                                              ↑
#                                          physics
#                                              ↑
#                                            ui (imm + rml)
#                                              ↑
#                                            editor
add_subdirectory(engine/core)
add_subdirectory(engine/math)
add_subdirectory(engine/simd)
add_subdirectory(engine/jobs)
add_subdirectory(engine/asset)
add_subdirectory(engine/scene)
add_subdirectory(engine/render)
add_subdirectory(engine/world)
add_subdirectory(engine/physics)
add_subdirectory(engine/audio)
add_subdirectory(engine/net)
add_subdirectory(engine/script)
add_subdirectory(engine/ui)
add_subdirectory(engine/platform)
if(PSYNDER_EDITOR)
    add_subdirectory(engine/editor)
endif()
if(PSYNDER_BUILD_PLAYER)
    add_subdirectory(player)
endif()

# ─── Tools / samples / tests ──────────────────────────────────────────────
if(PSYNDER_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(PSYNDER_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()
if(PSYNDER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Source-visible asset precook declarations keep sample/app CMake boilerplate
# out of leaf projects.
psynder_autocook_declared_assets()

# ─── Summary ──────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "── Psynder build summary ──────────────────────────────")
message(STATUS "  Editor mode:     ${PSYNDER_EDITOR}")
message(STATUS "  Player:          ${PSYNDER_BUILD_PLAYER}")
message(STATUS "  Tools:           ${PSYNDER_BUILD_TOOLS}")
message(STATUS "  Samples:         ${PSYNDER_BUILD_SAMPLES}")
message(STATUS "  Tests:           ${PSYNDER_BUILD_TESTS}")
message(STATUS "  Tracy:           ${PSYNDER_ENABLE_TRACY}")
message(STATUS "  Sanitizers:      ${PSYNDER_ENABLE_SANITIZERS}")
message(STATUS "  LTO:             ${PSYNDER_ENABLE_LTO}")
message(STATUS "──────────────────────────────────────────────────────")
message(STATUS "")
