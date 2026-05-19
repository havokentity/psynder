# SPDX-License-Identifier: MIT
# psynder_simd — Lane 03. Owns: f32x4/x8, i32x4/x8, masks; SSE/AVX2/NEON
# kernels; runtime dispatch.
psynder_add_lane(simd LINKS psynder_math psynder_core)

# ─── SIMD compile flags ──────────────────────────────────────────────────
# We compile ONE binary per target. The frozen Simd.h decides intrinsic
# shape via __x86_64__ / __aarch64__; the build flags here turn on AVX2
# and AVX-512 when the user wants the fast/wide paths baked in. The
# runtime dispatcher (Dispatch.cpp) still consults CpuFeatures::detect()
# before issuing any tier-specific routine — so a binary built with
# -mavx2 will still happily fall back to the SSE4.2 lane on a Westmere
# host. (The compile-time path picks the *widest* the binary may issue;
# the runtime tier picks the *widest* the host actually supports.)

if(PSYNDER_ARCH_X64 AND NOT MSVC)
    # SSE4.2 baseline — already in the bootstrap, but make it explicit
    # so a future toolchain default change doesn't silently drop us.
    target_compile_options(psynder_simd PUBLIC -msse4.2)

    if(PSYNDER_ENABLE_AVX2)
        # AVX2 + FMA fast path. These define __AVX__, __AVX2__, __FMA__
        # which Simd_internal.h keys off.
        target_compile_options(psynder_simd PUBLIC -mavx2 -mfma)
    endif()

    if(PSYNDER_ENABLE_AVX512)
        # AVX-512F is the minimum we lean on; BW/VL are nice-to-have for
        # the bit-mask / lane-select extensions. -mavx512f defines the
        # __AVX512F__ macro used to gate f32x16 in the internal header.
        target_compile_options(psynder_simd PUBLIC
            -mavx512f -mavx512bw -mavx512vl
        )
    endif()
elseif(PSYNDER_ARCH_X64 AND MSVC)
    # MSVC variants: /arch:SSE4.2 isn't a thing (SSE2 is the baseline);
    # /arch:AVX2 implies FMA; /arch:AVX512 (VS2019 16.3+) lights up the
    # AVX-512 path. The first match wins so order matters.
    if(PSYNDER_ENABLE_AVX512)
        target_compile_options(psynder_simd PUBLIC /arch:AVX512)
    elseif(PSYNDER_ENABLE_AVX2)
        target_compile_options(psynder_simd PUBLIC /arch:AVX2)
    endif()
endif()

if(PSYNDER_ARCH_ARM64 AND NOT MSVC)
    # NEON is implicit on aarch64; nothing to set. Apple Silicon has no
    # 256-bit widening, so f32x8 stays as two-halves on this arch — the
    # internal header handles that automatically.
endif()
