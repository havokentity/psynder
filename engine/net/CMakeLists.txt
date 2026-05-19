# SPDX-License-Identifier: MIT
# psynder_net — Lane 14. Owns: reliable UDP, lockstep, snapshot interpolation.
psynder_add_lane(net LINKS psynder_core psynder_math psynder_jobs)
if(PSYNDER_PLATFORM_WIN32)
    target_link_libraries(psynder_net PUBLIC ws2_32)
endif()
