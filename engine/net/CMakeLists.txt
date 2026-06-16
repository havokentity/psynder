# SPDX-License-Identifier: MIT
# psynder_net - Lane 14. Owns: reliable UDP, lockstep, snapshot interpolation,
# ECS entity replication (M-NET). Links psynder_scene for the ECS registry +
# components the replication layer gathers/applies. scene does NOT depend on
# net, so this is acyclic.
psynder_add_lane(net LINKS psynder_core psynder_math psynder_jobs psynder_scene)
if(PSYNDER_PLATFORM_WIN32)
    target_link_libraries(psynder_net PUBLIC ws2_32)
endif()
