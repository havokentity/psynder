# SPDX-License-Identifier: MIT
# psynder_ui_console — software drop-down developer console overlay.
# Renders with the lane-16 immediate-mode primitives (psynder_ui_imm), drives
# the engine console backend (psynder_core / psynder::console), and calls the
# platform display-control helpers (request_fullscreen / request_window_size /
# set_text_input_capturing) which live in the platform shim.
#
# We link the FULL psynder_platform aggregate (shim + host backend), NOT just
# the shim: the shim's create_window() references create_window_impl() from the
# backend, so pulling the shim alone leaves an undefined reference on GNU ld's
# single-pass linker (Mac/MSVC tolerate the order; Linux does not). Depending
# on the aggregate makes CMake order the backend after the shim so it resolves.
psynder_add_lane(ui_console LINKS psynder_core psynder_ui_imm psynder_render_raster
                 psynder_platform)
