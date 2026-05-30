# SPDX-License-Identifier: MIT
# Games — standalone playable demo executables that compose the whole engine
# stack (level + hybrid render + physics + combat + AI). One subdirectory per
# game. Gated by PSYNDER_BUILD_GAMES (declared in the root CMakeLists, mirroring
# PSYNDER_BUILD_SAMPLES) so the games never force-build everywhere.
add_subdirectory(shooter_demo)
add_subdirectory(racer_demo)
add_subdirectory(df_demo)
