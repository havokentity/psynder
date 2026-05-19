# SPDX-License-Identifier: MIT
# psynder_simd — Lane 03. Owns: f32x4/x8, i32x4/x8, masks; SSE/AVX2/NEON kernels; runtime dispatch.
psynder_add_lane(simd LINKS psynder_math)

# SIMD-specific compile flags
if(PSYNDER_ARCH_X64 AND NOT MSVC)
    target_compile_options(psynder_simd PUBLIC -msse4.2)
endif()
if(PSYNDER_ARCH_ARM64 AND NOT MSVC)
    # NEON is implicit on aarch64
endif()
