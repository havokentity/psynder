// SPDX-License-Identifier: MIT
// Psynder — Sample 13. Quake-style indoor room lit by runtime raytraced
// shadows, walked first-person.
//
// The scene: a two-room-with-doorway box layout (à la sample 03), but here
// the room's wall / floor / ceiling triangles are emitted into a single
// triangle list, packed into a `render::rt::Bvh8` BLAS, and referenced once
// from a `render::rt::Tlas`. Two point lights (one per room) light the
// interior. Per low-res pixel we cast a primary ray into the room; on hit we
// Lambert-shade from each light and trace a shadow ray (gathered into an
// 8-wide `ShadowPacket8` and dispatched via
// `render::rt::trace_shadow_packet`) back through the SAME room BVH, so the
// dividing wall + doorway cast real shadows between the two rooms.
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
#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/rt/Bvh.h"
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

// ─── CLI parsing ─────────────────────────────────────────────────────────
struct Args {
    u32 smoke_frames = 0;
    std::string capture_out;
};

u32 parse_uint(std::string_view v) noexcept {
    u32 out = 0;
    for (char c : v) {
        if (c < '0' || c > '9')
            return 0;
        out = out * 10u + static_cast<u32>(c - '0');
    }
    return out;
}

Args parse_args(int argc, char** argv) {
    Args a{};
    constexpr std::string_view kFlag = "--smoke-frames=";
    constexpr std::string_view kFlagSp = "--smoke-frames";
    constexpr std::string_view kCapEq = "--smoke-capture-out=";
    constexpr std::string_view kCapSp = "--smoke-capture-out";
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s.starts_with(kFlag)) {
            a.smoke_frames = parse_uint(s.substr(kFlag.size()));
        } else if (s == kFlagSp && i + 1 < argc) {
            a.smoke_frames = parse_uint(std::string_view{argv[++i]});
        } else if (s.starts_with(kCapEq)) {
            a.capture_out = std::string(s.substr(kCapEq.size()));
        } else if (s == kCapSp && i + 1 < argc) {
            a.capture_out = argv[++i];
        }
    }
    return a;
}

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
PSY_FORCEINLINE u32 clamp_u8(f32 v) noexcept {
    if (v < 0.0f)
        return 0u;
    if (v > 255.0f)
        return 255u;
    return static_cast<u32>(v);
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

// ─── Camera ──────────────────────────────────────────────────────────────
struct Camera {
    math::Vec3 origin;
    math::Vec3 forward;
    math::Vec3 right;
    math::Vec3 up;
    f32 fov_tan;  // tan(half-fov-vertical)
    f32 aspect;
};

// Build the camera basis from the controller's eye + view matrix. The view
// matrix is world->view (rows = right / up / -forward), so we read the basis
// straight back out of it; this keeps the primary rays consistent with the
// controller's look direction.
Camera make_camera(const samples::CharacterController& cc, f32 fov_y_rad, f32 aspect) {
    Camera c{};
    c.origin = cc.eye();
    c.forward = cc.forward();
    c.right = math::normalize(math::cross(c.forward, math::Vec3{0, 1, 0}));
    c.up = math::cross(c.right, c.forward);
    c.fov_tan = std::tan(fov_y_rad * 0.5f);
    c.aspect = aspect;
    return c;
}

// Build a primary ray from a [0,1]^2 NDC pixel coordinate.
render::rt::Ray primary_ray(const Camera& cam, f32 nx, f32 ny) {
    const f32 sx = (2.0f * nx - 1.0f) * cam.aspect * cam.fov_tan;
    const f32 sy = (1.0f - 2.0f * ny) * cam.fov_tan;
    math::Vec3 dir{};
    dir.x = cam.forward.x + cam.right.x * sx + cam.up.x * sy;
    dir.y = cam.forward.y + cam.right.y * sx + cam.up.y * sy;
    dir.z = cam.forward.z + cam.right.z * sx + cam.up.z * sy;
    dir = math::normalize(dir);
    render::rt::Ray r{};
    r.origin = cam.origin;
    r.direction = dir;
    r.t_min = 1e-3f;
    r.t_max = 1e3f;
    return r;
}

// ─── Lights ──────────────────────────────────────────────────────────────
struct Light {
    math::Vec3 position;
    f32 intensity;
    f32 r, g, b;  // 0..1 color
    f32 range;
};

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

// ─── Skybox / background (seen only through the doorway / out-of-bounds) ──
u32 sample_sky(math::Vec3 dir) {
    const f32 t = 0.5f * (dir.y + 1.0f);
    const f32 horizon[3] = {6.0f, 10.0f, 28.0f};
    const f32 zenith[3] = {1.0f, 1.0f, 6.0f};
    const f32 r = horizon[0] * (1.0f - t) + zenith[0] * t;
    const f32 g = horizon[1] * (1.0f - t) + zenith[1] * t;
    const f32 b = horizon[2] * (1.0f - t) + zenith[2] * t;
    return pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

// ─── Bilinear upsample (low-res lit pass → final framebuffer) ────────────
void upsample_bilinear(const u32* src, u32 sw, u32 sh, u32* dst, u32 dw, u32 dh) {
    const f32 fx_scale = static_cast<f32>(sw) / static_cast<f32>(dw);
    const f32 fy_scale = static_cast<f32>(sh) / static_cast<f32>(dh);
    for (u32 y = 0; y < dh; ++y) {
        const f32 sy = (static_cast<f32>(y) + 0.5f) * fy_scale - 0.5f;
        const i32 y0 = std::max(0, static_cast<i32>(std::floor(sy)));
        const i32 y1 = std::min(static_cast<i32>(sh) - 1, y0 + 1);
        const f32 fy = sy - static_cast<f32>(y0);
        for (u32 x = 0; x < dw; ++x) {
            const f32 sx = (static_cast<f32>(x) + 0.5f) * fx_scale - 0.5f;
            const i32 x0 = std::max(0, static_cast<i32>(std::floor(sx)));
            const i32 x1 = std::min(static_cast<i32>(sw) - 1, x0 + 1);
            const f32 fx = sx - static_cast<f32>(x0);

            const u32 p00 = src[static_cast<usize>(y0) * sw + x0];
            const u32 p10 = src[static_cast<usize>(y0) * sw + x1];
            const u32 p01 = src[static_cast<usize>(y1) * sw + x0];
            const u32 p11 = src[static_cast<usize>(y1) * sw + x1];

            auto unpack = [](u32 p, f32& r, f32& g, f32& b) {
                r = static_cast<f32>(p & 0xFFu);
                g = static_cast<f32>((p >> 8) & 0xFFu);
                b = static_cast<f32>((p >> 16) & 0xFFu);
            };
            f32 r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
            unpack(p00, r00, g00, b00);
            unpack(p10, r10, g10, b10);
            unpack(p01, r01, g01, b01);
            unpack(p11, r11, g11, b11);
            const f32 w00 = (1.0f - fx) * (1.0f - fy);
            const f32 w10 = fx * (1.0f - fy);
            const f32 w01 = (1.0f - fx) * fy;
            const f32 w11 = fx * fy;
            const f32 r = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11;
            const f32 g = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11;
            const f32 b = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;
            dst[static_cast<usize>(y) * dw + x] = pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const u32 smoke_frames = args.smoke_frames;

    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 13 (RT Quake room)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_13: failed to create window");
        return EXIT_FAILURE;
    }

    // ── Build the static room geometry → one BLAS → TLAS. ───────────────
    RoomGeo room;
    build_room(room);
    PSY_LOG_INFO("sample_13: room built — {} triangles", room.tris.size());

    render::rt::Bvh8 room_blas;
    room_blas.build(room.tris.data(), static_cast<u32>(room.tris.size()));

    std::array<render::rt::Tlas::InstanceDesc, 1> insts{};
    insts[0].blas = &room_blas;
    insts[0].transform = math::identity4();

    render::rt::Tlas tlas;
    tlas.build(insts.data(), static_cast<u32>(insts.size()));

    // ── First-person controller (Fps mode, bounds = room union). ────────
    samples::CharacterControllerConfig cc_cfg{};
    cc_cfg.floor_y = room.floor_y;
    cc_cfg.eye_height = 1.6f;
    cc_cfg.bounds_skin = 0.3f;  // standoff > any near plane; ~player radius
    samples::CharacterController controller{cc_cfg};
    // Generic slide collision against the per-leaf volumes (Room A / corridor /
    // Room B) — the union AABB alone let you walk through the wall strips beside
    // the doorway.
    controller.set_volumes(room.walk_volumes.data(), static_cast<u32>(room.walk_volumes.size()));
    controller.set_mode(samples::ControllerMode::Fps);
    controller.set_position({0.0f, room.floor_y + cc_cfg.eye_height, -5.0f});  // in Room A
    controller.set_look(0.0f, 0.0f);

    // ── CPU framebuffers. ───────────────────────────────────────────────
    std::vector<u32> final_pixels(static_cast<usize>(kFbW) * kFbH, 0u);
    std::vector<u32> lit_pixels(static_cast<usize>(kLitW) * kLitH, 0u);

    render::Framebuffer fb{};
    fb.width = kFbW;
    fb.height = kFbH;
    fb.pitch = kFbW * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(final_pixels.data());

    PSY_LOG_INFO("Psynder sample 13 running{}",
                 smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", smoke_frames)
                                  : std::string{});

    const u64 t0 = platform::Clock::ticks_now();
    u64 last_ticks = t0;
    u32 frame = 0;

    const f32 aspect = static_cast<f32>(kFbW) / static_cast<f32>(kFbH);
    constexpr f32 kFovY = 70.0f * math::kDegToRad;

    std::array<Light, kNumLights> lights{};
    make_lights(lights);

    // Per-pixel primary-hit info so we can drive shadow packets in batches.
    struct PixelHit {
        bool hit;
        math::Vec3 position;
        math::Vec3 normal;
        f32 r, g, b;  // surface base color in 0..1
    };
    std::vector<PixelHit> hits(static_cast<usize>(kLitW) * kLitH);

    // 60-sample ring of frame-times (ms) for the debug HUD strip chart.
    constexpr u32 kFrameHistory = 60;
    std::array<f32, kFrameHistory> frame_ms_ring{};
    u64 prev_frame_ticks = t0;
    constexpr f32 kSmokeFrameMs = 1000.0f / 60.0f;

    while (!window->should_close()) {
        window->poll_events();

        const u64 now_ticks = platform::Clock::ticks_now();
        const f32 frame_ms =
            smoke_frames > 0
                ? kSmokeFrameMs
                : static_cast<f32>(platform::Clock::seconds(now_ticks - prev_frame_ticks) * 1000.0);
        prev_frame_ticks = now_ticks;
        frame_ms_ring[frame % kFrameHistory] = frame_ms;

        // ESC quits.
        if (auto* in = platform::input(); in && in->key_down(platform::KeyCode::Escape)) {
            PSY_LOG_INFO("sample_13: escape pressed, exiting");
            break;
        }

        auto* input = platform::input();

        // Per-frame dt for camera integration.
        const f32 dt =
            (smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(now_ticks - last_ticks)));
        last_ticks = now_ticks;

        // Editor F2/~ toggle + PLAY/EDIT badge bottom-right. EDIT mode freezes
        // camera motion so the user can inspect the lit scene.
        const editor::Mode edit_mode = input ? editor::sample_step(*input, fb) : editor::Mode::Play;

        if (smoke_frames > 0) {
            // Deterministic camera path: walk from deep in Room A through the
            // doorway into Room B over the first 60 frames, so the capture is
            // identical across hosts and exercises both lit rooms + the
            // shadowed doorway transition.
            const f32 phase = static_cast<f32>(frame) / 60.0f;
            const f32 t01 = std::clamp(phase, 0.0f, 1.0f);
            controller.set_position({0.0f, room.floor_y + cc_cfg.eye_height, -5.0f + 8.0f * t01});
            controller.set_look(0.0f, 0.0f);
        } else if (edit_mode != editor::Mode::Edit && input) {
            // Live play: WASD + mouse-look. V flies; `noclip 1` lifts bounds.
            controller.update(*input, dt);
        }

        const Camera cam = make_camera(controller, kFovY, aspect);

        // ── Pass 1: primary visibility @ low-res. ───────────────────────
        for (u32 y = 0; y < kLitH; ++y) {
            for (u32 x = 0; x < kLitW; ++x) {
                const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kLitW);
                const f32 ny = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kLitH);
                const render::rt::Ray ray = primary_ray(cam, nx, ny);
                const render::rt::Hit h = tlas.intersect(ray);

                const usize idx = static_cast<usize>(y) * kLitW + x;
                PixelHit ph{};
                ph.hit = h.hit;
                if (!h.hit) {
                    lit_pixels[idx] = sample_sky(ray.direction);
                } else {
                    // The triangle's stored inward color (BVH reports the
                    // hit primitive index into our parallel arrays).
                    const u32 prim =
                        std::min<u32>(h.primitive, static_cast<u32>(room.colors.size()) - 1u);
                    const math::Vec3 col = room.colors[prim];
                    ph.r = col.x;
                    ph.g = col.y;
                    ph.b = col.z;
                    ph.position = {
                        ray.origin.x + ray.direction.x * h.t,
                        ray.origin.y + ray.direction.y * h.t,
                        ray.origin.z + ray.direction.z * h.t,
                    };
                    // Use the geometry's authored inward normal (robust even
                    // if the BVH's geometric normal sign differs for a face).
                    ph.normal = room.normals[prim];
                }
                hits[idx] = ph;
            }
        }

        // ── Pass 2: per-pixel shadow trace (packets of 8). ──────────────
        std::vector<f32> accum(static_cast<usize>(kLitW) * kLitH * 3, 0.0f);

        for (u32 li = 0; li < kNumLights; ++li) {
            const Light& L = lights[li];
            render::rt::ShadowPacket8 pkt{};
            u32 in_pkt = 0;
            u32 pkt_idx[8] = {0};
            bool pkt_real[8] = {false};

            auto dispatch = [&]() {
                for (u32 i = in_pkt; i < 8; ++i) {
                    pkt.rays[i].origin = {0, 1e6f, 0};
                    pkt.rays[i].direction = {0, 1, 0};
                    pkt.rays[i].t_min = 1e-4f;
                    pkt.rays[i].t_max = 1.0f;
                    pkt_real[i] = false;
                }
                render::rt::trace_shadow_packet(tlas, pkt);
                for (u32 i = 0; i < 8; ++i) {
                    if (!pkt_real[i])
                        continue;
                    if (pkt.occluded[i])
                        continue;
                    const usize px = pkt_idx[i];
                    const PixelHit& ph = hits[px];
                    math::Vec3 to_light = math::sub(L.position, ph.position);
                    const f32 d2 = math::dot(to_light, to_light);
                    const f32 d = std::sqrt(d2);
                    if (d > L.range || d < 1e-4f)
                        continue;
                    const math::Vec3 ld = math::mul(to_light, 1.0f / d);
                    const f32 ndotl = std::max(0.0f, math::dot(ph.normal, ld));
                    if (ndotl <= 0.0f)
                        continue;
                    const f32 atten = 1.0f / (1.0f + d * d * 0.05f);
                    const f32 k = ndotl * atten * L.intensity;
                    accum[px * 3 + 0] += ph.r * L.r * k;
                    accum[px * 3 + 1] += ph.g * L.g * k;
                    accum[px * 3 + 2] += ph.b * L.b * k;
                }
                in_pkt = 0;
            };

            for (usize px = 0, n = hits.size(); px < n; ++px) {
                const PixelHit& ph = hits[px];
                if (!ph.hit)
                    continue;
                math::Vec3 to_light = math::sub(L.position, ph.position);
                const f32 d2 = math::dot(to_light, to_light);
                const f32 d = std::sqrt(d2);
                if (d > L.range || d < 1e-4f)
                    continue;
                const math::Vec3 ld = math::mul(to_light, 1.0f / d);
                // Back-face cull: skip if the light is behind the surface.
                if (math::dot(ph.normal, ld) <= 0.0f)
                    continue;
                // Origin offset along the normal to avoid self-shadowing.
                const math::Vec3 origin = {
                    ph.position.x + ph.normal.x * 1e-3f,
                    ph.position.y + ph.normal.y * 1e-3f,
                    ph.position.z + ph.normal.z * 1e-3f,
                };
                pkt.rays[in_pkt].origin = origin;
                pkt.rays[in_pkt].direction = ld;
                pkt.rays[in_pkt].t_min = 1e-4f;
                pkt.rays[in_pkt].t_max = d - 1e-3f;
                pkt_idx[in_pkt] = static_cast<u32>(px);
                pkt_real[in_pkt] = true;
                ++in_pkt;
                if (in_pkt == 8)
                    dispatch();
            }
            if (in_pkt > 0)
                dispatch();
        }

        // ── Combine: write final low-res lit color. ─────────────────────
        for (u32 y = 0; y < kLitH; ++y) {
            for (u32 x = 0; x < kLitW; ++x) {
                const usize idx = static_cast<usize>(y) * kLitW + x;
                if (!hits[idx].hit)
                    continue;  // sky already written
                const PixelHit& ph = hits[idx];
                // Ambient floor so unlit faces aren't pure black.
                const f32 amb = 0.12f;
                const f32 r = ph.r * amb * 60.0f + accum[idx * 3 + 0] * 20.0f;
                const f32 g = ph.g * amb * 60.0f + accum[idx * 3 + 1] * 20.0f;
                const f32 b = ph.b * amb * 60.0f + accum[idx * 3 + 2] * 20.0f;
                lit_pixels[idx] = pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
            }
        }

        // ── Pass 3: bilinear upsample low-res into the final FB. ────────
        upsample_bilinear(lit_pixels.data(), kLitW, kLitH, final_pixels.data(), kFbW, kFbH);

        // Debug HUD overlay — `r_debug_hud full` enables.
        {
            ui::imm::DebugHudStats stats{};
            stats.frame_ms = frame_ms;
            stats.avg_frame_ms = [&]() noexcept {
                const u32 n = std::min<u32>(frame + 1u, kFrameHistory);
                if (n == 0u)
                    return 0.0f;
                f32 sum = 0.0f;
                for (u32 i = 0; i < n; ++i)
                    sum += frame_ms_ring[i];
                return sum / static_cast<f32>(n);
            }();
            stats.draw_calls = 1;
            stats.triangles = 0;  // raytraced — no rasterized geometry
            stats.active_voices = 0;
            ui::imm::draw_debug_hud(fb, stats);
        }

        window->present(fb);

        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("sample_13: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(args.capture_out.c_str(),
                                                             final_pixels.data(),
                                                             fb.width,
                                                             fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_13: failed to write capture to {}", args.capture_out);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_13: wrote capture to {}", args.capture_out);
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
