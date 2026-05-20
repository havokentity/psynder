// SPDX-License-Identifier: MIT
// Psynder — reusable first-person / free-cam character controller shared by
// the sample binaries. Header-only, zero deps beyond the public engine API
// (platform::Input, console::Console, math::). Owned by lane 25
// (samples-tests). Lives under samples/common/ so any sample can
// `#include "common/CharacterController.h"` and get a consistent camera.
//
// Two modes (toggled at runtime with a key, default `V`):
//
//   Fps     — gravity-grounded walk. The eye is pinned to floor_y + eye_height
//             every frame; WASD moves on the XZ plane relative to yaw (pitch
//             never flies you up/down). Jumping is intentionally out of scope.
//   FreeCam — six-degrees-of-freedom fly. WASD + up/down keys move along the
//             full view basis (pitch included), no gravity, a faster speed.
//
// Mouse-look drives yaw + pitch; pitch is clamped to ~±89° so you can't flip
// over the pole. World bounds are an axis-aligned box (math::Aabb): the eye
// position is clamped inside it so you can never leave the level.
//
// Console cvar `noclip` (bool, default 0) disables the bounds clamp AND
// gravity/floor-pin, letting you fly through walls and out of the world. It
// is registered lazily on first construction (mirrors the lazy
// RegisterCVar pattern in engine/render/raster/Raster.cpp) so merely
// including this header costs nothing until a controller exists.
//
// The mode-toggle key deliberately avoids F2 / `~` (Tilde), which the editor
// hot-key watcher (engine/editor/core/HotKey.h) reserves for Play/Edit.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "core/console/Console.h"

#include <algorithm>
#include <cmath>

namespace psynder::samples {

// Per-controller tuning. All distances are metres, speeds m/s, angles rad.
// Defaults are sane for the human-scale rooms the samples build.
struct CharacterControllerConfig {
    f32 eye_height = 1.6f;                    // FPS eye offset above floor_y
    f32 floor_y = 0.0f;                       // ground plane the FPS eye rides on
    f32 walk_speed = 3.0f;                    // FPS planar speed
    f32 fly_speed = 6.0f;                     // FreeCam speed (faster than walking)
    f32 mouse_sensitivity = 0.0025f;          // radians per pixel of mouse delta
    f32 pitch_limit = math::kHalfPi - 0.05f;  // ~±89°
    // Keeps the eye from clipping the exact AABB faces; the clamp shrinks the
    // box by this margin on every axis so you stop just shy of the geometry.
    f32 bounds_skin = 0.05f;
    platform::KeyCode toggle_key = platform::KeyCode::V;  // avoids F2 / `~`
};

enum class ControllerMode : u8 {
    Fps,      // gravity-grounded walk
    FreeCam,  // six-DoF fly
};

class CharacterController {
   public:
    CharacterController() noexcept { ensure_noclip_cvar(); }

    explicit CharacterController(const CharacterControllerConfig& cfg) noexcept : cfg_(cfg) {
        ensure_noclip_cvar();
        if (mode_ == ControllerMode::Fps)
            pos_.y = cfg_.floor_y + cfg_.eye_height;
    }

    // ─── Setup ────────────────────────────────────────────────────────────
    void set_config(const CharacterControllerConfig& cfg) noexcept { cfg_ = cfg; }
    const CharacterControllerConfig& config() const noexcept { return cfg_; }

    // World box the eye is clamped inside (unless `noclip`). Pass the level
    // bounds; an empty/zero box disables clamping.
    void set_bounds(const math::Aabb& bounds) noexcept { bounds_ = bounds; }
    const math::Aabb& bounds() const noexcept { return bounds_; }

    void set_mode(ControllerMode m) noexcept {
        mode_ = m;
        if (mode_ == ControllerMode::Fps)
            pos_.y = cfg_.floor_y + cfg_.eye_height;
    }
    ControllerMode mode() const noexcept { return mode_; }

    // Teleport the eye and (optionally) reset the look angles. Bounds are not
    // applied here so callers can stage a deterministic start pose.
    void set_position(math::Vec3 eye) noexcept { pos_ = eye; }
    void set_look(f32 yaw_rad, f32 pitch_rad) noexcept {
        yaw_ = yaw_rad;
        pitch_ = std::clamp(pitch_rad, -cfg_.pitch_limit, cfg_.pitch_limit);
    }

    // ─── State ────────────────────────────────────────────────────────────
    math::Vec3 eye() const noexcept { return pos_; }
    f32 yaw() const noexcept { return yaw_; }
    f32 pitch() const noexcept { return pitch_; }

    // Unit forward vector from the current yaw/pitch. yaw == pitch == 0 looks
    // down -Z (matches the right-handed sample convention).
    math::Vec3 forward() const noexcept {
        const f32 cy = std::cos(yaw_), sy = std::sin(yaw_);
        const f32 cp = std::cos(pitch_), sp = std::sin(pitch_);
        return {sy * cp, sp, -cy * cp};
    }

    // Right-handed view matrix looking down `forward()` from the eye.
    math::Mat4 view_matrix() const noexcept {
        return math::look_at_rh(pos_, math::add(pos_, forward()), math::Vec3{0, 1, 0});
    }

    bool noclip() const noexcept { return noclip_cvar_ && noclip_cvar_->GetBool(); }

    // ─── Per-frame integration ────────────────────────────────────────────
    // Reads mouse + keys off `input`, advances the eye by `dt` seconds. Edge-
    // triggered toggle key flips Fps <-> FreeCam. Honors the `noclip` cvar.
    void update(const platform::Input& input, f32 dt) noexcept {
        if (input.key_pressed(cfg_.toggle_key))
            set_mode(mode_ == ControllerMode::Fps ? ControllerMode::FreeCam : ControllerMode::Fps);

        // Mouse-look: yaw follows +dx, pitch follows -dy (screen-down lowers
        // the gaze), clamped shy of the poles.
        const platform::MouseState& m = input.mouse();
        yaw_ += m.dx * cfg_.mouse_sensitivity;
        pitch_ -= m.dy * cfg_.mouse_sensitivity;
        pitch_ = std::clamp(pitch_, -cfg_.pitch_limit, cfg_.pitch_limit);

        const bool fly = (mode_ == ControllerMode::FreeCam) || noclip();
        const f32 speed = fly ? cfg_.fly_speed : cfg_.walk_speed;

        // Movement basis. FreeCam/noclip move along the full pitched view so
        // you can fly toward where you look; FPS walks on the yaw-only plane.
        math::Vec3 fwd, right;
        if (fly) {
            fwd = forward();
            right = math::normalize(math::cross(fwd, math::Vec3{0, 1, 0}));
        } else {
            fwd = math::Vec3{std::sin(yaw_), 0.0f, -std::cos(yaw_)};
            right = math::Vec3{std::cos(yaw_), 0.0f, std::sin(yaw_)};
        }

        math::Vec3 move{0, 0, 0};
        if (input.key_down(platform::KeyCode::W))
            move = math::add(move, fwd);
        if (input.key_down(platform::KeyCode::S))
            move = math::sub(move, fwd);
        if (input.key_down(platform::KeyCode::D))
            move = math::add(move, right);
        if (input.key_down(platform::KeyCode::A))
            move = math::sub(move, right);

        // Vertical fly controls (FreeCam / noclip only). Space rises, Ctrl
        // sinks — chosen so they don't collide with WASD or the toggle key.
        if (fly) {
            if (input.key_down(platform::KeyCode::Space))
                move = math::add(move, math::Vec3{0, 1, 0});
            if (input.key_down(platform::KeyCode::LeftCtrl) ||
                input.key_down(platform::KeyCode::RightCtrl))
                move = math::sub(move, math::Vec3{0, 1, 0});
        }

        if (move.x != 0.0f || move.y != 0.0f || move.z != 0.0f) {
            move = math::normalize(move);
            pos_ = math::add(pos_, math::mul(move, speed * dt));
        }

        // Ground pin: FPS (when not flying) always rides the floor plane.
        if (mode_ == ControllerMode::Fps && !noclip())
            pos_.y = cfg_.floor_y + cfg_.eye_height;

        // World clamp: skip entirely when noclip is set so you can leave the
        // level. A zero-sized box (default) also disables the clamp.
        if (!noclip())
            clamp_to_bounds();
    }

   private:
    void clamp_to_bounds() noexcept {
        const bool empty = bounds_.min.x >= bounds_.max.x && bounds_.min.y >= bounds_.max.y &&
                           bounds_.min.z >= bounds_.max.z;
        if (empty)
            return;
        const f32 s = cfg_.bounds_skin;
        // Clamp each axis independently; guard against a skin larger than the
        // box extent by collapsing to the box centre on that axis.
        auto clamp_axis = [s](f32 v, f32 lo, f32 hi) noexcept {
            const f32 a = lo + s;
            const f32 b = hi - s;
            if (a > b)
                return 0.5f * (lo + hi);
            return std::clamp(v, a, b);
        };
        pos_.x = clamp_axis(pos_.x, bounds_.min.x, bounds_.max.x);
        pos_.y = clamp_axis(pos_.y, bounds_.min.y, bounds_.max.y);
        pos_.z = clamp_axis(pos_.z, bounds_.min.z, bounds_.max.z);
    }

    // Lazily register (or look up) the `noclip` cvar exactly once and cache
    // the pointer. Idempotent across many controllers / repeated includes:
    // RegisterCVar returns the existing entry if the name is already known.
    void ensure_noclip_cvar() noexcept {
        auto& console = console::Console::Get();
        noclip_cvar_ = console.FindCVar("noclip");
        if (!noclip_cvar_) {
            noclip_cvar_ = console.RegisterCVar(
                "noclip",
                "0",
                "Disable character-controller world-bounds collision + gravity "
                "(free flythrough). 0 = collide, 1 = noclip.",
                console::CVAR_CHEAT);
        }
    }

    CharacterControllerConfig cfg_{};
    math::Aabb bounds_{};  // zero box => no clamp
    ControllerMode mode_ = ControllerMode::Fps;
    math::Vec3 pos_{0.0f, 1.6f, 0.0f};
    f32 yaw_ = 0.0f;
    f32 pitch_ = 0.0f;
    console::CVar* noclip_cvar_ = nullptr;
};

}  // namespace psynder::samples
