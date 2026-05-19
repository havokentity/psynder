// SPDX-License-Identifier: MIT
// Psynder physics — floating-point determinism helpers (DESIGN.md §10.1).
//
// Physics determinism requires: (a) no fast-math reassociation, wired via the
// per-TU compile flags in engine/physics/CMakeLists.txt, AND (b) the FPU
// pinned to round-to-nearest-even with denormals enabled. Lane 13 internal.
//
// We pin the FPU control word at the start of every physics step rather than
// once at startup, because the host application or third-party DLLs (audio
// codecs are notorious offenders on Windows) can mutate it underneath us.

#pragma once

#include "core/Types.h"

#if defined(__x86_64__) || defined(_M_X64)
#   include <xmmintrin.h>
#   include <pmmintrin.h>
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#   include <fenv.h>
#endif

namespace psynder::physics::detail {

// RAII guard that pins the host FPU to round-to-nearest-even with FTZ/DAZ
// *off* (denormals preserved — we'd rather take the perf hit on the rare
// denormal than lose determinism). Restores the previous control word on
// destruction so we don't leak our state to game code.
class FpGuard {
public:
    PSY_FORCEINLINE FpGuard() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        prev_mxcsr_ = _mm_getcsr();
        // Round-to-nearest (bits 13:14 = 00), exceptions masked, FTZ off, DAZ off
        u32 csr = prev_mxcsr_;
        csr &= ~(0x6000u);                  // clear rounding-control bits
        csr &= ~(0x8040u);                  // clear FTZ + DAZ
        csr |=  (0x1F80u);                  // mask all exceptions
        _mm_setcsr(csr);
#elif defined(__aarch64__) || defined(_M_ARM64)
        // arm64 FPCR: RMode bits 23:22 = 00 (round-to-nearest), FZ bit 24 = 0
        u64 fpcr;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
        prev_fpcr_ = fpcr;
        fpcr &= ~((u64{3} << 22) | (u64{1} << 24));
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
    }

    PSY_FORCEINLINE ~FpGuard() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_setcsr(prev_mxcsr_);
#elif defined(__aarch64__) || defined(_M_ARM64)
        __asm__ __volatile__("msr fpcr, %0" : : "r"(prev_fpcr_));
#endif
    }

    FpGuard(const FpGuard&) = delete;
    FpGuard& operator=(const FpGuard&) = delete;

private:
#if defined(__x86_64__) || defined(_M_X64)
    u32 prev_mxcsr_ = 0;
#elif defined(__aarch64__) || defined(_M_ARM64)
    u64 prev_fpcr_  = 0;
#endif
};

}  // namespace psynder::physics::detail
