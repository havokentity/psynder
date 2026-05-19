// SPDX-License-Identifier: MIT
// Psynder — Lane 03 Wave-B helper tests. Covers the three header-only
// additions that don't have explicit kernel surfaces of their own:
//
//   1. Prefetch wrappers (prefetch_t0/t1/t2/nta + prefetch_range) must
//      not crash on null or on a zero-length range — production call
//      sites issue them defensively before the bounds check.
//   2. `stream_store_f32x4` must produce a byte-identical buffer to a
//      regular `store_unaligned4` once `stream_fence` has flushed.
//   3. `has_avx512f()` must return a sensible bool — true only on x86,
//      and only when the runtime detector confirms it.
//
// These tests stay deliberately structural: the kernels exercised here
// are tiny and any cross-platform behavioural drift would manifest
// elsewhere (the bench / dispatcher tests). Goal: prove the public
// helpers compile, link, and don't trap on the obvious edge inputs.

#include "simd/Avx512.h"
#include "simd/Prefetch.h"
#include "simd/Simd.h"
#include "simd/Simd_internal.h"
#include "simd/Streaming.h"

#include "core/hardware/CpuFeatures.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>

using namespace psynder;

TEST_CASE("prefetch helpers do not crash on null or empty inputs",
          "[simd][prefetch]") {
    // All four hint tiers should be safe to call on a null pointer —
    // x86 / aarch64 specs treat the address as a hint, not a load.
    simd::prefetch_t0(nullptr);
    simd::prefetch_t1(nullptr);
    simd::prefetch_t2(nullptr);
    simd::prefetch_nta(nullptr);

    // Range walker on null / zero / zero-stride is a no-op (the guard
    // inside prefetch_range short-circuits before touching memory).
    simd::prefetch_range(nullptr, 4096);
    simd::prefetch_range(nullptr, 0);

    alignas(64) std::array<u8, 256> buf{};
    simd::prefetch_range(buf.data(), 0);
    simd::prefetch_range(buf.data(), buf.size(), 0);

    // Non-empty walk over real memory: must finish, must touch every
    // cache line on the way past. (We can't observe the prefetch from
    // user space; we just confirm the call returns.)
    simd::prefetch_range(buf.data(), buf.size());
    simd::prefetch_range(buf.data(), buf.size(), 32);   // sub-line stride
    REQUIRE(true);                                       // reached the end
}

TEST_CASE("stream_store_f32x4 produces the same bytes as store_unaligned4",
          "[simd][stream]") {
    // 16-byte alignment is mandatory for _mm_stream_ps on x86; the same
    // alignment is harmless on NEON. Match the contract here.
    alignas(16) std::array<f32, 4> stream_dst{};
    alignas(16) std::array<f32, 4> cached_dst{};

    const std::array<f32, 4> src{1.25f, -2.5f, 4.0f, 0.0f};
    const auto               v = simd::load_unaligned4(src.data());

    simd::stream_store_f32x4(stream_dst.data(), v);
    simd::stream_fence();                                // make NT store visible

    simd::store_unaligned4(cached_dst.data(), v);

    // Byte-identical: NT and temporal stores of the same vector must
    // write the same value pattern.
    REQUIRE(std::memcmp(stream_dst.data(), cached_dst.data(),
                        sizeof(stream_dst)) == 0);
    for (size_t i = 0; i < 4; ++i) REQUIRE(stream_dst[i] == src[i]);
}

TEST_CASE("has_avx512f returns a sensible bool", "[simd][avx512]") {
    const bool reported = simd::has_avx512f();
    const auto& cpu     = hardware::detect();

#if defined(__x86_64__) || defined(_M_X64)
    // On x86 the probe must echo the detector exactly.
    REQUIRE(reported == cpu.avx512f);
#else
    // On aarch64 / Apple Silicon, AVX-512 cannot exist — the probe must
    // unconditionally return false.
    (void)cpu;
    REQUIRE(reported == false);
#endif
}
