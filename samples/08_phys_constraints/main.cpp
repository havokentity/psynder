// SPDX-License-Identifier: MIT
// Psynder — Sample 08. Constraints / ragdoll showcase.
//
// The scene: a static anchor body high overhead, a swinging rope/chain of
// dynamic point-masses hanging from it, and a small jointed "ragdoll" (torso
// box + head + two arms + two legs) dangling beside it. Everything sways
// under gravity and settles into a hanging pose.
//
// ── How the constraint demo is wired (important) ─────────────────────────
// The engine's *public* physics API (engine/physics/Physics.h) exposes only
// create_body / step / set_gravity / get_position / get_rotation. There is
// no public force/velocity/position writer and the solver only resolves
// CONTACT constraints (sequential-impulse PGS) — it does NOT solve joints.
// The six joint kinds (weld / axis / slider / ball-socket / rope / elastic)
// live in engine/editor/core/Constraints.h, but that module is purely an
// AUTHORING / serialisation data model: `constraints::Graph` stores the
// descriptors, it does not integrate them.
//
// So this sample exercises BOTH real public APIs honestly:
//   * `physics::World` — we create one rigid body per node (the static
//     anchor has mass = 0) and read the anchor pose back through the engine.
//   * `editor::constraints` — we author the full joint graph with the
//     `make_weld / make_axis / make_slider / make_ball_socket / make_rope /
//     make_elastic` constructors, using all six kinds.
// The motion itself is produced by a small deterministic Position-Based
// Dynamics (Verlet) solver *inside the sample* that iterates exactly the
// `constraints::Graph` we authored. dt is pinned to the engine's fixed
// 1/120 s tick so smoke runs are bit-stable across hosts.
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the final framebuffer to PATH as PNG.

#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Constraints.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "physics/Physics.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"

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
constexpr u32 kFbW = 640;
constexpr u32 kFbH = 360;

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

// ─── Physics nodes (PBD particles) ───────────────────────────────────────
// Each node is one engine rigid body PLUS a Verlet particle. The Verlet
// state is what we integrate against the authored constraint graph; the
// engine body provides the handle + (for the anchor) the pose we read back.
struct Node {
    physics::BodyId body{};    // engine body handle (anchor has mass 0)
    math::Vec3 pos{0, 0, 0};   // current position (Verlet)
    math::Vec3 prev{0, 0, 0};  // previous position (Verlet implicit velocity)
    f32 inv_mass = 1.0f;       // 0 == pinned / static anchor
    f32 radius = 0.12f;        // draw radius (visual)
    u32 color = 0xFFFFFFFFu;   // RGBA8 draw colour
};

// The whole scene's particle set + the authored constraint graph. Node
// indices in the graph are stored in Constraint::body_a / body_b (we use
// them as plain array indices into `nodes`, which is exactly what the
// editor-side model means by a body slot).
struct Scene {
    std::vector<Node> nodes;
    editor::constraints::Graph graph;
};

u32 add_node(Scene& s, math::Vec3 p, f32 mass, f32 radius, u32 color) {
    Node n{};
    physics::BodyDesc bd{};
    bd.shape = physics::Shape::Sphere;
    bd.mass = mass;  // 0 -> engine marks it static
    bd.position = p;
    bd.half_extent = {radius, radius, radius};
    n.body = physics::World::Get().create_body(bd);
    n.pos = p;
    n.prev = p;
    n.inv_mass = (mass > 0.0f) ? 1.0f / mass : 0.0f;
    n.radius = radius;
    n.color = color;
    const u32 idx = static_cast<u32>(s.nodes.size());
    s.nodes.push_back(n);
    return idx;
}

// Build the demo scene: anchor + rope/chain + ragdoll. Returns the node
// index of the chain tip (handy for smoke logging).
struct SceneInfo {
    u32 chain_tip = 0;
    u32 ragdoll_head = 0;
    u32 chain_anchor = 0;
};

SceneInfo build_scene(Scene& s) {
    using namespace editor::constraints;
    SceneInfo info{};

    // ── Static anchor up high (engine body, mass 0 == static). ──────────
    const math::Vec3 anchor_pos{-1.8f, 4.6f, 0.0f};
    const u32 anchor = add_node(s, anchor_pos, 0.0f, 0.16f, pack_rgba8(180, 180, 190));
    info.chain_anchor = anchor;

    // ── Rope / chain of dynamic point-masses hanging from the anchor. ───
    // Linked anchor->n0->n1->...->tip with ROPE constraints (length-limited
    // distance). The final link is an ELASTIC spring so both kinds show.
    constexpr u32 kChainLinks = 5;
    constexpr f32 kSeg = 0.42f;  // segment length (m)
    u32 prev = anchor;
    math::Vec3 p = anchor_pos;
    for (u32 i = 0; i < kChainLinks; ++i) {
        p = math::add(p, math::Vec3{0.10f, -kSeg, 0.0f});  // slight x lean so it swings
        const u32 hue = 60u + i * 30u;
        const u32 cur = add_node(s, p, 0.6f, 0.13f, pack_rgba8(230, hue, 70));
        if (i + 1 < kChainLinks) {
            // ROPE: max distance == segment length, anchors at the bodies.
            s.graph.add(make_rope(prev, cur, {0, 0, 0}, {0, 0, 0}, kSeg));
        } else {
            // ELASTIC: soft spring for the last link (rest length, stiffness,
            // damping). Gives the chain tip a springy bounce.
            s.graph.add(make_elastic(prev, cur, {0, 0, 0}, {0, 0, 0}, kSeg, 220.0f, 6.0f));
        }
        prev = cur;
        info.chain_tip = cur;
    }

    // ── Ragdoll: a jointed body dangling beside the chain. ──────────────
    // Hung from a second fixed point so it dangles instead of free-falling.
    // The joints form a clean tree rooted at the fixed hang point, and use
    // the remaining four kinds so all six appear in the graph:
    //   hang =WELD=> head =BALL=> torso =SLIDER=> pelvis
    //   torso =BALL=> {l_arm, r_arm}; arm =AXIS=> forearm (elbow hinge)
    //   pelvis =BALL=> {l_leg, r_leg}
    const math::Vec3 hang_pos{1.9f, 4.4f, 0.0f};
    const u32 hang = add_node(s, hang_pos, 0.0f, 0.10f, pack_rgba8(180, 180, 190));

    const u32 head =
        add_node(s, math::add(hang_pos, {0.0f, -0.45f, 0.0f}), 0.5f, 0.18f, pack_rgba8(240, 200, 160));
    const u32 torso =
        add_node(s, math::add(hang_pos, {0.0f, -1.05f, 0.0f}), 1.4f, 0.22f, pack_rgba8(80, 140, 230));
    const u32 pelvis =
        add_node(s, math::add(hang_pos, {0.0f, -1.70f, 0.0f}), 1.0f, 0.20f, pack_rgba8(70, 110, 200));

    const u32 l_arm =
        add_node(s, math::add(hang_pos, {-0.50f, -1.00f, 0.0f}), 0.4f, 0.12f, pack_rgba8(230, 120, 120));
    const u32 r_arm =
        add_node(s, math::add(hang_pos, {0.50f, -1.00f, 0.0f}), 0.4f, 0.12f, pack_rgba8(230, 120, 120));
    const u32 l_fore =
        add_node(s, math::add(hang_pos, {-0.80f, -1.45f, 0.0f}), 0.3f, 0.10f, pack_rgba8(230, 150, 100));
    const u32 r_fore =
        add_node(s, math::add(hang_pos, {0.80f, -1.45f, 0.0f}), 0.3f, 0.10f, pack_rgba8(230, 150, 100));
    const u32 l_leg =
        add_node(s, math::add(hang_pos, {-0.28f, -2.35f, 0.0f}), 0.6f, 0.13f, pack_rgba8(120, 200, 140));
    const u32 r_leg =
        add_node(s, math::add(hang_pos, {0.28f, -2.35f, 0.0f}), 0.6f, 0.13f, pack_rgba8(120, 200, 140));
    info.ragdoll_head = head;

    // WELD the head to the fixed hang point (rigid attachment, holds the
    // ragdoll up). BALL-SOCKET neck + shoulders + hips (3-DoF about a point).
    s.graph.add(make_weld(hang, head, {0, 0, 0}));
    s.graph.add(make_ball_socket(head, torso, {0, 0, 0}));
    s.graph.add(make_ball_socket(torso, l_arm, {0, 0, 0}));
    s.graph.add(make_ball_socket(torso, r_arm, {0, 0, 0}));
    s.graph.add(make_ball_socket(pelvis, l_leg, {0, 0, 0}));
    s.graph.add(make_ball_socket(pelvis, r_leg, {0, 0, 0}));
    // SLIDER spine: torso<->pelvis, 1-DoF along local Y (telescoping).
    s.graph.add(make_slider(torso, pelvis, {0, 0, 0}, {0, 1, 0}, 0.55f, 0.72f));
    // AXIS elbows: forearm hinges off the upper arm (1-DoF with limits).
    s.graph.add(make_axis(l_arm, l_fore, {0, 0, 0}, {1, 0, 0}, -1.2f, 0.2f));
    s.graph.add(make_axis(r_arm, r_fore, {0, 0, 0}, {1, 0, 0}, -1.2f, 0.2f));

    // The rigid kinds (weld / ball-socket / axis) hold a *fixed* separation:
    // bake the spawn distance into rest_length so the solver pins to it every
    // step (the editor-side constructors leave rest_length = 0). Rope/elastic/
    // slider already carry their own length data and are left untouched.
    for (auto& c : s.graph.mutable_list()) {
        const bool rigid = c.kind == Kind::Weld || c.kind == Kind::BallSocket || c.kind == Kind::Axis;
        if (rigid) {
            c.rest_length = math::length(math::sub(s.nodes[c.body_b].pos, s.nodes[c.body_a].pos));
        }
    }

    return info;
}

// ─── Position-Based Dynamics solve (deterministic) ───────────────────────
// One fixed sub-step at dt. Verlet integrate, then satisfy each authored
// constraint as a distance/length projection. This is a faithful (if
// simplified) interpretation of the six kinds for a hanging demo:
//   * Weld / BallSocket / Axis -> hold anchors coincident (distance 0).
//   * Slider                   -> clamp the inter-body distance to [min,max].
//   * Rope                     -> clamp distance to <= rest_length.
//   * Elastic                  -> pull toward rest_length softly (stiffness).
// Static nodes (inv_mass == 0) never move.
void solve_constraint(Scene& s, const editor::constraints::Constraint& c, f32 dt) {
    using editor::constraints::Kind;
    Node& a = s.nodes[c.body_a];
    Node& b = s.nodes[c.body_b];
    const f32 w_sum = a.inv_mass + b.inv_mass;
    if (w_sum <= 0.0f)
        return;  // both pinned

    math::Vec3 delta = math::sub(b.pos, a.pos);
    f32 dist = math::length(delta);
    if (dist < 1e-6f)
        return;
    const math::Vec3 dir = math::mul(delta, 1.0f / dist);

    // Resolve the rest target + correction strength per kind.
    f32 target = dist;
    f32 stiffness = 1.0f;  // 0..1 fraction of the error to remove this step
    switch (c.kind) {
        case Kind::Weld:
        case Kind::BallSocket:
        case Kind::Axis:
            // Rigid pin: drive the separation back to the spawn distance baked
            // into rest_length at authoring time (see build_scene). This is
            // what keeps the ragdoll hanging from its welded head instead of
            // free-falling. (A full weld would also lock orientation; for a
            // point-mass dangle the distance pin is the load-bearing part, and
            // the ball-socket's 3-DoF rotation is exactly free orientation.)
            target = c.rest_length;
            stiffness = 1.0f;
            break;
        case Kind::Slider: {
            // 1-DoF translation: clamp separation into [min_limit, max_limit].
            const f32 lo = c.min_limit, hi = c.max_limit;
            if (dist < lo)
                target = lo;
            else if (dist > hi)
                target = hi;
            else
                return;  // within slot, no correction
            stiffness = 1.0f;
            break;
        }
        case Kind::Rope:
            // Length-limited: only correct when over the max length.
            if (dist <= c.rest_length)
                return;
            target = c.rest_length;
            stiffness = 1.0f;
            break;
        case Kind::Elastic: {
            // Soft spring toward rest_length. Map stiffness (N/m) + damping to
            // a stable [0,1] correction factor for this sub-step.
            target = c.rest_length;
            const f32 k = 1.0f - std::exp(-c.stiffness * dt * dt);  // implicit-ish
            stiffness = std::clamp(k, 0.0f, 1.0f);
            // Light velocity damping along the spring axis.
            const math::Vec3 va = math::sub(a.pos, a.prev);
            const math::Vec3 vb = math::sub(b.pos, b.prev);
            const f32 rel_v = math::dot(math::sub(vb, va), dir);
            const f32 damp = std::clamp(c.damping * dt, 0.0f, 1.0f) * rel_v;
            a.prev = math::sub(a.prev, math::mul(dir, damp * (a.inv_mass / w_sum)));
            b.prev = math::add(b.prev, math::mul(dir, damp * (b.inv_mass / w_sum)));
            break;
        }
    }

    const f32 err = (dist - target) * stiffness;
    const math::Vec3 corr = math::mul(dir, err / w_sum);
    a.pos = math::add(a.pos, math::mul(corr, a.inv_mass));
    b.pos = math::sub(b.pos, math::mul(corr, b.inv_mass));
}

// One fixed PBD sub-step. dt is the engine's fixed tick (1/120 s).
void step_pbd(Scene& s, math::Vec3 gravity, f32 dt, u32 iterations) {
    // Verlet integrate (implicit velocity = pos - prev).
    for (Node& n : s.nodes) {
        if (n.inv_mass == 0.0f)
            continue;
        const math::Vec3 vel = math::sub(n.pos, n.prev);
        const math::Vec3 next = math::add(math::add(n.pos, vel), math::mul(gravity, dt * dt));
        n.prev = n.pos;
        n.pos = next;
    }
    // Constraint projection iterations (Gauss-Seidel).
    for (u32 it = 0; it < iterations; ++it) {
        for (usize i = 0, e = s.graph.size(); i < e; ++i)
            solve_constraint(s, s.graph.at(i), dt);
    }
}

// ─── Camera ──────────────────────────────────────────────────────────────
struct Camera {
    math::Vec3 origin;
    math::Vec3 forward;
    math::Vec3 right;
    math::Vec3 up;
    f32 fov_tan;
    f32 aspect;
};

Camera make_orbit_camera(f32 t_seconds, f32 aspect) {
    const f32 radius = 8.5f;
    const f32 height = 3.0f;
    const f32 angle = t_seconds * 0.25f;
    const math::Vec3 eye{std::cos(angle) * radius, height, std::sin(angle) * radius};
    const math::Vec3 target{0.0f, 2.6f, 0.0f};
    const math::Vec3 world_up{0.0f, 1.0f, 0.0f};

    const math::Vec3 fwd = math::normalize(math::sub(target, eye));
    const math::Vec3 right = math::normalize(math::cross(fwd, world_up));
    const math::Vec3 up = math::cross(right, fwd);

    Camera c{};
    c.origin = eye;
    c.forward = fwd;
    c.right = right;
    c.up = up;
    c.fov_tan = std::tan(50.0f * math::kDegToRad * 0.5f);
    c.aspect = aspect;
    return c;
}

// Project a world point to pixel space + view-space depth. Returns false if
// the point is behind the camera.
bool project(const Camera& cam, math::Vec3 wp, f32& out_px, f32& out_py, f32& out_depth) {
    const math::Vec3 rel = math::sub(wp, cam.origin);
    const f32 z = math::dot(rel, cam.forward);  // view depth
    if (z <= 1e-3f)
        return false;
    const f32 x = math::dot(rel, cam.right);
    const f32 y = math::dot(rel, cam.up);
    const f32 ndc_x = (x / z) / (cam.aspect * cam.fov_tan);
    const f32 ndc_y = (y / z) / cam.fov_tan;
    out_px = (ndc_x * 0.5f + 0.5f) * static_cast<f32>(kFbW);
    out_py = (0.5f - ndc_y * 0.5f) * static_cast<f32>(kFbH);
    out_depth = z;
    return true;
}

// ─── Software splat renderer (depth-sorted shaded discs + links) ─────────
void clear_bg(std::vector<u32>& px) {
    for (u32 y = 0; y < kFbH; ++y) {
        const f32 t = static_cast<f32>(y) / static_cast<f32>(kFbH);
        const u32 r = clamp_u8(18.0f + t * 22.0f);
        const u32 g = clamp_u8(22.0f + t * 30.0f);
        const u32 b = clamp_u8(34.0f + t * 50.0f);
        const u32 c = pack_rgba8(r, g, b);
        for (u32 x = 0; x < kFbW; ++x)
            px[static_cast<usize>(y) * kFbW + x] = c;
    }
}

void draw_line(std::vector<u32>& px, f32 x0, f32 y0, f32 x1, f32 y1, u32 color) {
    // Simple DDA, clipped to the framebuffer.
    const f32 dx = x1 - x0, dy = y1 - y0;
    const f32 steps = std::max(std::fabs(dx), std::fabs(dy));
    if (steps < 1.0f)
        return;
    const f32 ix = dx / steps, iy = dy / steps;
    f32 cx = x0, cy = y0;
    for (f32 i = 0; i <= steps; i += 1.0f) {
        const i32 px_x = static_cast<i32>(cx);
        const i32 px_y = static_cast<i32>(cy);
        if (px_x >= 0 && px_x < static_cast<i32>(kFbW) && px_y >= 0 && px_y < static_cast<i32>(kFbH)) {
            px[static_cast<usize>(px_y) * kFbW + static_cast<usize>(px_x)] = color;
        }
        cx += ix;
        cy += iy;
    }
}

void draw_disc(std::vector<u32>& px, f32 cx, f32 cy, f32 r, u32 color) {
    if (r < 0.5f)
        r = 0.5f;
    const i32 x0 = std::max(0, static_cast<i32>(std::floor(cx - r)));
    const i32 x1 = std::min(static_cast<i32>(kFbW) - 1, static_cast<i32>(std::ceil(cx + r)));
    const i32 y0 = std::max(0, static_cast<i32>(std::floor(cy - r)));
    const i32 y1 = std::min(static_cast<i32>(kFbH) - 1, static_cast<i32>(std::ceil(cy + r)));
    const f32 r2 = r * r;
    const f32 cr = static_cast<f32>(color & 0xFFu);
    const f32 cg = static_cast<f32>((color >> 8) & 0xFFu);
    const f32 cb = static_cast<f32>((color >> 16) & 0xFFu);
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const f32 ddx = static_cast<f32>(x) + 0.5f - cx;
            const f32 ddy = static_cast<f32>(y) + 0.5f - cy;
            const f32 d2 = ddx * ddx + ddy * ddy;
            if (d2 > r2)
                continue;
            // Cheap spherical shade: brighten toward the upper-left.
            const f32 nz = std::sqrt(std::max(0.0f, 1.0f - d2 / r2));
            const f32 shade =
                0.45f +
                0.55f * std::clamp(0.6f * nz - 0.25f * (ddx / r) - 0.25f * (ddy / r), 0.0f, 1.0f);
            px[static_cast<usize>(y) * kFbW + static_cast<usize>(x)] =
                pack_rgba8(clamp_u8(cr * shade), clamp_u8(cg * shade), clamp_u8(cb * shade));
        }
    }
}

void render_scene(std::vector<u32>& px, const Scene& s, const Camera& cam) {
    clear_bg(px);

    // 1) Draw the constraint links first (behind the nodes), tinted by kind.
    for (usize i = 0, e = s.graph.size(); i < e; ++i) {
        const auto& c = s.graph.at(i);
        f32 ax, ay, ad, bx, by, bd;
        if (!project(cam, s.nodes[c.body_a].pos, ax, ay, ad))
            continue;
        if (!project(cam, s.nodes[c.body_b].pos, bx, by, bd))
            continue;
        u32 link_col = pack_rgba8(120, 120, 130);
        using editor::constraints::Kind;
        switch (c.kind) {
            case Kind::Rope:
                link_col = pack_rgba8(210, 190, 90);
                break;
            case Kind::Elastic:
                link_col = pack_rgba8(110, 230, 230);
                break;
            case Kind::Weld:
                link_col = pack_rgba8(230, 230, 230);
                break;
            case Kind::BallSocket:
                link_col = pack_rgba8(170, 170, 240);
                break;
            case Kind::Axis:
                link_col = pack_rgba8(240, 160, 110);
                break;
            case Kind::Slider:
                link_col = pack_rgba8(160, 240, 170);
                break;
        }
        draw_line(px, ax, ay, bx, by, link_col);
    }

    // 2) Depth-sort the node discs (painter's algorithm, far -> near).
    struct Splat {
        f32 px, py, depth, r;
        u32 color;
    };
    std::vector<Splat> splats;
    splats.reserve(s.nodes.size());
    for (const Node& n : s.nodes) {
        f32 sx, sy, sd;
        if (!project(cam, n.pos, sx, sy, sd))
            continue;
        // Perspective-scale the draw radius by inverse depth.
        const f32 screen_r = n.radius / sd * (static_cast<f32>(kFbH) * 0.5f / cam.fov_tan);
        splats.push_back({sx, sy, sd, screen_r, n.color});
    }
    std::sort(splats.begin(), splats.end(), [](const Splat& a, const Splat& b) {
        return a.depth > b.depth;
    });
    for (const Splat& sp : splats)
        draw_disc(px, sp.px, sp.py, sp.r, sp.color);
}

}  // namespace

int run_sample(const app::AppArgs& base_args, app::WindowApp& app_host) {
    const app::AppArgs& args = base_args;
    const u32 smoke_frames = args.smoke_frames;
    auto* window = &app_host.window();

    // Earth gravity into the engine world (it integrates the bodies; we mirror
    // the same vector into our PBD step so both stay consistent).
    const math::Vec3 gravity{0.0f, -9.81f, 0.0f};
    physics::World::Get().set_gravity(gravity);

    Scene scene{};
    const SceneInfo info = build_scene(scene);

    PSY_LOG_INFO(
        "Psynder sample 08 running{} — {} bodies, {} constraints (rope+elastic chain, "
        "weld/ball-socket/slider/axis ragdoll)",
        smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", smoke_frames) : std::string{},
        scene.nodes.size(),
        scene.graph.size());

    std::vector<u32>& pixels = app_host.pixels();

    const render::Framebuffer& fb = app_host.framebuffer();
    const f32 aspect =
        fb.height == 0u ? 1.0f : static_cast<f32>(fb.width) / static_cast<f32>(fb.height);

    // Fixed-tick contract: engine sim is 120 Hz. Pin dt in smoke mode for
    // determinism (mirrors the other samples' time pinning).
    constexpr f32 kFixedDt = 1.0f / 120.0f;
    constexpr u32 kPbdIterations = 12;  // Gauss-Seidel passes per sub-step

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;

    while (!window->should_close()) {
        window->poll_events();

        // ESC quits — unless the console is open, where Esc closes it instead.
        if (auto* in = platform::input();
            in && in->key_down(platform::KeyCode::Escape) && !editor::overlays_capturing()) {
            break;
        }

        // Editor F2/~ toggle + PLAY/EDIT badge. EDIT mode freezes the sim so
        // the hanging pose can be inspected.
        const editor::Mode edit_mode = app_host.engine_frame_update(kFixedDt);

        if (edit_mode != editor::Mode::Edit) {
            // Advance both the engine world (exercises step) and the PBD
            // solver that drives the authored constraint graph. One fixed
            // sub-step per frame keeps smoke runs deterministic.
            physics::World::Get().step(kFixedDt);
            step_pbd(scene, gravity, kFixedDt, kPbdIterations);
        }

        // Camera time: pinned to frame index in smoke mode.
        const f32 t =
            (edit_mode == editor::Mode::Edit) ? 0.0f
            : smoke_frames > 0
                ? static_cast<f32>(frame) * kFixedDt
                : static_cast<f32>(platform::Clock::seconds(platform::Clock::ticks_now() - t0));
        const Camera cam = make_orbit_camera(t, aspect);

        render_scene(pixels, scene, cam);
        app_host.engine_frame_post();
        app_host.present();

        if (smoke_frames > 0) {
            // Read the static anchor pose back THROUGH the engine to prove the
            // read path, and log the live chain-tip / ragdoll-head positions
            // so the smoke output is meaningful (and would expose a NaN blow-up).
            const math::Vec3 anchor =
                physics::World::Get().get_position(scene.nodes[info.chain_anchor].body);
            const math::Vec3 tip = scene.nodes[info.chain_tip].pos;
            const math::Vec3 head = scene.nodes[info.ragdoll_head].pos;
            PSY_LOG_INFO(
                "sample_08: frame {} anchor=({:.3f},{:.3f},{:.3f}) "
                "chain_tip=({:.3f},{:.3f},{:.3f}) "
                "ragdoll_head=({:.3f},{:.3f},{:.3f})",
                frame,
                anchor.x,
                anchor.y,
                anchor.z,
                tip.x,
                tip.y,
                tip.z,
                head.x,
                head.y,
                head.z);
        }

        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("sample_08: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    const bool capture_ok = app_host.write_capture_if_requested("sample_08");

    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct PhysConstraintsSample {
    static constexpr std::string_view log_name() noexcept { return "sample_08"; }
    static constexpr const char* display_name = "Psynder sample 08 (constraints / ragdoll)";

    int run(app::WindowApp& app_host, const app::AppArgs& args) {
        return run_sample(args, app_host);
    }
};

PSYNDER_WINDOW_SAMPLE_MAIN(PhysConstraintsSample)
