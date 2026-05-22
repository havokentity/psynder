// SPDX-License-Identifier: MIT
// Psynder — Sample 13. Quake-style indoor room lit by runtime raytraced
// shadows, walked first-person.
//
// The scene: a two-room-with-doorway box layout (à la sample 03), but here
// the room's wall / floor / ceiling triangles are emitted into a single
// triangle list, packed into a `render::rt::Bvh8` BLAS, and referenced once
// from a `render::rt::Tlas`. Two point lights (one per room) light the
// interior. The sample hands its TLAS, per-triangle materials, camera, and
// lights to `render::rt::FrameRenderer`, which casts primary rays and 8-wide
// shadow packets through the SAME room BVH, so the dividing wall + doorway
// cast real shadows between the two rooms.
//
// Like sample 05, the lit pass runs at quarter resolution and is bilinear-
// upsampled into the final framebuffer to stay real-time.
//
// First-person navigation uses the shared `samples::CharacterController` in
// Fps mode, bounds-clamped to the union of both rooms; `noclip 1` lifts the
// clamp. ESC quits.
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the final framebuffer to PATH as PNG.

#include "common/CharacterController.h"
#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/rt/Bvh.h"
#include "render/rt/FrameRenderer.h"
#include "ui/console/ConsoleOverlay.h"
#include "ui/imm/DebugHud.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

// ─── Render config ───────────────────────────────────────────────────────
// Internal framebuffer (small — raytracing per pixel is expensive). The lit
// pass runs at half these dimensions on each axis (quarter pixel count) and
// is bilinear-upsampled into the final image.
constexpr u32 kFbW = 512;
constexpr u32 kFbH = 288;
constexpr u32 kLitW = kFbW / 2;  // 256
constexpr u32 kLitH = kFbH / 2;  // 144
constexpr u32 kNumLights = 2;

// Pack RGBA8 little-endian.
PSY_FORCEINLINE u32 pack_rgba8(u32 r, u32 g, u32 b, u32 a = 0xFFu) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}

// ─── Room geometry ───────────────────────────────────────────────────────
// Two boxy rooms joined by a doorway, axis-aligned, like sample 03's BSP map
// but emitted as a flat triangle list for the BVH instead of BSP faces.
//
//   World axes: +X right, +Y up, +Z toward room B (right-handed).
//
//      Z = -8 ┌──────────────┐
//             │              │   ROOM A
//             │              │
//      Z = -2 └──┐        ┌──┘
//                │ doorway│
//      Z =  0 ┌──┘        └──┐
//             │              │   ROOM B
//      Z =  6 └──────────────┘
//             X=-4          X=4
//
// Every surface is emitted via push_quad(), which triangulates the quad AND
// guarantees the geometric normal (cross of the triangle edges, which is what
// the BVH reports for a hit) points along the supplied inward normal — so the
// camera standing inside the room always sees correctly-lit interior faces
// regardless of the corner order we hand it.

struct RoomGeo {
    std::vector<render::rt::Triangle> tris;
    // Per-triangle inward normal + base RGB color (0..1), parallel to `tris`.
    std::vector<math::Vec3> normals;
    std::vector<math::Vec3> colors;
    // Axis-aligned union of both rooms + corridor — the character controller
    // clamps the eye inside this so the outer walls block you.
    math::Aabb bounds{};
    // Per-leaf walkable volumes (Room A / corridor / Room B) for the generic
    // slide collision — the union AABB alone lets you walk through the wall
    // strips beside the doorway corridor.
    std::array<math::Aabb, 3> walk_volumes{};
    f32 floor_y = 0.0f;
};

math::Vec3 rgb01(u32 r, u32 g, u32 b) noexcept {
    return {static_cast<f32>(r) / 255.0f, static_cast<f32>(g) / 255.0f, static_cast<f32>(b) / 255.0f};
}

// Append a quad (corners a,b,c,d in either winding) as two triangles whose
// geometric normal is forced to match `inward`. `inward` must be a unit
// vector pointing into the room interior.
void push_quad(RoomGeo& g,
               math::Vec3 a,
               math::Vec3 b,
               math::Vec3 c,
               math::Vec3 d,
               math::Vec3 inward,
               math::Vec3 color) {
    // Geometric normal of triangle (a,b,c) as the BVH would compute it.
    const math::Vec3 e1 = math::sub(b, a);
    const math::Vec3 e2 = math::sub(c, a);
    const math::Vec3 gn = math::cross(e1, e2);
    // If it points away from the room interior, reverse the winding so the
    // reported hit normal faces inward.
    const bool flip = math::dot(gn, inward) < 0.0f;
    if (flip) {
        std::swap(b, d);  // reverse the fan order: a,d,c,b
    }
    g.tris.push_back({a, b, c});
    g.tris.push_back({a, c, d});
    g.normals.push_back(inward);
    g.normals.push_back(inward);
    g.colors.push_back(color);
    g.colors.push_back(color);
}

void build_room(RoomGeo& g) {
    constexpr f32 kFloorY = 0.0f;
    constexpr f32 kCeilY = 3.0f;
    constexpr f32 kRoomAZ0 = -8.0f;  // back wall of room A
    constexpr f32 kRoomAZ1 = -2.0f;  // front wall of room A (doorway side)
    constexpr f32 kDoorZ0 = -2.0f;   // doorway near A
    constexpr f32 kDoorZ1 = 0.0f;    // doorway near B
    constexpr f32 kRoomBZ0 = 0.0f;   // back wall of room B (doorway side)
    constexpr f32 kRoomBZ1 = 6.0f;   // front wall of room B
    constexpr f32 kRoomX0 = -4.0f;
    constexpr f32 kRoomX1 = 4.0f;
    constexpr f32 kDoorX0 = -1.0f;  // doorway corridor extent
    constexpr f32 kDoorX1 = 1.0f;

    // Varied per-surface colors so the two rooms read distinctly and the
    // doorway corridor pops.
    const math::Vec3 cA_floor = rgb01(110, 140, 180);  // cool blue room A
    const math::Vec3 cA_ceil = rgb01(70, 90, 130);
    const math::Vec3 cA_wall = rgb01(150, 170, 200);
    const math::Vec3 cB_floor = rgb01(180, 140, 110);  // warm orange room B
    const math::Vec3 cB_ceil = rgb01(130, 90, 70);
    const math::Vec3 cB_wall = rgb01(200, 170, 150);
    const math::Vec3 cDoor_floor = rgb01(160, 200, 130);  // green corridor
    const math::Vec3 cDoor_ceil = rgb01(110, 150, 90);
    const math::Vec3 cDoor_wall = rgb01(180, 210, 150);

    const math::Vec3 up{0, 1, 0};
    const math::Vec3 down{0, -1, 0};
    const math::Vec3 px{1, 0, 0};
    const math::Vec3 nx{-1, 0, 0};
    const math::Vec3 pz{0, 0, 1};
    const math::Vec3 nz{0, 0, -1};

    // ── Room A ────────────────────────────────────────────────────────────
    push_quad(g,
              {kRoomX0, kFloorY, kRoomAZ0},
              {kRoomX1, kFloorY, kRoomAZ0},
              {kRoomX1, kFloorY, kRoomAZ1},
              {kRoomX0, kFloorY, kRoomAZ1},
              up,
              cA_floor);
    push_quad(g,
              {kRoomX0, kCeilY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ1},
              {kRoomX0, kCeilY, kRoomAZ1},
              down,
              cA_ceil);
    push_quad(g,  // -X wall (faces +X interior)
              {kRoomX0, kFloorY, kRoomAZ0},
              {kRoomX0, kCeilY, kRoomAZ0},
              {kRoomX0, kCeilY, kRoomAZ1},
              {kRoomX0, kFloorY, kRoomAZ1},
              px,
              cA_wall);
    push_quad(g,  // +X wall (faces -X interior)
              {kRoomX1, kFloorY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ1},
              {kRoomX1, kFloorY, kRoomAZ1},
              nx,
              cA_wall);
    push_quad(g,  // -Z back wall (faces +Z interior)
              {kRoomX0, kFloorY, kRoomAZ0},
              {kRoomX0, kCeilY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ0},
              {kRoomX1, kFloorY, kRoomAZ0},
              pz,
              cA_wall);
    // +Z front wall (doorway side) — two strips around the door opening.
    push_quad(g,
              {kRoomX0, kFloorY, kRoomAZ1},
              {kRoomX0, kCeilY, kRoomAZ1},
              {kDoorX0, kCeilY, kRoomAZ1},
              {kDoorX0, kFloorY, kRoomAZ1},
              nz,
              cA_wall);
    push_quad(g,
              {kDoorX1, kFloorY, kRoomAZ1},
              {kDoorX1, kCeilY, kRoomAZ1},
              {kRoomX1, kCeilY, kRoomAZ1},
              {kRoomX1, kFloorY, kRoomAZ1},
              nz,
              cA_wall);

    // ── Doorway corridor ────────────────────────────────────────────────
    push_quad(g,
              {kDoorX0, kFloorY, kDoorZ0},
              {kDoorX1, kFloorY, kDoorZ0},
              {kDoorX1, kFloorY, kDoorZ1},
              {kDoorX0, kFloorY, kDoorZ1},
              up,
              cDoor_floor);
    push_quad(g,
              {kDoorX0, kCeilY, kDoorZ0},
              {kDoorX1, kCeilY, kDoorZ0},
              {kDoorX1, kCeilY, kDoorZ1},
              {kDoorX0, kCeilY, kDoorZ1},
              down,
              cDoor_ceil);
    push_quad(g,  // corridor -X wall
              {kDoorX0, kFloorY, kDoorZ0},
              {kDoorX0, kCeilY, kDoorZ0},
              {kDoorX0, kCeilY, kDoorZ1},
              {kDoorX0, kFloorY, kDoorZ1},
              px,
              cDoor_wall);
    push_quad(g,  // corridor +X wall
              {kDoorX1, kFloorY, kDoorZ0},
              {kDoorX1, kCeilY, kDoorZ0},
              {kDoorX1, kCeilY, kDoorZ1},
              {kDoorX1, kFloorY, kDoorZ1},
              nx,
              cDoor_wall);

    // ── Room B ────────────────────────────────────────────────────────────
    push_quad(g,
              {kRoomX0, kFloorY, kRoomBZ0},
              {kRoomX1, kFloorY, kRoomBZ0},
              {kRoomX1, kFloorY, kRoomBZ1},
              {kRoomX0, kFloorY, kRoomBZ1},
              up,
              cB_floor);
    push_quad(g,
              {kRoomX0, kCeilY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ1},
              {kRoomX0, kCeilY, kRoomBZ1},
              down,
              cB_ceil);
    push_quad(g,  // -X wall
              {kRoomX0, kFloorY, kRoomBZ0},
              {kRoomX0, kCeilY, kRoomBZ0},
              {kRoomX0, kCeilY, kRoomBZ1},
              {kRoomX0, kFloorY, kRoomBZ1},
              px,
              cB_wall);
    push_quad(g,  // +X wall
              {kRoomX1, kFloorY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ1},
              {kRoomX1, kFloorY, kRoomBZ1},
              nx,
              cB_wall);
    push_quad(g,  // +Z far wall (faces -Z interior)
              {kRoomX0, kFloorY, kRoomBZ1},
              {kRoomX0, kCeilY, kRoomBZ1},
              {kRoomX1, kCeilY, kRoomBZ1},
              {kRoomX1, kFloorY, kRoomBZ1},
              nz,
              cB_wall);
    // -Z back wall (doorway side) — two strips around the door opening.
    push_quad(g,
              {kRoomX0, kFloorY, kRoomBZ0},
              {kRoomX0, kCeilY, kRoomBZ0},
              {kDoorX0, kCeilY, kRoomBZ0},
              {kDoorX0, kFloorY, kRoomBZ0},
              pz,
              cB_wall);
    push_quad(g,
              {kDoorX1, kFloorY, kRoomBZ0},
              {kDoorX1, kCeilY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ0},
              {kRoomX1, kFloorY, kRoomBZ0},
              pz,
              cB_wall);

    // World bounds: axis-aligned union of room A, the corridor, and room B.
    g.floor_y = kFloorY;
    g.bounds.min = {kRoomX0, kFloorY, kRoomAZ0};
    g.bounds.max = {kRoomX1, kCeilY, kRoomBZ1};
    // Walkable volumes for slide collision. The corridor is stretched +/-0.75
    // in Z so it overlaps both rooms past the 0.3 wall standoff (no dead gap at
    // the doorways); the stretch only re-covers floor already inside the rooms.
    g.walk_volumes[0] = math::Aabb{{kRoomX0, kFloorY, kRoomAZ0}, {kRoomX1, kCeilY, kRoomAZ1}};
    g.walk_volumes[1] =
        math::Aabb{{kDoorX0, kFloorY, kDoorZ0 - 0.75f}, {kDoorX1, kCeilY, kDoorZ1 + 0.75f}};
    g.walk_volumes[2] = math::Aabb{{kRoomX0, kFloorY, kRoomBZ0}, {kRoomX1, kCeilY, kRoomBZ1}};
}

// ─── Lights ──────────────────────────────────────────────────────────────
using Camera = render::rt::FrameCamera;
using Light = render::rt::FrameLight;

// One warm light near the ceiling of each room. Static — the shadows that
// move are cast by the moving camera's view, but the dividing wall + doorway
// produce a fixed hard shadow boundary the player walks through.
void make_lights(std::array<Light, kNumLights>& lights) {
    // Room A light (warm white, slightly cool to match the blue room).
    lights[0].position = {0.0f, 2.7f, -5.0f};
    lights[0].r = 0.95f;
    lights[0].g = 0.97f;
    lights[0].b = 1.0f;
    lights[0].intensity = 11.0f;
    lights[0].range = 16.0f;
    // Room B light (warm amber to match the orange room).
    lights[1].position = {0.0f, 2.7f, 3.0f};
    lights[1].r = 1.0f;
    lights[1].g = 0.85f;
    lights[1].b = 0.65f;
    lights[1].intensity = 11.0f;
    lights[1].range = 16.0f;
}

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 13 (RT Quake room)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;
    return desc;
}

int sample_main(const app::AppArgs& base_args, app::WindowApp& app_host) {
    const app::AppArgs& args = base_args;
    const u32 smoke_frames = args.smoke_frames;
    render::rt::ensure_frame_renderer_console_registered();
    const platform::WindowDesc desc = make_window_desc(args);
    auto* window = &app_host.window();

    RoomGeo room;
    build_room(room);
    PSY_LOG_INFO("sample_13: room built -- {} triangles", room.tris.size());

    render::rt::Bvh8 room_blas;
    room_blas.build(room.tris.data(), static_cast<u32>(room.tris.size()));

    std::array<render::rt::Tlas::InstanceDesc, 1> insts{};
    insts[0].blas = &room_blas;
    insts[0].transform = math::identity4();

    render::rt::Tlas tlas;
    tlas.build(insts.data(), static_cast<u32>(insts.size()));

    samples::CharacterControllerConfig cc_cfg{};
    cc_cfg.floor_y = room.floor_y;
    cc_cfg.eye_height = 1.6f;
    cc_cfg.bounds_skin = 0.3f;
    samples::CharacterController controller{cc_cfg};
    controller.set_volumes(room.walk_volumes.data(), static_cast<u32>(room.walk_volumes.size()));
    controller.set_mode(samples::ControllerMode::Fps);
    controller.set_position({0.0f, room.floor_y + cc_cfg.eye_height, -5.0f});
    controller.set_look(0.0f, 0.0f);

    std::vector<u32>& final_pixels = app_host.pixels();
    render::Framebuffer& fb = app_host.framebuffer();
    render::rt::FrameRenderer rt_frame_renderer;
    ui::imm::DebugHudFrameHistory hud_history{};

    PSY_LOG_INFO("Psynder sample 13 running{}",
                 smoke_frames > 0 ? fmt::format(" -- smoke mode, {} frames", smoke_frames)
                                  : std::string{});

    const u64 t0 = platform::Clock::ticks_now();
    u64 last_ticks = t0;
    u64 prev_frame_ticks = t0;
    u32 frame = 0;
    const f32 aspect = static_cast<f32>(kFbW) / static_cast<f32>(kFbH);
    constexpr f32 kFovY = 70.0f * math::kDegToRad;
    constexpr f32 kSmokeFrameMs = 1000.0f / 60.0f;

    std::array<Light, kNumLights> lights{};
    make_lights(lights);

    while (!window->should_close()) {
        window->poll_events();

        const u64 now_ticks = platform::Clock::ticks_now();
        const f32 frame_ms =
            smoke_frames > 0
                ? kSmokeFrameMs
                : static_cast<f32>(platform::Clock::seconds(now_ticks - prev_frame_ticks) * 1000.0);
        prev_frame_ticks = now_ticks;
        hud_history.push(frame_ms);

        if (auto* in = platform::input();
            in && in->key_down(platform::KeyCode::Escape) && !ui::console::is_open()) {
            PSY_LOG_INFO("sample_13: escape pressed, exiting");
            break;
        }

        auto* input = platform::input();
        const f32 dt =
            (smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(now_ticks - last_ticks)));
        last_ticks = now_ticks;

        const editor::Mode edit_mode = input ? editor::sample_step(*input, fb, dt) : editor::Mode::Play;
        if (smoke_frames > 0) {
            const f32 phase = static_cast<f32>(frame) / 60.0f;
            const f32 t01 = std::clamp(phase, 0.0f, 1.0f);
            controller.set_position({0.0f, room.floor_y + cc_cfg.eye_height, -5.0f + 8.0f * t01});
            controller.set_look(0.0f, 0.0f);
        } else if (edit_mode != editor::Mode::Edit && input && !ui::console::is_open()) {
            controller.update(*input, dt);
        }

        const Camera cam =
            render::rt::make_frame_camera(controller.eye(), controller.forward(), aspect, kFovY);

        render::rt::FrameRenderConfig rt_config =
            render::rt::frame_render_config_from_console(kFbW, kFbH, kLitW, kLitH, 16u);
        rt_config.ambient_scale = 7.2f;
        rt_config.direct_scale = 20.0f;
        rt_config.attenuation_quadratic = 0.05f;

        render::rt::FrameRenderInput rt_input{};
        rt_input.tlas = &tlas;
        rt_input.camera = cam;
        rt_input.lights = lights.data();
        rt_input.light_count = static_cast<u32>(lights.size());
        rt_input.materials.primitive_rgb = room.colors.data();
        rt_input.materials.primitive_count = static_cast<u32>(room.colors.size());
        rt_input.materials.default_rgba8 = pack_rgba8(140, 140, 150);
        rt_frame_renderer.render(rt_input, rt_config, final_pixels.data());

        ui::imm::draw_debug_hud(fb, hud_history.make_stats(frame_ms, 1, 0, 0));
        ui::console::draw(fb);
        window->present(fb);

        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("sample_13: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    const bool capture_ok = app_host.write_capture_if_requested("sample_13");
    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
struct RtQuakeSample {
    static constexpr std::string_view log_name() noexcept { return "sample_13"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder sample 13"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    int run(app::WindowApp& app_host, const app::AppArgs& args) {
        return sample_main(args, app_host);
    }
};

PSYNDER_WINDOW_SAMPLE_MAIN(RtQuakeSample)
