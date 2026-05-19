# SPDX-License-Identifier: MIT
# psynder_platform_linux — Lane 22. Owns:
#   - Wayland xdg-shell + wp_viewporter present (DESIGN.md §11.2)
#   - X11 fallback via XShm + XRender
#   - evdev input
#   - PipeWire-first audio with ALSA fallback (dlopened — see LinuxAudio.cpp)
#
# This subdirectory is only added on PSYNDER_PLATFORM_LINUX (see
# engine/platform/CMakeLists.txt). All translation units here are also
# wrapped in `#ifdef PSYNDER_PLATFORM_LINUX` defensively so a sloppy
# build configuration on Mac / Windows still degrades to a no-op.

psynder_add_lane(platform_linux LINKS psynder_core psynder_math psynder_render_raster)

if(NOT PSYNDER_PLATFORM_LINUX)
    return()
endif()

find_package(PkgConfig REQUIRED)

# ─── Required: wayland-client, wayland-protocols, wayland-scanner ───────
pkg_check_modules(WL_CLIENT    REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WL_PROTOCOLS REQUIRED                 wayland-protocols)
pkg_check_modules(WL_SCANNER   REQUIRED                 wayland-scanner)
pkg_get_variable(WL_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(WL_SCANNER_BIN   wayland-scanner   wayland_scanner)

pkg_check_modules(XKBCOMMON  REQUIRED IMPORTED_TARGET xkbcommon)

# ─── X11 fallback: xlib + xext (MIT-SHM) + xrender ──────────────────────
pkg_check_modules(X11      REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XEXT     REQUIRED IMPORTED_TARGET xext)
pkg_check_modules(XRENDER  REQUIRED IMPORTED_TARGET xrender)

# ─── Code generator for the protocol-specific headers / private code ────
# We need: xdg-shell, viewporter, linux-dmabuf-v1 (detected at runtime,
# but its protocol XML is part of wayland-protocols).
set(_psy_wl_proto_dir "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
file(MAKE_DIRECTORY "${_psy_wl_proto_dir}")

function(psy_wl_gen proto_name proto_xml_relpath)
    set(_xml "${WL_PROTOCOLS_DIR}/${proto_xml_relpath}")
    set(_hdr "${_psy_wl_proto_dir}/${proto_name}-client-protocol.h")
    set(_src "${_psy_wl_proto_dir}/${proto_name}-client-protocol.c")
    add_custom_command(
        OUTPUT  "${_hdr}"
        COMMAND "${WL_SCANNER_BIN}" client-header "${_xml}" "${_hdr}"
        DEPENDS "${_xml}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT  "${_src}"
        COMMAND "${WL_SCANNER_BIN}" private-code "${_xml}" "${_src}"
        DEPENDS "${_xml}"
        VERBATIM
    )
    target_sources(psynder_platform_linux PRIVATE "${_hdr}" "${_src}")
endfunction()

psy_wl_gen(xdg-shell        "stable/xdg-shell/xdg-shell.xml")
psy_wl_gen(viewporter       "stable/viewporter/viewporter.xml")
psy_wl_gen(linux-dmabuf-v1  "stable/linux-dmabuf/linux-dmabuf-v1.xml")

target_include_directories(psynder_platform_linux PRIVATE "${_psy_wl_proto_dir}")

target_link_libraries(psynder_platform_linux PUBLIC
    PkgConfig::WL_CLIENT
    PkgConfig::XKBCOMMON
    PkgConfig::X11
    PkgConfig::XEXT
    PkgConfig::XRENDER
)

# dl needed for dlopen() of libpipewire-0.3 / libasound — we don't link
# either at compile time, so the Linux build remains buildable on hosts
# that have only one of the two installed.
target_link_libraries(psynder_platform_linux PUBLIC ${CMAKE_DL_LIBS})

# rt for shm_open / clock_gettime on some glibc versions.
find_library(_psy_rt rt)
if(_psy_rt)
    target_link_libraries(psynder_platform_linux PUBLIC ${_psy_rt})
endif()
