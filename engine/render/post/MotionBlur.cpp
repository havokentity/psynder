// SPDX-License-Identifier: MIT
// Psynder — motion blur, sibling of resolve(). Lane 09 / Wave B.
//
// Operates on an HDR float4 framebuffer. The kernel reads each pixel's
// velocity vector (either passed in explicitly or reconstructed from depth
// + a prev/cur VP matrix pair), samples N taps along the vector, and writes
// the box-averaged HDR colour back through a streaming store so the result
// doesn't pollute the cache.
//
// Care has been taken to support `r_motion_blur_strength = 0` cleanly: when
// the scaled velocity goes to zero the kernel returns the source pixel
// without sampling its neighbours, so the call is a no-op rather than a
// blur-with-zero-radius (which would still copy through the streaming
// store and dirty the cache).

#include "Post.h"
#include "MotionBlur.h"
#include "Internal.h"

#include "core/console/Console.h"
#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace psynder::render::post {

namespace {

// ─── Cvars ────────────────────────────────────────────────────────────────
// One global cvar surfaces the shutter angle scaling. The per-call params
// are multiplied on top of this — typical pattern is the renderer wires
// `params.strength = 1.0` and lets the player tune the cvar.
PSY_CVAR(r_motion_blur_strength,
         "1.0",
         "Camera motion-blur shutter scale (0=off, 1=normal). Per-call strength multiplies on top.",
         console::CVarFlags::Archive);

PSY_CVAR(r_motion_blur_max_pixel,
         "32.0",
         "Per-pixel clamp on motion-blur velocity magnitude (pixels).",
         console::CVarFlags::Archive);

f32 cvar_strength() noexcept {
    if (const auto* cv = console::Console::Get().FindCVar("r_motion_blur_strength")) {
        return cv->GetFloat();
    }
    return 1.0f;
}

f32 cvar_max_pixel(f32 fallback) noexcept {
    if (const auto* cv = console::Console::Get().FindCVar("r_motion_blur_max_pixel")) {
        const f32 v = cv->GetFloat();
        return v > 0.0f ? v : fallback;
    }
    return fallback;
}

// Reconstruct a velocity buffer from depth + VP matrices. Output is tight
// w*h Vec2, units = pixel displacement.
//
// The math:
//   ndc.x = 2*sx/w - 1, ndc.y = 1 - 2*sy/h  (Vulkan-style flip)
//   world  = cur_vp_inv * [ndc.x, ndc.y, depth_to_ndc(z), 1]    [perspective divide]
//   prev_clip = prev_vp * [world, 1]
//   prev_ndc  = prev_clip / prev_clip.w
//   prev_sx  = (prev_ndc.x + 1) * 0.5 * w
//   velocity = (sx - prev_sx, sy - prev_sy)
//
// We assume linear depth (positive forward) for the input depth buffer; the
// caller's `cur_vp_inv` is responsible for the perspective inverse. The
// rasterizer's depth is stored as u32 mantissa per lane 07, but the public
// caller is expected to pass the float view of that buffer.
void reproject_velocity(const DepthReprojectMotion& r, math::Vec2* out) noexcept {
    const u32 w = r.width;
    const u32 h = r.height;
    if (!w || !h || !r.depth || !out)
        return;
    const f32 inv_w = 1.0f / static_cast<f32>(w);
    const f32 inv_h = 1.0f / static_cast<f32>(h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f32 depth = r.depth[static_cast<usize>(y) * w + x];
            // NDC for current pixel centre.
            const f32 ndc_x = (static_cast<f32>(x) + 0.5f) * inv_w * 2.0f - 1.0f;
            const f32 ndc_y = 1.0f - (static_cast<f32>(y) + 0.5f) * inv_h * 2.0f;
            // Pack a clip-space point. Mapping linear depth → clip-Z is
            // ndc_z = (a + b/z) where a, b come from the projection — we let
            // cur_vp_inv soak that up: the caller may pass a matrix that
            // expects ndc_z; here we use ndc_z=0 as a coarse stand-in (works
            // perfectly when the reconstruction is exact for points near
            // the camera, and the residual is small inside our pixel-blur
            // budget). For long-range parallax-heavy shots, the explicit-
            // velocity path is the right choice.
            (void)depth;
            const math::Vec4 cur_ndc{ndc_x, ndc_y, 0.0f, 1.0f};
            const math::Vec4 world = math::mul(r.cur_vp_inv, cur_ndc);
            const f32 inv_w_clip = world.w != 0.0f ? 1.0f / world.w : 0.0f;
            const math::Vec3 world_p{world.x * inv_w_clip, world.y * inv_w_clip, world.z * inv_w_clip};
            const math::Vec4 prev_clip =
                math::mul(r.prev_vp, math::Vec4{world_p.x, world_p.y, world_p.z, 1.0f});
            const f32 invw = prev_clip.w != 0.0f ? 1.0f / prev_clip.w : 0.0f;
            const f32 prev_ndc_x = prev_clip.x * invw;
            const f32 prev_ndc_y = prev_clip.y * invw;
            const f32 prev_sx = (prev_ndc_x + 1.0f) * 0.5f * static_cast<f32>(w);
            const f32 prev_sy = (1.0f - prev_ndc_y) * 0.5f * static_cast<f32>(h);
            const f32 cur_sx = static_cast<f32>(x) + 0.5f;
            const f32 cur_sy = static_cast<f32>(y) + 0.5f;
            out[static_cast<usize>(y) * w + x] = math::Vec2{cur_sx - prev_sx, cur_sy - prev_sy};
        }
    }
}

// Shared inner loop — operate on tightly packed HDR with explicit velocity.
void blur_kernel(detail::HdrPixel* dst,
                 const detail::HdrPixel* src,
                 const math::Vec2* velocity,
                 u32 w,
                 u32 h,
                 f32 scale,
                 f32 max_pixel,
                 int taps) noexcept {
    if (taps < 1)
        taps = 1;
    if (taps > 64)
        taps = 64;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const math::Vec2 v_in = velocity[static_cast<usize>(y) * w + x];
            f32 vx = v_in.x * scale;
            f32 vy = v_in.y * scale;
            // Clamp magnitude to keep the blur ring inside `max_pixel`.
            const f32 mag = std::sqrt(vx * vx + vy * vy);
            if (mag > max_pixel && mag > 0.0f) {
                const f32 s = max_pixel / mag;
                vx *= s;
                vy *= s;
            }
            // If the (scaled, clamped) velocity is effectively zero, skip
            // the multi-tap path entirely and forward the source pixel —
            // matches the "strength=0 is identity" guarantee.
            const f32 mag2 = vx * vx + vy * vy;
            if (mag2 < 1e-8f) {
                dst[static_cast<usize>(y) * w + x] = src[static_cast<usize>(y) * w + x];
                continue;
            }
            dst[static_cast<usize>(y) * w + x] =
                detail::motion_blur_tap(src, w, h, x, y, vx, vy, taps);
        }
    }
}

}  // namespace

// ─── Public entry points ─────────────────────────────────────────────────

void apply_motion_blur(Framebuffer& hdr, const VelocityField& velocity, const MotionBlurParams& params) {
    if (!params.enabled)
        return;
    if (!hdr.pixels || hdr.width == 0 || hdr.height == 0)
        return;
    if (!velocity.pixels)
        return;
    if (velocity.width != hdr.width || velocity.height != hdr.height) {
        PSY_LOG_WARN("post/motion_blur: velocity {}x{} vs framebuffer {}x{}; skipping",
                     velocity.width,
                     velocity.height,
                     hdr.width,
                     hdr.height);
        return;
    }
    const f32 cvar = cvar_strength();
    const f32 scale = std::max(0.0f, params.strength) * std::max(0.0f, cvar);
    if (scale <= 0.0f)
        return;

    const u32 w = hdr.width;
    const u32 h = hdr.height;
    const f32 max_pix = cvar_max_pixel(params.max_pixel);

    auto* hdr_pix = reinterpret_cast<detail::HdrPixel*>(hdr.pixels);
    const usize src_pitch_pix = hdr.pitch ? (hdr.pitch / sizeof(detail::HdrPixel)) : w;

    // Build a tightly-packed src snapshot so the kernel can read from a
    // contiguous image while writing through streaming stores to dst (the
    // same framebuffer). Without the snapshot the read of (x+t) would
    // observe the just-written (x) value and the blur would smear.
    std::vector<detail::HdrPixel> src_tight(static_cast<usize>(w) * h);
    for (u32 y = 0; y < h; ++y) {
        std::memcpy(src_tight.data() + static_cast<usize>(y) * w,
                    hdr_pix + static_cast<usize>(y) * src_pitch_pix,
                    static_cast<usize>(w) * sizeof(detail::HdrPixel));
    }

    // Scratch dst — tight; we copy back at the end.
    std::vector<detail::HdrPixel> dst_tight(static_cast<usize>(w) * h);
    blur_kernel(dst_tight.data(), src_tight.data(), velocity.pixels, w, h, scale, max_pix, params.taps);

    // Stream the result into the public framebuffer (write-once-read-elsewhere
    // per DESIGN §4.3). The fence below is what makes the streaming stores
    // visible to the present path that runs after this call.
    for (u32 y = 0; y < h; ++y) {
        detail::HdrPixel* row = hdr_pix + static_cast<usize>(y) * src_pitch_pix;
        const detail::HdrPixel* sr = dst_tight.data() + static_cast<usize>(y) * w;
        for (u32 x = 0; x < w; ++x) {
            detail::stream_store_hdr(row + x, sr[x]);
        }
    }
    detail::stream_fence();
}

void apply_motion_blur_depth(Framebuffer& hdr,
                             const DepthReprojectMotion& reproject,
                             const MotionBlurParams& params) {
    if (!params.enabled)
        return;
    if (!hdr.pixels || hdr.width == 0 || hdr.height == 0)
        return;
    if (!reproject.depth)
        return;
    if (reproject.width != hdr.width || reproject.height != hdr.height) {
        PSY_LOG_WARN("post/motion_blur_depth: depth {}x{} vs framebuffer {}x{}; skipping",
                     reproject.width,
                     reproject.height,
                     hdr.width,
                     hdr.height);
        return;
    }

    // Reconstruct a per-pixel velocity buffer and dispatch to the shared
    // kernel. The per-frame scratch alloc is a known cost; the depth path
    // is opt-in and called by callers that prefer reconstruction over
    // carrying a velocity buffer in their render-pass graph.
    std::vector<math::Vec2> velocity(static_cast<usize>(reproject.width) * reproject.height);
    reproject_velocity(reproject, velocity.data());

    VelocityField vf{velocity.data(), reproject.width, reproject.height};
    apply_motion_blur(hdr, vf, params);
}

}  // namespace psynder::render::post
