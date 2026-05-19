# SPDX-License-Identifier: MIT
# Platform aggregator — adds Platform.cpp + the host's platform lane.

# Shared shim — the dispatcher + Clock impl
add_library(psynder_platform_shim STATIC Platform.cpp Platform.h)
target_link_libraries(psynder_platform_shim PUBLIC psynder_core psynder_math)
target_include_directories(psynder_platform_shim PUBLIC ${CMAKE_SOURCE_DIR}/engine)

if(PSYNDER_PLATFORM_WIN32)
    add_subdirectory(win32)
    add_library(psynder_platform INTERFACE)
    target_link_libraries(psynder_platform INTERFACE psynder_platform_shim psynder_platform_win32)
elseif(PSYNDER_PLATFORM_LINUX)
    add_subdirectory(linux)
    add_library(psynder_platform INTERFACE)
    target_link_libraries(psynder_platform INTERFACE psynder_platform_shim psynder_platform_linux)
elseif(PSYNDER_PLATFORM_MACOS)
    add_subdirectory(macos)
    add_library(psynder_platform INTERFACE)
    target_link_libraries(psynder_platform INTERFACE psynder_platform_shim psynder_platform_macos)
endif()
add_library(psynder::platform ALIAS psynder_platform)
