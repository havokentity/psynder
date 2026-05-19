// SPDX-License-Identifier: MIT
// Psynder — camera-velocity motion blur. Lane 09 / Wave B. Sibling header to
// Post.h (which is FROZEN); new APIs introduced for Wave B live here so the
// public surface stays stable.
//
// Two velocity-source paths are offered:
//
//   1. Caller-supplied per-pixel 2-D velocity buffer (Vec2 in NDC-ish [-1,1]
//      space; magnitude expresses pixel displacement scaled by viewport).
//      The renderer drives this when it has both a prev-frame and current-
//      frame VP matrix available.
//
//   2. Reconstruct from depth + a (prev_vp, cur_vp) pair. We unproject each
//      pixel using `cur_vp_inv` and the current depth, then reproject with
//      prev_vp; the screen-space delta is the velocity. This is the path
//      callers use when they don't want to spend the bandwidth on a real
//      velocity buffer (1080p × 8 B = 16 MB/frame is cheap, but optional).
//
// Either way the kernel samples N taps along the velocity vector and box-
// averages the HDR neighbourhood. Shutter angle is `r_motion_blur_strength`
// (a global scale on the velocity magnitude); per-call `strength` multiplies
// on top of the cvar so callers can attenuate the effect for low-motion
// scenes without touching the user's preference.
//
// This pass operates on the HDR float4 framebuffer; it must run BEFORE
// `resolve()` because Reinhard tonemap is nonlinear and would smear the
// post-tonemap byte values in a way that produces dark fringes.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

namespace psynder::render::post {

// Per-pixel velocity buffer. The renderer writes one Vec2 per pixel; the
// magnitude expresses pixel displacement (in pixel units) of the world point
// projected at this screen location since the last frame.
struct VelocityField {
    const math::Vec2* pixels = nullptr;   // tightly packed, width*height entries
    u32               width  = 0;
    u32               height = 0;
};

// User-tunable per-call knobs. `r_motion_blur_strength` is the global cvar
// scale on top of these.
struct MotionBlurParams {
    bool enabled   = true;
    f32  strength  = 1.0f;   // per-call multiplier on top of the cvar; >=0
    int  taps      = 8;      // number of samples along the velocity vector
    f32  max_pixel = 32.0f;  // clamp on velocity magnitude (pixels)
};

// Caller-supplied velocity buffer path.
void apply_motion_blur(Framebuffer& hdr,
                       const VelocityField& velocity,
                       const MotionBlurParams& params);

// Depth-reconstructed velocity path. `depth` is a tight w*h linear-space
// depth buffer (Z in world units; 0 = at near, kept positive per DESIGN.md
// §7 conventions); `cur_vp_inv` un-projects screen+depth to world, `prev_vp`
// re-projects to the previous frame's clip coordinates.
//
// The reconstructed velocity uses the same units (pixels per frame) as the
// explicit-buffer path so the rest of the kernel is shared.
struct DepthReprojectMotion {
    const f32*    depth     = nullptr;   // w*h linear depth
    u32           width     = 0;
    u32           height    = 0;
    math::Mat4    prev_vp{};             // view-projection of the previous frame
    math::Mat4    cur_vp_inv{};          // inverse of the current view-projection
};

void apply_motion_blur_depth(Framebuffer& hdr,
                             const DepthReprojectMotion& reproject,
                             const MotionBlurParams& params);

}  // namespace psynder::render::post
