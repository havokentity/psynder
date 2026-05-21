# SPDX-License-Identifier: MIT
# psynder_ui_console — software drop-down developer console overlay.
# Renders with the lane-16 immediate-mode primitives (psynder_ui_imm) and
# drives the engine console backend (psynder_core / psynder::console). Takes
# input through the abstract platform::Input interface (header only — no link
# against the platform lane, so this stays a leaf UI lane with no cycle).
psynder_add_lane(ui_console LINKS psynder_core psynder_ui_imm psynder_render_raster
                 psynder_platform_shim)
