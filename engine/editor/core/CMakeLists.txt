# SPDX-License-Identifier: MIT
# psynder_editor_core — Lane 18. Owns: mode toggle, brush CSG, sculpt, physgun,
# constraints, undo/redo, .psylevel / .psyc serialisation, REPL host.
#
# Linkage: scene (entity records), physics (body handle types), script (REPL),
# ui_imm (in-viewport gizmos), ui_console (sample_step drives the drop-down
# console — propagated PUBLIC so every sample that links editor_core gets it
# without a per-sample CMake edit), asset (VFS read fall-through for load_*).
psynder_add_lane(editor_core
    LINKS
        psynder_core
        psynder_math
        psynder_asset
        psynder_scene
        psynder_physics
        psynder_script
        psynder_ui_imm
        psynder_ui_console
)

# Optional zstd compression for .psylevel / .psyc bodies. The SaveLoadApi TU
# detects the header at compile time; if zstd isn't on the include path, the
# files are written raw with the kFlagZstd bit cleared.
if(TARGET zstd::libzstd_static)
    target_link_libraries(psynder_editor_core PRIVATE zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
    target_link_libraries(psynder_editor_core PRIVATE zstd::libzstd_shared)
elseif(TARGET zstd::libzstd)
    target_link_libraries(psynder_editor_core PRIVATE zstd::libzstd)
endif()
