# SPDX-License-Identifier: MIT
# psynder_platform_macos — Lane 23. Owns: AppKit window, CAMetalLayer scanout, CoreAudio, GameController.
psynder_add_lane(platform_macos LINKS psynder_core psynder_math psynder_render_raster)
if(PSYNDER_PLATFORM_MACOS)
    target_link_libraries(psynder_platform_macos PUBLIC
        "-framework Cocoa"
        "-framework AppKit"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework MetalKit"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework GameController"
        "-framework IOKit"
        "-framework IOSurface"
        "-framework ForceFeedback"
    )
endif()
