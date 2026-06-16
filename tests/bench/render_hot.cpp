// SPDX-License-Identifier: MIT
// Psynder — bench: the HOT render kernels (Wave-6 perf pass).
//
// The existing raster_tile bench drives an UNtextured, UNlit fullscreen quad,
// so it never exercises the fragment combine loop's lighting/shadow/texture
// dispatch or the per-pixel perspective-correct attribute interpolation that
// dominates a real lit frame. Likewise nothing here previously timed the
// BVH8/TLAS ray traversal + Moller-Trumbore ray-tri kernel. This bench fills
// both gaps so the kernelization pass has a before/after signal:
//
//   raster_lit_frame   — render a grid of lit, camera-facing quads through the
//                        real engine rasterizer (begin/submit/end) with a
//                        dynamic directional light fed via ViewState. This
//                        walks evaluate_raster_lighting per covered pixel and
//                        the full perspective-correct attribute interp, which
//                        is the kernel the perf pass tightens. Reports
//                        ns/frame and Mpix/s over the covered framebuffer.
//
//   raster_unlit_frame — same geometry with NO dynamic lights (the Raster
//                        default / golden path). Isolates the no-dynamic-light
//                        fragment cost so a regression there is visible
//                        independent of the lit path.
//
//   rt_bvh_primary     — trace a dense screen-shaped batch of coherent primary
//                        rays at a single BLAS (Bvh8::intersect → traverse_scalar
//                        → ray_triangle_mt). Reports Mrays/s.
//
//   rt_tlas_occluded   — trace the same ray batch as occlusion queries through a
//                        TLAS of instanced BLASes (Tlas::occluded →
//                        occluded_tlas_scalar). This is the shadow-ray shape the
//                        hybrid raster path fires per pixel per light.
//
// Numbers are reported on stderr (human) and stdout (JSON, same schema as
// runner.cpp so the CI baseline diff understands them). Run release-only:
//   ./build/mac-release/bin/psynder_bench_render_hot
//   --smoke for a 1-iter ctest-speed pass; --iters=N to override.

#include "core/Log.h"
#include "core/Types.h"
#include "core/console/Console.h"
#include "math/Math.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "render/raster/RasterLighting.h"
#include "render/rt/Bvh.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

using Clock = std::chrono::steady_clock;

f64 elapsed_ns(Clock::time_point a, Clock::time_point b) {
    using ns = std::chrono::duration<f64, std::nano>;
    return std::chrono::duration_cast<ns>(b - a).count();
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

struct BenchResult {
    std::string name;
    u64 iterations = 0;
    f64 ns_per_iter = 0.0;
    f64 throughput = 0.0;
    std::string throughput_unit;
};

// ─── Raster frame: a grid of camera-facing quads, optionally lit ─────────
struct RasterScene {
    static constexpr u32 W = 640;
    static constexpr u32 H = 360;
    std::vector<u32> pixels;
    std::vector<u32> depth;
    std::vector<render::raster::Vertex> verts;
    std::vector<u32> indices;
    render::Framebuffer fb{};

    RasterScene()
        : pixels(static_cast<usize>(W) * H, 0xFF000000u)
        , depth(static_cast<usize>(W) * H, 0) {
        fb.width = W;
        fb.height = H;
        fb.pitch = W * 4;
        fb.format = render::PixelFormat::RGBA8;
        fb.pixels = reinterpret_cast<u8*>(pixels.data());
        fb.depth = depth.data();

        // A 6x6 grid of unit quads tiled across the camera frustum at z=0 so
        // they cover most of the framebuffer with overdraw-light geometry. Each
        // quad's normal faces +Z (the camera) so the dynamic light lands.
        constexpr i32 kGrid = 6;
        for (i32 gy = 0; gy < kGrid; ++gy) {
            for (i32 gx = 0; gx < kGrid; ++gx) {
                const f32 cx = (static_cast<f32>(gx) - kGrid * 0.5f + 0.5f) * 0.9f;
                const f32 cy = (static_cast<f32>(gy) - kGrid * 0.5f + 0.5f) * 0.5f;
                const f32 hs = 0.42f;
                const u32 base = static_cast<u32>(verts.size());
                verts.push_back(render::raster::Vertex{math::Vec3{cx - hs, cy - hs, 0.0f},
                                                       math::Vec3{0, 0, 1}, math::Vec2{0, 0},
                                                       math::Vec2{0, 0}, 0xFFFFFFFFu});
                verts.push_back(render::raster::Vertex{math::Vec3{cx + hs, cy - hs, 0.0f},
                                                       math::Vec3{0, 0, 1}, math::Vec2{1, 0},
                                                       math::Vec2{0, 0}, 0xFFFFFFFFu});
                verts.push_back(render::raster::Vertex{math::Vec3{cx + hs, cy + hs, 0.0f},
                                                       math::Vec3{0, 0, 1}, math::Vec2{1, 1},
                                                       math::Vec2{0, 0}, 0xFFFFFFFFu});
                verts.push_back(render::raster::Vertex{math::Vec3{cx - hs, cy + hs, 0.0f},
                                                       math::Vec3{0, 0, 1}, math::Vec2{0, 1},
                                                       math::Vec2{0, 0}, 0xFFFFFFFFu});
                indices.push_back(base + 0);
                indices.push_back(base + 1);
                indices.push_back(base + 2);
                indices.push_back(base + 0);
                indices.push_back(base + 2);
                indices.push_back(base + 3);
            }
        }
    }
};

BenchResult bench_raster_frame(const char* name, bool lit, u64 iters) {
    using namespace render::raster;
    RasterScene scene;

    console::Console::Get().RegisterCVar(
        "r_tile_size", "64", "Per-tile rasterizer tile dimension.", console::CVarFlags::None);
    console::Console::Get().SetCVarOverride("r_tile_size", "64");

    RasterLight key{};
    key.kind = RasterLightKind::Directional;
    key.direction_world = math::Vec3{0.0f, 0.0f, -1.0f};
    key.color_linear = math::Vec3{1.0f, 1.0f, 1.0f};
    key.intensity = 0.8f;

    ViewState v{};
    v.view = math::look_at_rh(math::Vec3{0, 0, 4}, math::Vec3{0, 0, 0}, math::Vec3{0, 1, 0});
    v.projection = math::perspective_rh(60.0f * math::kDegToRad,
                                        static_cast<f32>(RasterScene::W) / static_cast<f32>(RasterScene::H),
                                        0.1f, 100.0f);
    v.target = scene.fb;
    v.tile_w = 64;
    v.tile_h = 64;
    if (lit) {
        v.lights = &key;
        v.light_count = 1;
        v.ambient_linear = math::Vec3{0.15f, 0.15f, 0.15f};
    }

    DrawItem d{};
    d.vertices = scene.verts.data();
    d.vertex_count = static_cast<u32>(scene.verts.size());
    d.indices = scene.indices.data();
    d.index_count = static_cast<u32>(scene.indices.size());
    d.model = math::identity4();
    d.cull = CullMode::None;

    auto& r = Rasterizer::Get();
    auto frame = [&]() {
        clear_framebuffer(scene.fb, 0xFF000000u);
        r.begin_frame(v);
        r.submit(d);
        r.end_frame();
    };

    for (u64 i = 0; i < std::min<u64>(iters, 4); ++i)
        frame();
    do_not_optimize(scene.pixels);

    const auto t0 = Clock::now();
    for (u64 i = 0; i < iters; ++i)
        frame();
    do_not_optimize(scene.pixels);
    const auto t1 = Clock::now();
    const f64 ns = elapsed_ns(t0, t1);

    BenchResult br{};
    br.name = name;
    br.iterations = iters;
    br.ns_per_iter = ns / static_cast<f64>(iters);
    const f64 px = static_cast<f64>(iters) * RasterScene::W * RasterScene::H;
    br.throughput = (ns > 0) ? (px * 1.0e9 / ns) : 0.0;
    br.throughput_unit = "pixels/s";
    return br;
}

// ─── RT scene: a dense triangle mesh + a screen-shaped coherent ray batch ─
std::vector<render::rt::Triangle> make_rt_mesh(u32 dim) {
    std::vector<render::rt::Triangle> tris;
    tris.reserve(static_cast<usize>(dim) * dim * 2);
    const f32 inv = 1.0f / static_cast<f32>(dim);
    for (u32 j = 0; j < dim; ++j) {
        for (u32 i = 0; i < dim; ++i) {
            const f32 x = (static_cast<f32>(i) * inv - 0.5f) * 4.0f;
            const f32 y = (static_cast<f32>(j) * inv - 0.5f) * 4.0f;
            const f32 s = inv * 3.6f;
            tris.push_back(render::rt::Triangle{math::Vec3{x, y, 5.0f},
                                                math::Vec3{x + s, y, 5.0f},
                                                math::Vec3{x, y + s, 5.0f}});
            tris.push_back(render::rt::Triangle{math::Vec3{x + s, y, 5.0f},
                                                math::Vec3{x + s, y + s, 5.0f},
                                                math::Vec3{x, y + s, 5.0f}});
        }
    }
    return tris;
}

// Build a coherent screen-shaped batch of rays aimed at the mesh plane.
std::vector<render::rt::Ray> make_ray_batch(u32 rw, u32 rh) {
    std::vector<render::rt::Ray> rays;
    rays.reserve(static_cast<usize>(rw) * rh);
    for (u32 py = 0; py < rh; ++py) {
        for (u32 px = 0; px < rw; ++px) {
            const f32 sx = (static_cast<f32>(px) / static_cast<f32>(rw) - 0.5f) * 4.2f;
            const f32 sy = (static_cast<f32>(py) / static_cast<f32>(rh) - 0.5f) * 4.2f;
            render::rt::Ray r;
            r.origin = math::Vec3{sx, sy, 0.0f};
            r.direction = math::Vec3{0.0f, 0.0f, 1.0f};
            r.t_min = 1e-4f;
            r.t_max = 1e30f;
            rays.push_back(r);
        }
    }
    return rays;
}

BenchResult bench_rt_bvh_primary(u64 iters) {
    using namespace render::rt;
    const auto tris = make_rt_mesh(24);  // 24*24*2 = 1152 tris
    Bvh8 bvh;
    bvh.build(tris.data(), static_cast<u32>(tris.size()));
    const auto rays = make_ray_batch(96, 96);  // 9216 coherent primary rays

    u32 hits = 0;
    auto trace_all = [&]() {
        u32 h = 0;
        for (const Ray& r : rays) {
            const Hit hit = bvh.intersect(r);
            h += hit.hit ? 1u : 0u;
        }
        hits = h;
    };
    for (u64 i = 0; i < std::min<u64>(iters, 2); ++i)
        trace_all();
    do_not_optimize(hits);

    const auto t0 = Clock::now();
    for (u64 i = 0; i < iters; ++i)
        trace_all();
    const auto t1 = Clock::now();
    do_not_optimize(hits);
    const f64 ns = elapsed_ns(t0, t1);

    BenchResult br{};
    br.name = "rt_bvh_primary";
    br.iterations = iters;
    br.ns_per_iter = ns / static_cast<f64>(iters);
    const f64 nrays = static_cast<f64>(iters) * static_cast<f64>(rays.size());
    br.throughput = (ns > 0) ? (nrays * 1.0e9 / ns) : 0.0;
    br.throughput_unit = "rays/s";
    return br;
}

BenchResult bench_rt_tlas_occluded(u64 iters) {
    using namespace render::rt;
    const auto tris = make_rt_mesh(20);  // 800 tris
    Bvh8 blas;
    blas.build(tris.data(), static_cast<u32>(tris.size()));

    Tlas::InstanceDesc insts[4];
    const f32 offs[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
    for (u32 i = 0; i < 4; ++i) {
        insts[i].blas = &blas;
        math::Mat4 m = math::identity4();
        m.m[12] = offs[i][0];
        m.m[13] = offs[i][1];
        insts[i].transform = m;
    }
    Tlas tlas;
    tlas.build(insts, 4);

    const auto rays = make_ray_batch(72, 72);  // 5184 shadow-shaped rays

    u32 blocked = 0;
    auto trace_all = [&]() {
        u32 b = 0;
        for (const Ray& r : rays) {
            b += tlas.occluded(r) ? 1u : 0u;
        }
        blocked = b;
    };
    for (u64 i = 0; i < std::min<u64>(iters, 2); ++i)
        trace_all();
    do_not_optimize(blocked);

    const auto t0 = Clock::now();
    for (u64 i = 0; i < iters; ++i)
        trace_all();
    const auto t1 = Clock::now();
    do_not_optimize(blocked);
    const f64 ns = elapsed_ns(t0, t1);

    BenchResult br{};
    br.name = "rt_tlas_occluded";
    br.iterations = iters;
    br.ns_per_iter = ns / static_cast<f64>(iters);
    const f64 nrays = static_cast<f64>(iters) * static_cast<f64>(rays.size());
    br.throughput = (ns > 0) ? (nrays * 1.0e9 / ns) : 0.0;
    br.throughput_unit = "rays/s";
    return br;
}

// ─── Bit-identity oracle (--checksum) ────────────────────────────────────
// Renders one lit + one unlit frame and traces the RT primary batch once,
// emitting FNV-1a hashes of the EXACT outputs (raw framebuffer bytes; per-ray
// hit flag + t bits + normal bits + primitive). Run before/after a kernel
// change: identical hashes prove the output is byte-for-byte unchanged.
u64 fnv1a(u64 h, const void* p, usize n) noexcept {
    const u8* b = static_cast<const u8*>(p);
    for (usize i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

u64 raster_frame_checksum(bool lit) {
    using namespace render::raster;
    RasterScene scene;
    console::Console::Get().RegisterCVar("r_tile_size", "64", "", console::CVarFlags::None);
    console::Console::Get().SetCVarOverride("r_tile_size", "64");

    RasterLight key{};
    key.kind = RasterLightKind::Directional;
    key.direction_world = math::Vec3{0.0f, 0.0f, -1.0f};
    key.color_linear = math::Vec3{1.0f, 1.0f, 1.0f};
    key.intensity = 0.8f;

    ViewState v{};
    v.view = math::look_at_rh(math::Vec3{0, 0, 4}, math::Vec3{0, 0, 0}, math::Vec3{0, 1, 0});
    v.projection = math::perspective_rh(60.0f * math::kDegToRad,
                                        static_cast<f32>(RasterScene::W) / static_cast<f32>(RasterScene::H),
                                        0.1f, 100.0f);
    v.target = scene.fb;
    v.tile_w = 64;
    v.tile_h = 64;
    if (lit) {
        v.lights = &key;
        v.light_count = 1;
        v.ambient_linear = math::Vec3{0.15f, 0.15f, 0.15f};
    }
    DrawItem d{};
    d.vertices = scene.verts.data();
    d.vertex_count = static_cast<u32>(scene.verts.size());
    d.indices = scene.indices.data();
    d.index_count = static_cast<u32>(scene.indices.size());
    d.model = math::identity4();
    d.cull = CullMode::None;

    clear_framebuffer(scene.fb, 0xFF000000u);
    auto& r = Rasterizer::Get();
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();
    return fnv1a(1469598103934665603ull, scene.pixels.data(), scene.pixels.size() * sizeof(u32));
}

u64 rt_primary_checksum() {
    using namespace render::rt;
    const auto tris = make_rt_mesh(24);
    Bvh8 bvh;
    bvh.build(tris.data(), static_cast<u32>(tris.size()));
    const auto rays = make_ray_batch(96, 96);
    u64 h = 1469598103934665603ull;
    for (const Ray& r : rays) {
        const Hit hit = bvh.intersect(r);
        const u8 flag = hit.hit ? 1u : 0u;
        h = fnv1a(h, &flag, 1);
        h = fnv1a(h, &hit.t, sizeof(hit.t));
        h = fnv1a(h, &hit.normal, sizeof(hit.normal));
        h = fnv1a(h, &hit.primitive, sizeof(hit.primitive));
    }
    return h;
}

std::string emit_json(const std::vector<BenchResult>& results) {
    std::ostringstream os;
    os.precision(6);
    os << std::fixed;
    os << "{\n  \"benches\": [\n";
    for (usize i = 0; i < results.size(); ++i) {
        const BenchResult& r = results[i];
        os << "    {"
           << "\"name\": \"" << r.name << "\", "
           << "\"iterations\": " << r.iterations << ", "
           << "\"ns_per_iter\": " << r.ns_per_iter << ", "
           << "\"throughput\": " << r.throughput << ", "
           << "\"throughput_unit\": \"" << r.throughput_unit << "\""
           << "}";
        if (i + 1 < results.size())
            os << ",";
        os << "\n";
    }
    os << "  ]\n}\n";
    return os.str();
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    bool checksum = false;
    u64 iters_override = 0;
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s == "--smoke")
            smoke = true;
        else if (s == "--checksum")
            checksum = true;
        else if (s.starts_with("--iters="))
            iters_override = std::strtoull(std::string(s.substr(8)).c_str(), nullptr, 10);
    }

    if (checksum) {
        std::printf("raster_lit_frame   checksum: %016llx\n",
                    static_cast<unsigned long long>(raster_frame_checksum(true)));
        std::printf("raster_unlit_frame checksum: %016llx\n",
                    static_cast<unsigned long long>(raster_frame_checksum(false)));
        std::printf("rt_bvh_primary     checksum: %016llx\n",
                    static_cast<unsigned long long>(rt_primary_checksum()));
        return EXIT_SUCCESS;
    }

    const u64 raster_iters = smoke ? 1 : (iters_override ? iters_override : 400);
    const u64 rt_iters = smoke ? 1 : (iters_override ? iters_override : 60);

    std::vector<BenchResult> results;
    results.push_back(bench_raster_frame("raster_lit_frame", true, raster_iters));
    results.push_back(bench_raster_frame("raster_unlit_frame", false, raster_iters));
    results.push_back(bench_rt_bvh_primary(rt_iters));
    results.push_back(bench_rt_tlas_occluded(rt_iters));

    for (const BenchResult& r : results) {
        PSY_LOG_INFO("bench: {} — {} iters, {:.1f} ns/iter, {:.3e} {}",
                     r.name, r.iterations, r.ns_per_iter, r.throughput, r.throughput_unit);
    }

    std::fputs(emit_json(results).c_str(), stdout);
    return EXIT_SUCCESS;
}
