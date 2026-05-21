# SPDX-License-Identifier: MIT
# UI aggregator — adds imm + console + rml sublanes.
add_subdirectory(imm)
add_subdirectory(console)
add_subdirectory(rml)

add_library(psynder_ui INTERFACE)
target_link_libraries(psynder_ui INTERFACE psynder_ui_imm psynder_ui_console psynder_ui_rml)
add_library(psynder::ui ALIAS psynder_ui)
