# SPDX-License-Identifier: MIT
# psynder_platform_linux — Lane 22. Owns: Wayland xdg-shell present, X11 fallback, evdev, PipeWire/ALSA.
psynder_add_lane(platform_linux LINKS psynder_core psynder_math psynder_render_raster)
if(PSYNDER_PLATFORM_LINUX)
    target_link_libraries(psynder_platform_linux PUBLIC
        wayland-client xkbcommon
    )
endif()
