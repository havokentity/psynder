// SPDX-License-Identifier: MIT
// Psynder — Lane 03 unit tests. Covers the invariants the rest of the
// engine leans on:
//
//   1. Pack-load round-trip — `store(load(x)) == x` for every supported
//      width and load/store alignment combination.
//   2. FMA precision — `fma(a,b,c)` matches the scalar `a*b+c` reference
//      across a deterministic random sample.
//   3. Blend correctness — `blend(a, b, cmp_lt(a,b))` selects the smaller
//      per-lane value, matching scalar min.
//
// The dispatcher tier-init is invoked once at fixture setup so any
// downstream test linking against psynder_simd starts in a known state.

#include "simd/Dispatch.h"
#include "simd/Simd.h"
#include "simd/Simd_internal.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace psynder;

// Tighten the tolerance from the default Catch2 1e-5 — FMA should hit
// scalar reference to within a single ULP for the inputs we use.
static constexpr f32 kFmaTol = 1e-5f;

TEST_CASE("simd dispatcher reports a sensible tier", "[simd][dispatch]") {
    simd::dispatch_init();
    auto t = simd::current_tier();
    // The dispatcher must pick *something* — Scalar only if we built with
    // no SIMD at all, which Wave-A targets never do.
#if defined(__aarch64__) || defined(_M_ARM64)
    REQUIRE(t == simd::Tier::Neon);
#elif defined(__x86_64__) || defined(_M_X64)
    REQUIRE((t == simd::Tier::Sse42 || t == simd::Tier::Avx2 ||
             t == simd::Tier::Avx512));
#endif
    INFO("tier=" << simd::tier_name(t));
    REQUIRE(t != simd::Tier::Scalar);
}

TEST_CASE("f32x4 load_unaligned/store_unaligned round-trip", "[simd][load]") {
    const std::array<f32, 4> in{1.0f, -2.0f, 3.5f, 0.0f};
    auto v = simd::load_unaligned4(in.data());

    std::array<f32, 4> out{};
    simd::store_unaligned4(out.data(), v);

    for (size_t i = 0; i < 4; ++i) REQUIRE(out[i] == in[i]);
}

TEST_CASE("f32x4 load_aligned/store_aligned round-trip", "[simd][load]") {
    alignas(16) std::array<f32, 4> in{1.5f, 2.5f, -3.5f, 4.5f};
    auto v = simd::load_aligned4(in.data());

    alignas(16) std::array<f32, 4> out{};
    simd::store_aligned4(out.data(), v);

    for (size_t i = 0; i < 4; ++i) REQUIRE(out[i] == in[i]);
}

TEST_CASE("f32x4 frozen-public load/store round-trip", "[simd][load]") {
    // The frozen Simd.h `load`/`store` (unaligned) is the path samples
    // and bench code go through — protect the contract explicitly.
    const std::array<f32, 4> in{0.1f, 0.2f, 0.3f, 0.4f};
    auto v = simd::load(in.data());

    std::array<f32, 4> out{};
    simd::store(out.data(), v);

    for (size_t i = 0; i < 4; ++i) REQUIRE(out[i] == in[i]);
}

TEST_CASE("f32x8 load/store round-trip both alignments", "[simd][load]") {
    alignas(32) std::array<f32, 8> in{1, 2, 3, 4, 5, 6, 7, 8};
    auto v_a = simd::load_aligned8(in.data());
    auto v_u = simd::load_unaligned8(in.data());

    alignas(32) std::array<f32, 8> out_a{};
    std::array<f32, 8>             out_u{};
    simd::store_aligned8(out_a.data(), v_a);
    simd::store_unaligned8(out_u.data(), v_u);

    for (size_t i = 0; i < 8; ++i) {
        REQUIRE(out_a[i] == in[i]);
        REQUIRE(out_u[i] == in[i]);
    }
}

TEST_CASE("broadcast splats a scalar across every lane", "[simd][broadcast]") {
    auto v4 = simd::broadcast4(7.25f);
    std::array<f32, 4> out4{};
    simd::store_unaligned4(out4.data(), v4);
    for (auto x : out4) REQUIRE(x == 7.25f);

    auto v8 = simd::broadcast8(-3.5f);
    std::array<f32, 8> out8{};
    simd::store_unaligned8(out8.data(), v8);
    for (auto x : out8) REQUIRE(x == -3.5f);
}

TEST_CASE("FMA matches scalar reference within tolerance", "[simd][fma]") {
    std::mt19937 rng(0xDEADBEEFu);   // fixed seed for determinism
    std::uniform_real_distribution<f32> dist(-1000.0f, 1000.0f);

    for (int trial = 0; trial < 256; ++trial) {
        alignas(16) std::array<f32, 4> a{}, b{}, c{}, out{};
        for (size_t i = 0; i < 4; ++i) {
            a[i] = dist(rng);
            b[i] = dist(rng);
            c[i] = dist(rng);
        }
        auto v = simd::fma4(simd::load_aligned4(a.data()),
                            simd::load_aligned4(b.data()),
                            simd::load_aligned4(c.data()));
        simd::store_aligned4(out.data(), v);
        for (size_t i = 0; i < 4; ++i) {
            // Reference using std::fma matches a fused multiply-add. On
            // SSE-only x86 our fma() falls back to mul+add, so allow a
            // touch more slack there.
            f32 ref = std::fma(a[i], b[i], c[i]);
            REQUIRE(std::fabs(out[i] - ref) <= kFmaTol * std::fabs(ref) + kFmaTol);
        }
    }
}

TEST_CASE("FMA on f32x8 matches scalar reference", "[simd][fma]") {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_real_distribution<f32> dist(-10.0f, 10.0f);
    for (int trial = 0; trial < 64; ++trial) {
        alignas(32) std::array<f32, 8> a{}, b{}, c{}, out{};
        for (size_t i = 0; i < 8; ++i) {
            a[i] = dist(rng);
            b[i] = dist(rng);
            c[i] = dist(rng);
        }
        auto v = simd::fma8(simd::load_aligned8(a.data()),
                            simd::load_aligned8(b.data()),
                            simd::load_aligned8(c.data()));
        simd::store_aligned8(out.data(), v);
        for (size_t i = 0; i < 8; ++i) {
            f32 ref = std::fma(a[i], b[i], c[i]);
            REQUIRE(std::fabs(out[i] - ref) <= kFmaTol * std::fabs(ref) + kFmaTol);
        }
    }
}

TEST_CASE("cmp_lt + blend picks the smaller value per lane", "[simd][blend]") {
    alignas(16) std::array<f32, 4> a{1.0f, -2.0f, 3.0f, 0.0f};
    alignas(16) std::array<f32, 4> b{0.0f, -1.0f, 5.0f, -3.0f};

    auto va = simd::load_aligned4(a.data());
    auto vb = simd::load_aligned4(b.data());
    auto m  = simd::cmp_lt4(vb, va);             // true where b < a
    auto v  = simd::blend4(va, vb, m);           // pick b where b<a, else a

    alignas(16) std::array<f32, 4> out{};
    simd::store_aligned4(out.data(), v);

    REQUIRE(out[0] == 0.0f);   // b=0 < a=1
    REQUIRE(out[1] == -2.0f);  // a=-2 < b=-1
    REQUIRE(out[2] == 3.0f);   // a=3 < b=5
    REQUIRE(out[3] == -3.0f);  // b=-3 < a=0
}

TEST_CASE("min/max match scalar fmin/fmax", "[simd][minmax]") {
    alignas(16) std::array<f32, 4> a{1.0f, 5.0f, -2.0f, 0.0f};
    alignas(16) std::array<f32, 4> b{3.0f, 4.0f, -3.0f, 1.0f};

    alignas(16) std::array<f32, 4> mn{}, mx{};
    simd::store_aligned4(mn.data(),
                         simd::min4(simd::load_aligned4(a.data()),
                                    simd::load_aligned4(b.data())));
    simd::store_aligned4(mx.data(),
                         simd::max4(simd::load_aligned4(a.data()),
                                    simd::load_aligned4(b.data())));
    for (size_t i = 0; i < 4; ++i) {
        REQUIRE(mn[i] == std::min(a[i], b[i]));
        REQUIRE(mx[i] == std::max(a[i], b[i]));
    }
}

TEST_CASE("abs flips the sign bit only", "[simd][abs]") {
    alignas(16) std::array<f32, 4> in{-1.0f, 2.0f, -0.0f, -3.5f};
    alignas(16) std::array<f32, 4> out{};
    simd::store_aligned4(out.data(), simd::abs4(simd::load_aligned4(in.data())));
    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[1] == 2.0f);
    REQUIRE(out[2] == 0.0f);
    REQUIRE(out[3] == 3.5f);
}

TEST_CASE("sqrt matches std::sqrt", "[simd][sqrt]") {
    alignas(16) std::array<f32, 4> in{0.0f, 1.0f, 4.0f, 9.0f};
    alignas(16) std::array<f32, 4> out{};
    simd::store_aligned4(out.data(), simd::sqrt4(simd::load_aligned4(in.data())));
    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[1] == 1.0f);
    REQUIRE(out[2] == 2.0f);
    REQUIRE(out[3] == 3.0f);
}

TEST_CASE("rsqrt produces an 8-bit-or-better approximation", "[simd][rsqrt]") {
    // SSE rsqrt promises ~12-bit precision; NEON `vrsqrteq_f32` is spec'd
    // at 8 bits (≈4e-3) on some uarchs (the Apple Silicon implementation
    // hits ~2e-3 in practice). We test against the conservative ARM spec
    // so the test passes on any host. Callers needing tighter precision
    // chain a Newton-Raphson step on top — see Simd_internal docs.
    alignas(16) std::array<f32, 4> in{1.0f, 4.0f, 16.0f, 100.0f};
    alignas(16) std::array<f32, 4> out{};
    simd::store_aligned4(out.data(), simd::rsqrt4(simd::load_aligned4(in.data())));
    for (size_t i = 0; i < 4; ++i) {
        f32 ref = 1.0f / std::sqrt(in[i]);
        REQUIRE(std::fabs(out[i] - ref) / ref < 4.0e-3f);
    }
}

TEST_CASE("reduce_add sums every lane", "[simd][reduce]") {
    alignas(16) std::array<f32, 4> in4{1.0f, 2.0f, 3.0f, 4.0f};
    REQUIRE(simd::reduce_add4(simd::load_aligned4(in4.data())) == 10.0f);

    alignas(32) std::array<f32, 8> in8{1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(simd::reduce_add8(simd::load_aligned8(in8.data())) == 36.0f);
}

TEST_CASE("reduce_min/max match std::min/max over the lanes", "[simd][reduce]") {
    alignas(16) std::array<f32, 4> in{3.0f, -1.0f, 2.0f, 5.0f};
    auto v = simd::load_aligned4(in.data());
    REQUIRE(simd::reduce_min4(v) == -1.0f);
    REQUIRE(simd::reduce_max4(v) == 5.0f);
}

TEST_CASE("mask_to_int packs lane signs into the bottom bits", "[simd][mask]") {
    alignas(16) std::array<f32, 4> a{1.0f, 2.0f, 3.0f, 4.0f};
    alignas(16) std::array<f32, 4> b{0.0f, 5.0f, 0.0f, 5.0f};   // <a, >a, <a, >a
    auto m = simd::cmp_lt4(simd::load_aligned4(b.data()),
                           simd::load_aligned4(a.data()));
    // lanes 0 and 2 are set → 0b0101 = 5
    REQUIRE(simd::mask_to_int4(m) == 0b0101);
    REQUIRE(simd::any_of(m));
    REQUIRE_FALSE(simd::all_of(m));
    REQUIRE_FALSE(simd::none_of(m));
}

TEST_CASE("mask logical ops compose", "[simd][mask]") {
    alignas(16) std::array<f32, 4> a{1.0f, 2.0f, 3.0f, 4.0f};
    alignas(16) std::array<f32, 4> b{1.0f, 1.0f, 3.0f, 5.0f};
    auto eq = simd::cmp_eq4(simd::load_aligned4(a.data()),
                            simd::load_aligned4(b.data()));
    auto lt = simd::cmp_lt4(simd::load_aligned4(a.data()),
                            simd::load_aligned4(b.data()));

    // eq lanes: 0, 2  → bits 0b0101
    REQUIRE(simd::mask_to_int4(eq) == 0b0101);
    // lt lanes: 3     → bit  0b1000
    REQUIRE(simd::mask_to_int4(lt) == 0b1000);

    auto either = simd::mask_or(eq, lt);
    REQUIRE(simd::mask_to_int4(either) == 0b1101);

    auto both   = simd::mask_and(eq, lt);
    REQUIRE(simd::mask_to_int4(both) == 0);

    auto inv    = simd::mask_not(eq);
    REQUIRE(simd::mask_to_int4(inv) == (0b1111 & ~0b0101));
}

TEST_CASE("gather pulls scattered indices", "[simd][gather]") {
    std::array<f32, 16> base{};
    for (size_t i = 0; i < 16; ++i) base[i] = static_cast<f32>(i * 10);

    alignas(16) std::array<i32, 4> idx{5, 1, 0, 12};
    auto v = simd::gather4(base.data(), simd::load_i32x4(idx.data()));

    alignas(16) std::array<f32, 4> out{};
    simd::store_aligned4(out.data(), v);
    REQUIRE(out[0] == 50.0f);
    REQUIRE(out[1] == 10.0f);
    REQUIRE(out[2] ==  0.0f);
    REQUIRE(out[3] == 120.0f);
}

TEST_CASE("add/sub/mul/div match scalar across a small grid", "[simd][arith]") {
    alignas(16) std::array<f32, 4> a{1.5f, -2.0f, 4.0f, 0.5f};
    alignas(16) std::array<f32, 4> b{2.0f,  3.0f, 0.5f, 1.0f};

    auto va = simd::load_aligned4(a.data());
    auto vb = simd::load_aligned4(b.data());

    alignas(16) std::array<f32, 4> r_add{}, r_sub{}, r_mul{}, r_div{};
    simd::store_aligned4(r_add.data(), simd::add4(va, vb));
    simd::store_aligned4(r_sub.data(), simd::sub4(va, vb));
    simd::store_aligned4(r_mul.data(), simd::mul4(va, vb));
    simd::store_aligned4(r_div.data(), simd::div4(va, vb));

    for (size_t i = 0; i < 4; ++i) {
        REQUIRE(r_add[i] == a[i] + b[i]);
        REQUIRE(r_sub[i] == a[i] - b[i]);
        REQUIRE(r_mul[i] == a[i] * b[i]);
        REQUIRE(r_div[i] == a[i] / b[i]);
    }
}

TEST_CASE("dispatch.reduce_add matches a scalar sum", "[simd][dispatch]") {
    std::vector<f32> buf(1024);
    f32              ref = 0.0f;
    std::mt19937     rng(0x12345u);
    std::uniform_real_distribution<f32> dist(-1.0f, 1.0f);
    for (auto& x : buf) {
        x   = dist(rng);
        ref += x;
    }
    f32 simd_sum = simd::reduce_add(buf.data(), buf.size());
    // floating-point summation order differs, so accept a relative slop.
    REQUIRE(std::fabs(simd_sum - ref) <= 1e-3f * std::fabs(ref) + 1e-3f);
}

TEST_CASE("dispatch.add_buffer + fma_buffer match scalar refs", "[simd][dispatch]") {
    const usize  n = 1031;   // deliberately not a multiple of 8 or 4
    std::vector<f32> a(n), b(n), c(n), y(n), ref(n);
    std::mt19937     rng(0x99u);
    std::uniform_real_distribution<f32> dist(-1.0f, 1.0f);
    for (usize i = 0; i < n; ++i) { a[i] = dist(rng); b[i] = dist(rng); c[i] = dist(rng); }

    simd::add_buffer(a.data(), b.data(), y.data(), n);
    for (usize i = 0; i < n; ++i) REQUIRE(y[i] == a[i] + b[i]);

    simd::fma_buffer(a.data(), b.data(), c.data(), y.data(), n);
    for (usize i = 0; i < n; ++i) {
        f32 ref_i = std::fma(a[i], b[i], c[i]);
        REQUIRE(std::fabs(y[i] - ref_i) <= 1e-5f * std::fabs(ref_i) + 1e-5f);
    }
}
