// SPDX-License-Identifier: MIT
// Psynder — lane 07 unit test: surface cache auto-engage + LRU + hysteresis.
// DESIGN.md §7.6 / ADR-001 — the cache must:
//   1. Auto-classify SurfaceCached vs OnTheFly from DrawItem flags.
//   2. Apply 4-frame hysteresis before flipping a surface to SurfaceCached.
//   3. Honor `r_force_shading_path` overrides for A/B testing.
//   4. LRU-evict the oldest entry when the slab fills.
//   5. Key entries by `(surface_id, lightmap_version, mip_level)`.

#include "core/console/Console.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "render/raster/SurfaceCache.h"
#include "render/raster/TestMesh.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::render;
using namespace psynder::render::raster;

namespace {

// Make sure each test starts from a clean cache state so test ordering
// is deterministic. The cache is a process-singleton so any prior test
// could have left stale entries.
struct CacheReset {
    CacheReset() {
        // Force r_force_shading_path to 0 (auto). Tests that need a
        // different value re-set it.
        console::Console::Get().RegisterCVar("r_force_shading_path", "0", "", console::CVarFlags::None);
        console::Console::Get().SetCVarOverride("r_force_shading_path", "0");
        SurfaceCache::Get().clear();
        SurfaceCache::Get().reset_stats();
    }
};

}  // namespace

TEST_CASE("classify_surface returns OnTheFly when no eligibility hints",
          "[raster][surface-cache][adr-001]") {
    CacheReset reset;
    SurfaceDesc s{};
    s.surface_id = 1;
    s.flags = DrawFlags::None;  // none of the eligibility bits set
    REQUIRE(classify_surface(s) == ShadingPath::OnTheFly);
}

TEST_CASE(
    "classify_surface returns OnTheFly even when eligible for the "
    "first kHysteresis-1 frames",
    "[raster][surface-cache][hysteresis][adr-001]") {
    CacheReset reset;
    SurfaceDesc s{};
    s.surface_id = 42;
    s.flags = DrawFlags::EligibleMask;

    // First (kHysteresis - 1) classifications must still be OnTheFly.
    for (u32 i = 0; i < SurfaceCache::kHysteresis - 1; ++i) {
        SurfaceCache::Get().begin_frame();
        REQUIRE(classify_surface(s) == ShadingPath::OnTheFly);
    }
    // On the kHysteresis-th eligible frame, flip to SurfaceCached.
    SurfaceCache::Get().begin_frame();
    REQUIRE(classify_surface(s) == ShadingPath::SurfaceCached);
}

TEST_CASE("classify_surface: a single ineligible frame resets the streak",
          "[raster][surface-cache][hysteresis][adr-001]") {
    CacheReset reset;
    SurfaceDesc eligible{};
    eligible.surface_id = 7;
    eligible.flags = DrawFlags::EligibleMask;
    SurfaceDesc passing_flash{};
    passing_flash.surface_id = 7;
    passing_flash.flags = draw_flags_without(DrawFlags::EligibleMask, DrawFlags::NoDynamicLights);

    // Build up 3 eligible frames (still OnTheFly — one short of the bar).
    for (u32 i = 0; i < 3; ++i) {
        SurfaceCache::Get().begin_frame();
        REQUIRE(classify_surface(eligible) == ShadingPath::OnTheFly);
    }
    // A muzzle flash on frame 4 drops dynamic-lights eligibility — streak
    // must reset.
    SurfaceCache::Get().begin_frame();
    REQUIRE(classify_surface(passing_flash) == ShadingPath::OnTheFly);
    // Going eligible again, we should need a fresh kHysteresis run.
    for (u32 i = 0; i < SurfaceCache::kHysteresis - 1; ++i) {
        SurfaceCache::Get().begin_frame();
        REQUIRE(classify_surface(eligible) == ShadingPath::OnTheFly);
    }
    SurfaceCache::Get().begin_frame();
    REQUIRE(classify_surface(eligible) == ShadingPath::SurfaceCached);
}

TEST_CASE("r_force_shading_path forces both paths regardless of eligibility",
          "[raster][surface-cache][cvar][adr-001]") {
    CacheReset reset;
    SurfaceDesc s{};
    s.surface_id = 11;
    s.flags = DrawFlags::EligibleMask;

    // Force OnTheFly even when surface is fully eligible.
    console::Console::Get().SetCVarOverride("r_force_shading_path", "1");
    for (u32 i = 0; i < SurfaceCache::kHysteresis + 2; ++i) {
        SurfaceCache::Get().begin_frame();
        REQUIRE(classify_surface(s) == ShadingPath::OnTheFly);
    }

    // Force SurfaceCached even on a surface with no eligibility flags.
    console::Console::Get().SetCVarOverride("r_force_shading_path", "2");
    SurfaceDesc t{};
    t.surface_id = 12;
    t.flags = DrawFlags::None;
    REQUIRE(classify_surface(t) == ShadingPath::SurfaceCached);

    // Reset the override.
    console::Console::Get().SetCVarOverride("r_force_shading_path", "0");
}

TEST_CASE("SurfaceCache::acquire and find: same key returns the same slot",
          "[raster][surface-cache]") {
    CacheReset reset;
    auto& c = SurfaceCache::Get();
    const u32 slot1 = c.acquire(/*surface_id*/ 100,
                                /*lm_ver*/ 3,
                                /*mip*/ 0,
                                /*w*/ 64,
                                /*h*/ 64,
                                /*bytes*/ 64 * 64 * 4);
    REQUIRE(slot1 != SurfaceCache::kInvalid);

    // find() the same key — same slot.
    const u32 slot2 = c.find(100, 3, 0);
    REQUIRE(slot2 == slot1);

    // Different lightmap_version ⇒ miss.
    const u32 slot3 = c.find(100, 4, 0);
    REQUIRE(slot3 == SurfaceCache::kInvalid);

    // Different mip_level ⇒ miss.
    const u32 slot4 = c.find(100, 3, 1);
    REQUIRE(slot4 == SurfaceCache::kInvalid);
}

TEST_CASE("SurfaceCache: LRU evicts the oldest entry when slab fills",
          "[raster][surface-cache][lru]") {
    CacheReset reset;
    auto& c = SurfaceCache::Get();

    // Acquire many entries, each ~256 KB, until eviction kicks in. The
    // slab is 4 MiB, so ~16 such entries fill it; we push past that.
    constexpr u32 kEntryBytes = 256 * 1024;
    constexpr u32 kCount = 20;
    u32 first_slot = SurfaceCache::kInvalid;
    for (u32 i = 0; i < kCount; ++i) {
        const u32 slot = c.acquire(/*sid*/ 1000 + i,
                                   /*lm_ver*/ 0,
                                   /*mip*/ 0,
                                   /*w*/ 256,
                                   /*h*/ 256,
                                   kEntryBytes);
        REQUIRE(slot != SurfaceCache::kInvalid);
        if (i == 0)
            first_slot = slot;
    }

    // After kCount acquires, the LRU must have evicted at least some
    // entries (the slab is bounded at 4 MiB). We should see eviction
    // events in the stats.
    const auto s = c.stats();
    INFO("entries_live = " << s.entries_live << ", slab_used = " << s.slab_bytes_used
                           << ", evictions = " << s.evictions);
    REQUIRE(s.evictions > 0);
    REQUIRE(s.slab_bytes_used <= SurfaceCache::kSlabBytes);

    // The very first entry should now be a streak-only slot (payload
    // dropped) — find() returns kInvalid for the original key because
    // the payload is gone.
    const u32 still_there = c.find(1000, 0, 0);
    REQUIRE(still_there == SurfaceCache::kInvalid);
    (void)first_slot;
}

TEST_CASE(
    "SurfaceCache: entries are not destroyed when surface becomes "
    "ineligible - DESIGN.md sec 7.6",
    "[raster][surface-cache][warm]") {
    CacheReset reset;
    auto& c = SurfaceCache::Get();
    SurfaceDesc s{};
    s.surface_id = 9001;
    s.flags = DrawFlags::EligibleMask;

    // Pass the hysteresis bar.
    for (u32 i = 0; i < SurfaceCache::kHysteresis; ++i) {
        c.begin_frame();
        (void)classify_surface(s);
    }
    REQUIRE(c.past_hysteresis(9001));

    // Drop eligibility — the streak resets to 0…
    s.flags = DrawFlags::None;
    c.begin_frame();
    (void)classify_surface(s);
    REQUIRE_FALSE(c.past_hysteresis(9001));

    // …but the slot itself sticks around so the cache can warm up
    // again. (The internal entry is still occupied even though the
    // streak is 0.)
    REQUIRE(c.stats().entries_live >= 1);
}

TEST_CASE("SurfaceCache::clear wipes every entry", "[raster][surface-cache]") {
    CacheReset reset;
    auto& c = SurfaceCache::Get();
    REQUIRE(c.acquire(1, 0, 0, 32, 32, 32 * 32 * 4) != SurfaceCache::kInvalid);
    REQUIRE(c.acquire(2, 0, 0, 32, 32, 32 * 32 * 4) != SurfaceCache::kInvalid);
    REQUIRE(c.stats().entries_live >= 2);
    c.clear();
    REQUIRE(c.stats().entries_live == 0);
    REQUIRE(c.stats().slab_bytes_used == 0);
    REQUIRE(c.find(1, 0, 0) == SurfaceCache::kInvalid);
    REQUIRE(c.find(2, 0, 0) == SurfaceCache::kInvalid);
}

TEST_CASE("SurfaceCache: oversized acquire (> slab) rejects cleanly",
          "[raster][surface-cache][oversize]") {
    CacheReset reset;
    auto& c = SurfaceCache::Get();
    const u32 slot = c.acquire(1, 0, 0, 65536, 65536, SurfaceCache::kSlabBytes * 2);
    REQUIRE(slot == SurfaceCache::kInvalid);
}

TEST_CASE("SurfaceCache::begin_frame advances the frame index", "[raster][surface-cache]") {
    CacheReset reset;
    auto& c = SurfaceCache::Get();
    const u32 before = c.frame_index();
    c.begin_frame();
    c.begin_frame();
    c.begin_frame();
    REQUIRE(c.frame_index() == before + 3);
}

// End-to-end: Rasterizer::end_frame() runs classify_surface() on every
// submitted draw and bumps the SurfaceCache singleton's per-frame frame
// counter. After kHysteresis-many begin_frame() calls with an eligible
// DrawItem, the classifier should return SurfaceCached. This test goes
// directly through the public Rasterizer API.
TEST_CASE(
    "Rasterizer dispatches SurfaceCached after kHysteresis "
    "eligible frames",
    "[raster][surface-cache][integration]") {
    CacheReset reset;
    auto& cache = SurfaceCache::Get();

    // Inline framebuffer helper — anonymous-namespace ImageRT struct.
    struct ImageRT {
        std::vector<u32> pixels;
        std::vector<u32> depth;
        Framebuffer fb{};
        ImageRT(u32 w, u32 h)
            : pixels(static_cast<std::size_t>(w) * h, 0xFF000000u)
            , depth(static_cast<std::size_t>(w) * h, 0) {
            fb.width = w;
            fb.height = h;
            fb.pitch = w * 4;
            fb.format = PixelFormat::RGBA8;
            fb.pixels = reinterpret_cast<u8*>(pixels.data());
            fb.depth = depth.data();
        }
    };

    auto mesh = test_mesh::colored_triangle();
    ImageRT img(64, 64);

    ViewState v{};
    v.view = math::look_at_rh(math::Vec3{0, 0, 2}, math::Vec3{0, 0, 0}, math::Vec3{0, 1, 0});
    v.projection =
        math::perspective_rh(60.0f * math::kDegToRad,
                             static_cast<f32>(img.fb.width) / static_cast<f32>(img.fb.height),
                             0.1f,
                             100.0f);
    v.target = img.fb;
    v.tile_w = 64;
    v.tile_h = 64;

    DrawItem d{};
    d.vertices = mesh.vertices;
    d.vertex_count = mesh.vertex_count;
    d.indices = mesh.indices;
    d.index_count = mesh.index_count;
    d.model = math::identity4();
    d.material = MaterialId{/*raw*/ 0xCAFE};
    d.flags = DrawFlags::EligibleMask;  // fully eligible

    auto& r = Rasterizer::Get();
    // Run kHysteresis frames — each begin_frame bumps the cache index.
    for (u32 i = 0; i < SurfaceCache::kHysteresis; ++i) {
        clear_framebuffer(img.fb, 0xFF000000u);
        r.begin_frame(v);
        r.submit(d);
        r.end_frame();
    }
    // After kHysteresis eligible classifications, the surface_id 0xCAFE
    // should be past the hysteresis bar.
    REQUIRE(cache.past_hysteresis(0xCAFE));
}
