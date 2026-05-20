# SPDX-License-Identifier: MIT
# Tests — Lane 25. Unit / golden / bench harnesses.
add_subdirectory(unit)
add_subdirectory(golden)
add_subdirectory(bench)

# ─── Sample smoke tests ──────────────────────────────────────────────────
# Registered here because the root CMakeLists.txt only calls enable_testing()
# AFTER add_subdirectory(samples), so any add_test() calls inside sample
# subdirs are silently dropped. We register them in tests/ instead — same
# net effect, and the sample targets are visible by name globally.
#
# All sample smokes share a single RESOURCE_LOCK so ctest -j N never runs
# two of them at once on the same Mac runner (see AGENTS.md mac-serialization).
if(PSYNDER_BUILD_SAMPLES)
    if(TARGET sample_00_clear)
        add_test(NAME sample_00_clear_smoke
                 COMMAND sample_00_clear --smoke-frames=4)
        set_tests_properties(sample_00_clear_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
    if(TARGET sample_01_triangle)
        add_test(NAME sample_01_triangle_smoke
                 COMMAND sample_01_triangle --smoke-frames=4)
        set_tests_properties(sample_01_triangle_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
    if(TARGET sample_02_textured_quad)
        add_test(NAME sample_02_textured_quad_smoke
                 COMMAND sample_02_textured_quad --smoke-frames=4)
        set_tests_properties(sample_02_textured_quad_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
    if(TARGET sample_07_phys_stacks)
        add_test(NAME sample_07_phys_stacks_smoke
                 COMMAND sample_07_phys_stacks --smoke-frames=4)
        set_tests_properties(sample_07_phys_stacks_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
    if(TARGET sample_08_phys_constraints)
        add_test(NAME sample_08_phys_constraints_smoke
                 COMMAND sample_08_phys_constraints --smoke-frames=4)
        set_tests_properties(sample_08_phys_constraints_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
    if(TARGET sample_09_char_playground)
        add_test(NAME sample_09_char_playground_smoke
                 COMMAND sample_09_char_playground --smoke-frames=4)
        set_tests_properties(sample_09_char_playground_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
    if(TARGET sample_10_shape_sandbox)
        add_test(NAME sample_10_shape_sandbox_smoke
                 COMMAND sample_10_shape_sandbox --smoke-frames=4)
        set_tests_properties(sample_10_shape_sandbox_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
    if(TARGET sample_12_rt_showcase)
        add_test(NAME sample_12_rt_showcase_smoke
                 COMMAND sample_12_rt_showcase --smoke-frames=4)
        set_tests_properties(sample_12_rt_showcase_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
    if(TARGET sample_13_rt_quake)
        add_test(NAME sample_13_rt_quake_smoke
                 COMMAND sample_13_rt_quake --smoke-frames=4)
        set_tests_properties(sample_13_rt_quake_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
    if(TARGET sample_11_rt_spheres)
        add_test(NAME sample_11_rt_spheres_smoke
                 COMMAND sample_11_rt_spheres --smoke-frames=4)
        set_tests_properties(sample_11_rt_spheres_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
    if(TARGET sample_14_lightmap_quake)
        add_test(NAME sample_14_lightmap_quake_smoke
                 COMMAND sample_14_lightmap_quake --smoke-frames=4)
        set_tests_properties(sample_14_lightmap_quake_smoke PROPERTIES
            RESOURCE_LOCK "psynder_platform_singleton"
            TIMEOUT 30)
    endif()
endif()
