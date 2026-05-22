// SPDX-License-Identifier: MIT
// Psynder — Lane 02 microbench for scalar math hot paths.

#include "core/Types.h"
#include "math/Batch.h"
#include "math/Math.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace psynder;

namespace {

struct BenchOpts {
    bool smoke = false;
    usize n = 1u << 16;
    int iters = 200;
};

BenchOpts parse_opts(int argc, char** argv) {
    BenchOpts o;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            o.smoke = true;
            o.n = 1024;
            o.iters = 4;
        }
    }
    return o;
}

template <class T>
PSY_FORCEINLINE void do_not_optimize(const T& v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&v) : "memory");
#else
    const volatile T& sink = v;
    (void)sink;
#endif
}

template <typename Fn>
double median_ns_per_elem(Fn&& fn, usize elements, int iters) {
    std::vector<double> samples;
    samples.reserve(static_cast<usize>(iters));
    for (int it = 0; it < iters; ++it) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        samples.push_back(ns / static_cast<double>(elements));
    }
    std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
    return samples[samples.size() / 2];
}

math::Mat4 sample_transform() {
    math::Quat q =
        math::quat_normalize(math::quat_from_axis_angle(math::Vec3{0.2f, 0.7f, -0.4f}, 0.8f));
    return math::mul(math::translate(math::Vec3{3.0f, -2.0f, 5.0f}),
                     math::mul(math::rotate_quat(q), math::scale(math::Vec3{1.5f, 0.5f, 2.0f})));
}

void bench_transform_points(const BenchOpts& o) {
    const math::Mat4 m = sample_transform();
    std::vector<math::Vec3> in(o.n);
    std::vector<math::Vec3> out(o.n);
    for (usize i = 0; i < o.n; ++i) {
        in[i] = math::Vec3{
            (static_cast<f32>(i % 97) - 48.0f) * 0.25f,
            (static_cast<f32>(i % 53) - 26.0f) * 0.5f,
            (static_cast<f32>(i % 29) - 14.0f) * 0.75f,
        };
    }

    auto fn = [&]() {
        math::transform_points(m, in.data(), out.data(), out.size());
        do_not_optimize(out[o.n / 2]);
    };
    const double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("math,transform_points,scalar,%zu,%.3f\n", static_cast<size_t>(o.n), ns_per);
}

void bench_transform_dirs(const BenchOpts& o) {
    const math::Mat4 m = sample_transform();
    std::vector<math::Vec3> in(o.n);
    std::vector<math::Vec3> out(o.n);
    for (usize i = 0; i < o.n; ++i) {
        in[i] = math::Vec3{
            (static_cast<f32>(i % 31) - 15.0f) * 0.125f,
            (static_cast<f32>(i % 43) - 21.0f) * 0.25f,
            (static_cast<f32>(i % 67) - 33.0f) * 0.5f,
        };
    }

    auto fn = [&]() {
        math::transform_dirs(m, in.data(), out.data(), out.size());
        do_not_optimize(out[o.n / 2]);
    };
    const double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("math,transform_dirs,scalar,%zu,%.3f\n", static_cast<size_t>(o.n), ns_per);
}

void bench_mat4_mul(const BenchOpts& o) {
    std::vector<math::Mat4> a(o.n);
    std::vector<math::Mat4> b(o.n);
    std::vector<math::Mat4> out(o.n);
    for (usize i = 0; i < o.n; ++i) {
        const f32 t = static_cast<f32>(i % 1024) * 0.001f;
        a[i] = math::mul(math::translate(math::Vec3{t, -t * 0.5f, t * 0.25f}),
                         math::scale(math::Vec3{1.0f + t, 0.75f + t, 1.25f + t}));
        b[i] = math::mul(math::rotate_quat(math::quat_normalize(
                             math::quat_from_axis_angle(math::Vec3{0.3f, 0.5f, 0.7f}, t))),
                         math::translate(math::Vec3{-t, t * 0.25f, t * 0.5f}));
    }

    auto fn = [&]() {
        for (usize i = 0; i < o.n; ++i) {
            out[i] = math::mul(a[i], b[i]);
        }
        do_not_optimize(out[o.n / 2]);
    };
    const double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("math,mat4_mul,scalar,%zu,%.3f\n", static_cast<size_t>(o.n), ns_per);
}

}  // namespace

int main(int argc, char** argv) {
    const BenchOpts o = parse_opts(argc, argv);
    if (!o.smoke) {
        std::printf("# psynder bench — math lane\n");
        std::printf("# columns: lane,case,tier,elements,ns_per_elem\n");
    }

    bench_transform_points(o);
    bench_transform_dirs(o);
    bench_mat4_mul(o);
    return 0;
}
