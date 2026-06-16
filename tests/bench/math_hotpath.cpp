// SPDX-License-Identifier: MIT
// Psynder — Lane 02 microbench for scalar math hot paths.

#include "core/Types.h"
#include "math/Batch.h"
#include "math/Math.h"
#include "math/VectorStack.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace psynder;

namespace {

#if defined(__aarch64__) || defined(_M_ARM64)
inline constexpr const char* kBatchTransformBackend = "neon4";
#elif defined(__x86_64__) || defined(_M_X64)
inline constexpr const char* kBatchTransformBackend = "sse4";
#else
inline constexpr const char* kBatchTransformBackend = "scalar";
#endif

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
    std::printf("math,transform_points,%s,%zu,%.3f\n",
                kBatchTransformBackend,
                static_cast<size_t>(o.n),
                ns_per);
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
    std::printf("math,transform_dirs,%s,%zu,%.3f\n",
                kBatchTransformBackend,
                static_cast<size_t>(o.n),
                ns_per);
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

void bench_vector_stack_points_soa(const BenchOpts& o) {
    const math::Mat4 m = sample_transform();
    math::Vec3SoaBuffer in(o.n);
    math::Vec3SoaBuffer out(o.n);
    f32* x = in.x_data();
    f32* y = in.y_data();
    f32* z = in.z_data();
    f32* ox = out.x_data();
    f32* oy = out.y_data();
    f32* oz = out.z_data();
    for (usize i = 0; i < o.n; ++i) {
        x[i] = (static_cast<f32>(i % 97) - 48.0f) * 0.25f;
        y[i] = (static_cast<f32>(i % 53) - 26.0f) * 0.5f;
        z[i] = (static_cast<f32>(i % 29) - 14.0f) * 0.75f;
    }

    math::VectorStack stack;
    stack.reserve_ops(1);
    auto fn = [&]() {
        stack.clear();
        stack.transform_points(m, in.view(), out.mutable_view());
        stack.flush();
        do_not_optimize(ox[o.n / 2]);
        do_not_optimize(oy[o.n / 2]);
        do_not_optimize(oz[o.n / 2]);
    };
    const double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("math,vector_stack_points_soa,%s,%zu,%.3f\n",
                math::vector_stack_backend(),
                static_cast<size_t>(o.n),
                ns_per);
}

void bench_vector_stack_points_aos(const BenchOpts& o) {
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

    math::VectorStack stack;
    stack.reserve_ops(1);
    auto fn = [&]() {
        stack.clear();
        stack.transform_points(m, in.data(), out.data(), out.size());
        stack.flush();
        do_not_optimize(out[o.n / 2]);
    };
    const double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("math,vector_stack_points_aos,%s,%zu,%.3f\n",
                math::vector_stack_backend(),
                static_cast<size_t>(o.n),
                ns_per);
}

void bench_integrate_positions_aos_scalar(const BenchOpts& o) {
    std::vector<math::Vec3> positions(o.n);
    std::vector<math::Vec3> velocities(o.n);
    for (usize i = 0; i < o.n; ++i) {
        positions[i] = math::Vec3{
            (static_cast<f32>(i % 97) - 48.0f) * 0.25f,
            (static_cast<f32>(i % 53) - 26.0f) * 0.5f,
            (static_cast<f32>(i % 29) - 14.0f) * 0.75f,
        };
        velocities[i] = math::Vec3{
            (static_cast<f32>(i % 31) - 15.0f) * 0.01f,
            (static_cast<f32>(i % 43) - 21.0f) * 0.02f,
            (static_cast<f32>(i % 67) - 33.0f) * 0.03f,
        };
    }

    auto fn = [&]() {
        for (usize i = 0; i < o.n; ++i) {
            positions[i] = math::add(positions[i], math::mul(velocities[i], 1.0f / 60.0f));
        }
        do_not_optimize(positions[o.n / 2]);
    };
    const double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("math,integrate_positions_aos,scalar,%zu,%.3f\n", static_cast<size_t>(o.n), ns_per);
}

void bench_vector_stack_integrate_soa(const BenchOpts& o) {
    math::Vec3SoaBuffer positions(o.n);
    math::Vec3SoaBuffer velocities(o.n);
    f32* px = positions.x_data();
    f32* py = positions.y_data();
    f32* pz = positions.z_data();
    f32* vx = velocities.x_data();
    f32* vy = velocities.y_data();
    f32* vz = velocities.z_data();
    for (usize i = 0; i < o.n; ++i) {
        px[i] = (static_cast<f32>(i % 97) - 48.0f) * 0.25f;
        py[i] = (static_cast<f32>(i % 53) - 26.0f) * 0.5f;
        pz[i] = (static_cast<f32>(i % 29) - 14.0f) * 0.75f;
        vx[i] = (static_cast<f32>(i % 31) - 15.0f) * 0.01f;
        vy[i] = (static_cast<f32>(i % 43) - 21.0f) * 0.02f;
        vz[i] = (static_cast<f32>(i % 67) - 33.0f) * 0.03f;
    }

    math::VectorStack stack;
    stack.reserve_ops(1);
    auto fn = [&]() {
        stack.clear();
        stack.integrate_positions(positions.mutable_view(), velocities.view(), 1.0f / 60.0f);
        stack.flush();
        do_not_optimize(px[o.n / 2]);
        do_not_optimize(py[o.n / 2]);
        do_not_optimize(pz[o.n / 2]);
    };
    const double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("math,vector_stack_integrate_soa,%s,%zu,%.3f\n",
                math::vector_stack_backend(),
                static_cast<size_t>(o.n),
                ns_per);
}

void bench_vector_stack_integrate_aos(const BenchOpts& o) {
    std::vector<math::Vec3> positions(o.n);
    std::vector<math::Vec3> velocities(o.n);
    for (usize i = 0; i < o.n; ++i) {
        positions[i] = math::Vec3{
            (static_cast<f32>(i % 97) - 48.0f) * 0.25f,
            (static_cast<f32>(i % 53) - 26.0f) * 0.5f,
            (static_cast<f32>(i % 29) - 14.0f) * 0.75f,
        };
        velocities[i] = math::Vec3{
            (static_cast<f32>(i % 31) - 15.0f) * 0.01f,
            (static_cast<f32>(i % 43) - 21.0f) * 0.02f,
            (static_cast<f32>(i % 67) - 33.0f) * 0.03f,
        };
    }

    math::VectorStack stack;
    stack.reserve_ops(1);
    auto fn = [&]() {
        stack.clear();
        stack.integrate_positions(positions.data(), velocities.data(), positions.size(), 1.0f / 60.0f);
        stack.flush();
        do_not_optimize(positions[o.n / 2]);
    };
    const double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("math,vector_stack_integrate_aos,%s,%zu,%.3f\n",
                math::vector_stack_backend(),
                static_cast<size_t>(o.n),
                ns_per);
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
    bench_vector_stack_points_soa(o);
    bench_vector_stack_points_aos(o);
    bench_integrate_positions_aos_scalar(o);
    bench_vector_stack_integrate_soa(o);
    bench_vector_stack_integrate_aos(o);
    return 0;
}
