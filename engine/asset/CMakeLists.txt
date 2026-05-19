# SPDX-License-Identifier: MIT
# psynder_asset — Lane 05. Owns: VFS, .lmpak archive reader, hot-reload, cooker formats.
psynder_add_lane(asset LINKS psynder_core psynder_jobs)

# ─── zstd — optional, enables .lmpak compressed-entry support ────────────
# The root Dependencies.cmake already issued find_package(zstd CONFIG QUIET).
# Prefer the static target; fall back to shared; if neither is present the
# reader compiles without compression (uncompressed entries still work).
if(TARGET zstd::libzstd_static)
    target_link_libraries(psynder_asset PRIVATE zstd::libzstd_static)
    target_include_directories(psynder_asset PRIVATE
        $<TARGET_PROPERTY:zstd::libzstd_static,INTERFACE_INCLUDE_DIRECTORIES>)
elseif(TARGET zstd::libzstd_shared)
    target_link_libraries(psynder_asset PRIVATE zstd::libzstd_shared)
    target_include_directories(psynder_asset PRIVATE
        $<TARGET_PROPERTY:zstd::libzstd_shared,INTERFACE_INCLUDE_DIRECTORIES>)
elseif(TARGET zstd::libzstd)
    target_link_libraries(psynder_asset PRIVATE zstd::libzstd)
    target_include_directories(psynder_asset PRIVATE
        $<TARGET_PROPERTY:zstd::libzstd,INTERFACE_INCLUDE_DIRECTORIES>)
else()
    message(STATUS "[asset] zstd not found; .lmpak compressed entries will be unavailable")
endif()
