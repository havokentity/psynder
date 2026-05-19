// SPDX-License-Identifier: MIT
// Psynder — SIMD runtime dispatch. Tier selection runs once at startup;
// the chosen Tier feeds the batched-routine slots.

#include "Dispatch.h"
#include "Simd_internal.h"

#include "core/hardware/CpuFeatures.h"

#include <atomic>

namespace psynder::simd {

namespace {
std::atomic<Tier> g_tier{Tier::Scalar};
std::atomic<bool> g_init{false};

Tier pick_tier(const psynder::hardware::CpuFeatures& f) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    // NEON is baseline on aarch64 — even if the detect probe somehow says
    // otherwise, the compiled binary already requires it.
    (void)f;
    return Tier::Neon;
#elif defined(__x86_64__) || defined(_M_X64)
    if (f.avx512f) return Tier::Avx512;
    if (f.avx2 && f.fma) return Tier::Avx2;
    if (f.sse42) return Tier::Sse42;
    return Tier::Scalar;
#else
    (void)f;
    return Tier::Scalar;
#endif
}
}  // namespace

void dispatch_init() noexcept {
    if (g_init.load(std::memory_order_acquire)) return;
    const auto& f = psynder::hardware::detect();
    g_tier.store(pick_tier(f), std::memory_order_release);
    g_init.store(true, std::memory_order_release);
}

Tier current_tier() noexcept {
    if (!g_init.load(std::memory_order_acquire)) dispatch_init();
    return g_tier.load(std::memory_order_acquire);
}

const char* tier_name(Tier t) noexcept {
    switch (t) {
        case Tier::Scalar: return "scalar";
        case Tier::Sse42:  return "sse4.2";
        case Tier::Avx2:   return "avx2+fma";
        case Tier::Avx512: return "avx-512";
        case Tier::Neon:   return "neon";
    }
    return "?";
}

// ─── Batched kernels ─────────────────────────────────────────────────────
// Each routine has one widest-path implementation backed by Simd_internal,
// plus a tail loop for the unaligned remainder. We branch on the runtime
// tier so that on a host with only SSE4.2 we don't try to issue AVX2 ops
// (which would crash with #UD anyway since the binary wouldn't carry them).
// On Apple Silicon there's no widening — the f32x4 path is fastest. On x86
// we always use the widest path the compiler was built with that's also
// supported at runtime; the build flags pin this in lockstep.

psynder::f32 reduce_add(const psynder::f32* p, psynder::usize n) noexcept {
    psynder::usize i = 0;
    psynder::f32   acc = 0.0f;

#if defined(__AVX__)
    if (current_tier() == Tier::Avx512 || current_tier() == Tier::Avx2) {
        f32x8 v_acc = broadcast8(0.0f);
        for (; i + 8 <= n; i += 8) v_acc = add8(v_acc, load_unaligned8(p + i));
        acc += reduce_add8(v_acc);
    }
#endif
    {
        f32x4 v_acc = broadcast4(0.0f);
        for (; i + 4 <= n; i += 4) v_acc = add4(v_acc, load_unaligned4(p + i));
        acc += reduce_add4(v_acc);
    }
    for (; i < n; ++i) acc += p[i];
    return acc;
}

void add_buffer(const psynder::f32* a, const psynder::f32* b,
                psynder::f32* y, psynder::usize n) noexcept {
    psynder::usize i = 0;
#if defined(__AVX__)
    if (current_tier() == Tier::Avx512 || current_tier() == Tier::Avx2) {
        for (; i + 8 <= n; i += 8) {
            store_unaligned8(y + i, add8(load_unaligned8(a + i),
                                         load_unaligned8(b + i)));
        }
    }
#endif
    for (; i + 4 <= n; i += 4) {
        store_unaligned4(y + i, add4(load_unaligned4(a + i),
                                     load_unaligned4(b + i)));
    }
    for (; i < n; ++i) y[i] = a[i] + b[i];
}

void fma_buffer(const psynder::f32* a, const psynder::f32* b,
                const psynder::f32* c, psynder::f32* y,
                psynder::usize n) noexcept {
    psynder::usize i = 0;
#if defined(__AVX__)
    if (current_tier() == Tier::Avx512 || current_tier() == Tier::Avx2) {
        for (; i + 8 <= n; i += 8) {
            store_unaligned8(y + i, fma8(load_unaligned8(a + i),
                                         load_unaligned8(b + i),
                                         load_unaligned8(c + i)));
        }
    }
#endif
    for (; i + 4 <= n; i += 4) {
        store_unaligned4(y + i, fma4(load_unaligned4(a + i),
                                     load_unaligned4(b + i),
                                     load_unaligned4(c + i)));
    }
    for (; i < n; ++i) y[i] = a[i] * b[i] + c[i];
}

// ─── Static initialiser ──────────────────────────────────────────────────
// __attribute__((constructor)) is supported by Clang and GCC. On MSVC the
// same effect is achieved by a function-local static in a dummy global —
// not needed yet because Psynder uses Clang on every supported platform.
namespace {
#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor)) void simd_dispatch_ctor() { dispatch_init(); }
#endif
}  // namespace

}  // namespace psynder::simd
