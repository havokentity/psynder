# SPDX-License-Identifier: MIT
# Psynder — cooked scene asset build helpers.

function(psynder_target_psyscene_assets target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "psynder_target_psyscene_assets: unknown target '${target}'")
    endif()
    if(NOT TARGET scene_cook)
        message(FATAL_ERROR
            "psynder_target_psyscene_assets: scene_cook target is unavailable; "
            "build tools are required to cook .psyscene.json assets")
    endif()

    set(_sources ${ARGN})
    if(_sources STREQUAL "")
        file(GLOB _sources
            CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.psyscene.json"
        )
    endif()

    if(_sources STREQUAL "")
        return()
    endif()

    set(_outputs)
    foreach(_src IN LISTS _sources)
        if(NOT IS_ABSOLUTE "${_src}")
            set(_src_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        else()
            set(_src_abs "${_src}")
        endif()

        get_filename_component(_name "${_src_abs}" NAME)
        if(NOT _name MATCHES "\\.psyscene\\.json$")
            message(FATAL_ERROR
                "psynder_target_psyscene_assets: '${_name}' is not a .psyscene.json file")
        endif()
        string(REGEX REPLACE "\\.json$" "" _cooked_name "${_name}")
        set(_out "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/${_cooked_name}")

        add_custom_command(
            OUTPUT "${_out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets"
            COMMAND $<TARGET_FILE:scene_cook> "${_src_abs}" "${_out}"
            DEPENDS scene_cook "${_src_abs}"
            COMMENT "Cooking PsyScene ${_name}"
            VERBATIM
        )
        list(APPEND _outputs "${_out}")
    endforeach()

    set(_asset_target "${target}_psyscene_assets")
    add_custom_target(${_asset_target} DEPENDS ${_outputs})
    add_dependencies(${target} ${_asset_target})
endfunction()
