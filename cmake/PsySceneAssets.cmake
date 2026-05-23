# SPDX-License-Identifier: MIT
# Psynder — cooked scene asset build helpers.

set(_PSYNDER_PRECOOK_DEFAULT_FOLDER "__PSYNDER_DEFAULT__")

function(_psynder_validate_runtime_output_folder target folder)
    if("${folder}" STREQUAL "")
        message(FATAL_ERROR
            "psynder_autocook_declared_assets: '${target}' has an empty output folder")
    endif()
    if(IS_ABSOLUTE "${folder}" OR "${folder}" MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR
            "psynder_autocook_declared_assets: '${target}' has invalid output folder '${folder}'")
    endif()
endfunction()

function(_psynder_apply_runtime_output_folder target output_folder)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "_psynder_apply_runtime_output_folder: unknown target '${target}'")
    endif()

    _psynder_validate_runtime_output_folder(${target} "${output_folder}")
    if(CMAKE_CONFIGURATION_TYPES)
        set(_runtime_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/${output_folder}")
    else()
        set(_runtime_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${output_folder}")
    endif()
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_runtime_dir}")
    set(${ARGV2} "${_runtime_dir}" PARENT_SCOPE)
endfunction()

function(_psynder_psyscene_dependency_files out src_abs)
    set(_deps "${src_abs}")
    if(EXISTS "${src_abs}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src_abs}")
        file(READ "${src_abs}" _content)
        string(REGEX MATCHALL
            "\"[^\"]+(\\.psyscript|\\.psygraph\\.json|\\.psybehavior\\.json)\""
            _behavior_refs
            "${_content}"
        )
        if(_behavior_refs)
            get_filename_component(_src_dir "${src_abs}" DIRECTORY)
            foreach(_ref IN LISTS _behavior_refs)
                string(REGEX REPLACE "^\"|\"$" "" _behavior "${_ref}")
                if(IS_ABSOLUTE "${_behavior}")
                    set(_behavior_abs "${_behavior}")
                else()
                    get_filename_component(_behavior_abs "${_src_dir}/${_behavior}" ABSOLUTE)
                endif()
                list(APPEND _deps "${_behavior_abs}")
                if(EXISTS "${_behavior_abs}")
                    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_behavior_abs}")
                endif()
            endforeach()
        endif()
    endif()
    list(REMOVE_DUPLICATES _deps)
    set(${out} ${_deps} PARENT_SCOPE)
endfunction()

function(_psynder_target_psyscene_assets target output_folder)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "_psynder_target_psyscene_assets: unknown target '${target}'")
    endif()
    if(NOT TARGET scene_cook)
        message(FATAL_ERROR
            "_psynder_target_psyscene_assets: scene_cook target is unavailable; "
            "build tools are required to cook .psyscene.json assets")
    endif()

    set(_sources ${ARGN})
    if(_sources STREQUAL "")
        return()
    endif()

    _psynder_apply_runtime_output_folder(${target} "${output_folder}" _runtime_dir)
    set(_asset_dir "${_runtime_dir}/assets")

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
                "_psynder_target_psyscene_assets: '${_name}' is not a .psyscene.json file")
        endif()
        string(REGEX REPLACE "\\.json$" "" _cooked_name "${_name}")
        set(_out "${_asset_dir}/${_cooked_name}")
        _psynder_psyscene_dependency_files(_deps "${_src_abs}")

        add_custom_command(
            OUTPUT "${_out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_asset_dir}"
            COMMAND $<TARGET_FILE:scene_cook> "${_src_abs}" "${_out}"
            DEPENDS scene_cook ${_deps}
            COMMENT "Cooking PsyScene ${_name}"
            VERBATIM
        )
        list(APPEND _outputs "${_out}")
    endforeach()

    set(_asset_target "${target}_psyscene_assets")
    if(TARGET ${_asset_target})
        return()
    endif()
    add_custom_target(${_asset_target} DEPENDS ${_outputs})
    add_dependencies(${target} ${_asset_target})
endfunction()

function(_psynder_scan_runtime_bundle_source out source_dir source)
    set(_folders)
    _psynder_source_abs(_source_abs "${source_dir}" "${source}")
    if(NOT EXISTS "${_source_abs}")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source_abs}")

    get_filename_component(_ext "${_source_abs}" EXT)
    string(TOLOWER "${_ext}" _ext)
    if(NOT _ext MATCHES "^\\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()

    file(READ "${_source_abs}" _content)
    string(REGEX MATCHALL
        "PSYNDER_RUNTIME_BUNDLE[ \t\r\n]*\\([ \t\r\n]*\"[^\"]+\""
        _matches
        "${_content}"
    )
    foreach(_match IN LISTS _matches)
        string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" _folder "${_match}")
        list(APPEND _folders "${_folder}")
    endforeach()

    set(${out} ${_folders} PARENT_SCOPE)
endfunction()

function(_psynder_collect_build_dirs out dir)
    set(_dirs "${dir}")
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        _psynder_collect_build_dirs(_child_dirs "${_subdir}")
        list(APPEND _dirs ${_child_dirs})
    endforeach()
    set(${out} ${_dirs} PARENT_SCOPE)
endfunction()

function(_psynder_source_abs out target_dir source)
    if(IS_ABSOLUTE "${source}")
        set(${out} "${source}" PARENT_SCOPE)
        return()
    endif()
    get_filename_component(_abs "${target_dir}/${source}" ABSOLUTE)
    set(${out} "${_abs}" PARENT_SCOPE)
endfunction()

function(_psynder_scan_precook_psyscene_source out source_dir source)
    set(_records)
    _psynder_source_abs(_source_abs "${source_dir}" "${source}")
    if(NOT EXISTS "${_source_abs}")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source_abs}")

    get_filename_component(_ext "${_source_abs}" EXT)
    string(TOLOWER "${_ext}" _ext)
    if(NOT _ext MATCHES "^\\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()

    file(READ "${_source_abs}" _content)
    string(REGEX MATCHALL
        "PSYNDER_PRECOOK_PSYSCENE[ \t\r\n]*\\([ \t\r\n]*\"[^\"]+\""
        _matches
        "${_content}"
    )
    foreach(_match IN LISTS _matches)
        string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" _asset "${_match}")
        get_filename_component(_asset_abs "${source_dir}/${_asset}" ABSOLUTE)
        list(APPEND _records "${_PSYNDER_PRECOOK_DEFAULT_FOLDER}|||${_asset_abs}")
    endforeach()

    set(${out} ${_records} PARENT_SCOPE)
endfunction()

function(psynder_autocook_declared_assets)
    _psynder_collect_build_dirs(_dirs "${CMAKE_BINARY_DIR}")
    foreach(_dir IN LISTS _dirs)
        get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        if(_targets STREQUAL "")
            continue()
        endif()

        get_property(_source_dir DIRECTORY "${_dir}" PROPERTY SOURCE_DIR)
        foreach(_target IN LISTS _targets)
            if(_target STREQUAL "scene_cook")
                continue()
            endif()

            get_target_property(_type ${_target} TYPE)
            if(NOT _type STREQUAL "EXECUTABLE")
                continue()
            endif()

            get_target_property(_sources ${_target} SOURCES)
            if(_sources STREQUAL "" OR _sources STREQUAL "_sources-NOTFOUND")
                continue()
            endif()

            set(_records)
            set(_bundle_folders)
            foreach(_source IN LISTS _sources)
                if("${_source}" MATCHES "^\\$<")
                    continue()
                endif()
                unset(_source_bundle_folders)
                _psynder_scan_runtime_bundle_source(
                    _source_bundle_folders "${_source_dir}" "${_source}")
                if(NOT _source_bundle_folders STREQUAL "")
                    list(APPEND _bundle_folders ${_source_bundle_folders})
                endif()

                unset(_source_records)
                _psynder_scan_precook_psyscene_source(_source_records "${_source_dir}" "${_source}")
                if(NOT _source_records STREQUAL "")
                    list(APPEND _records ${_source_records})
                endif()
            endforeach()

            set(_explicit_output_folder "")
            if(_bundle_folders)
                list(REMOVE_DUPLICATES _bundle_folders)
                foreach(_folder IN LISTS _bundle_folders)
                    if(_explicit_output_folder STREQUAL "")
                        set(_explicit_output_folder "${_folder}")
                    elseif(NOT "${_explicit_output_folder}" STREQUAL "${_folder}")
                        message(FATAL_ERROR
                            "psynder_autocook_declared_assets: '${_target}' declares multiple "
                            "runtime bundle folders ('${_explicit_output_folder}' and '${_folder}')")
                    endif()
                endforeach()
                _psynder_apply_runtime_output_folder(${_target} "${_explicit_output_folder}" _unused)
            endif()

            if(_records)
                list(REMOVE_DUPLICATES _records)
                set(_output_folder "")
                set(_assets)
                foreach(_record IN LISTS _records)
                    string(REPLACE "|||" ";" _parts "${_record}")
                    list(GET _parts 0 _folder)
                    list(GET _parts 1 _asset)
                    if(_asset STREQUAL "")
                        message(FATAL_ERROR
                            "psynder_autocook_declared_assets: '${_target}' has an empty PsyScene asset")
                    endif()
                    if(_folder STREQUAL "${_PSYNDER_PRECOOK_DEFAULT_FOLDER}")
                        if(_explicit_output_folder STREQUAL "")
                            set(_folder "${_target}")
                        else()
                            set(_folder "${_explicit_output_folder}")
                        endif()
                    elseif(_folder STREQUAL "")
                        set(_folder "${_target}")
                    endif()
                    if(NOT _explicit_output_folder STREQUAL "" AND
                       NOT "${_explicit_output_folder}" STREQUAL "${_folder}")
                        message(FATAL_ERROR
                            "psynder_autocook_declared_assets: '${_target}' runtime bundle "
                            "'${_explicit_output_folder}' conflicts with PsyScene output "
                            "folder '${_folder}'")
                    endif()
                    if(_output_folder STREQUAL "")
                        set(_output_folder "${_folder}")
                    elseif(NOT "${_output_folder}" STREQUAL "${_folder}")
                        message(FATAL_ERROR
                            "psynder_autocook_declared_assets: '${_target}' declares multiple "
                            "PsyScene output folders ('${_output_folder}' and '${_folder}')")
                    endif()
                    list(APPEND _assets "${_asset}")
                endforeach()
                _psynder_target_psyscene_assets(${_target} "${_output_folder}" ${_assets})
            endif()
        endforeach()
    endforeach()
endfunction()
