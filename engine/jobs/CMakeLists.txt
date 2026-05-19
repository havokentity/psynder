# SPDX-License-Identifier: MIT
# psynder_jobs — Lane 04. Owns: Chase-Lev work-stealing deque, job graph, fibers.
psynder_add_lane(jobs LINKS psynder_core psynder_simd)
if(NOT MSVC)
    target_link_libraries(psynder_jobs PUBLIC pthread)
endif()
