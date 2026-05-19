# SPDX-License-Identifier: MIT
# psynder_platform_win32 — Lane 21. Owns: Win32 window, DXGI present, WASAPI audio, XInput.
psynder_add_lane(platform_win32 LINKS psynder_core psynder_math psynder_render_raster)
if(PSYNDER_PLATFORM_WIN32)
    target_link_libraries(psynder_platform_win32 PUBLIC
        user32 gdi32 shell32 ole32 dxgi d3d11 d3dcompiler d2d1 dwrite winmm xinput
    )
    # Many WinSDKs ship D3DCompile only via the optional d3dcompiler_47 redist;
    # link the import lib that exists in every default toolchain.
    target_compile_definitions(psynder_platform_win32 PRIVATE
        UNICODE _UNICODE
    )
endif()
