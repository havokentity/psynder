# SPDX-License-Identifier: MIT
# psynder_ui_rml — Lane 17. Owns: RmlUi integration, RenderInterface impl,
# FreeType glyphs, Lua handler binding.

# ─── Build the lane static library via the project helper ────────────────
psynder_add_lane(ui_rml
    LINKS
        psynder_core
        psynder_math
        psynder_render_raster
        psynder_asset
)
# psynder_script is intentionally not linked: lane 15 ships only a public
# header in Wave-A.  The Lua handler binding installs a function-pointer
# hook (see LuaBinding.cpp + detail::set_lua_backend) that lane 15 or the
# upstream RmlUi Lua plugin can fill in without a hard build-time dep.

# ─── Optional: vendor RmlUi + FreeType via FetchContent ──────────────────
#
# Default OFF.  Wave-A ships an in-tree RML/RCSS subset implementation so
# every host platform builds green without a multi-megabyte fetch.  When
# the integration branch is ready to validate against the upstream library,
# flip this option (`-DPSYNDER_VENDOR_RMLUI=ON`).  The public API
# (`engine/ui/rml/Rml.h`) is unchanged either way.
option(PSYNDER_VENDOR_RMLUI
    "Vendor RmlUi + FreeType via FetchContent under third_party/. Lane 17 only."
    OFF
)

if(PSYNDER_VENDOR_RMLUI)
    include(FetchContent)

    # FreeType first — RmlUi's default font engine depends on it.
    find_package(Freetype QUIET)
    if(NOT Freetype_FOUND)
        message(STATUS "[deps] FreeType: fetching via FetchContent")
        FetchContent_Declare(freetype
            GIT_REPOSITORY https://github.com/freetype/freetype.git
            GIT_TAG        VER-2-13-3
            GIT_SHALLOW    TRUE
            SOURCE_DIR     ${CMAKE_SOURCE_DIR}/third_party/freetype
        )
        set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(freetype)
    endif()

    # RmlUi.  Disable samples + tests; we wire our own RenderInterface.
    message(STATUS "[deps] RmlUi: fetching via FetchContent")
    FetchContent_Declare(rmlui
        GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
        GIT_TAG        6.2
        GIT_SHALLOW    TRUE
        SOURCE_DIR     ${CMAKE_SOURCE_DIR}/third_party/rmlui
    )
    set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS         OFF CACHE BOOL "" FORCE)
    set(RMLUI_SAMPLES             OFF CACHE BOOL "" FORCE)
    set(RMLUI_BACKEND             "none" CACHE STRING "" FORCE)
    set(RMLUI_FONT_ENGINE         "freetype" CACHE STRING "" FORCE)
    set(RMLUI_LUA_BINDINGS        OFF CACHE BOOL "" FORCE)
    set(RMLUI_LOTTIE_PLUGIN       OFF CACHE BOOL "" FORCE)
    set(RMLUI_SVG_PLUGIN          OFF CACHE BOOL "" FORCE)
    set(RMLUI_HARFBUZZ_SAMPLE     OFF CACHE BOOL "" FORCE)
    set(RMLUI_THIRDPARTY_CONTAINERS ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(rmlui)
    set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

    target_compile_definitions(psynder_ui_rml PUBLIC PSYNDER_HAS_RMLUI=1)
    target_link_libraries(psynder_ui_rml PUBLIC RmlCore)
endif()

# ─── Hook our test sources into the shared psynder_unit binary ───────────
#
# tests/unit/CMakeLists.txt is owned by lane 25; we extend its link line
# from this lane's own file by registering a DEFERred call on the project
# root directory.  The call fires after every subdir has been processed
# (psynder_unit exists by then) so we never edit a file outside lane 17
# ownership.  Guarded so the unit binary may be absent in trimmed builds.
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL
    psynder_ui_rml_link_into_unit_tests
)

function(psynder_ui_rml_link_into_unit_tests)
    if(TARGET psynder_unit AND TARGET psynder_ui_rml)
        target_link_libraries(psynder_unit PRIVATE psynder_ui_rml)
    endif()
endfunction()
