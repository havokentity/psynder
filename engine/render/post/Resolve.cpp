// SPDX-License-Identifier: MIT
// Psynder — resolve pass. HDR-linear float4 → LDR sRGB packed RGBA8/BGRA8.
// Lane 09. Owns the public symbol psynder::render::post::resolve().
//
// The src framebuffer is interpreted as 16 bytes-per-pixel (R,G,B,A float)
// regardless of its `format` enum slot — the public Framebuffer enum is
// frozen and does not (yet) carry an HDR float entry, so this lane defines
// the convention: src_hdr.pixels points at a tightly packed float4 image
// with src_hdr.pitch == src_hdr.width * 16. Callers that violate this
// invariant get garbage out; debug builds assert.
//
// The destination is RGBA8 or BGRA8 (8-bit per channel). Other formats fall
// through to the no-op path until lane 07 expands the PixelFormat enum.

#include "Post.h"
#include "Internal.h"

#include "core/console/Console.h"
#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace psynder::render::post {

// ─── User-facing cvars ───────────────────────────────────────────────────
// Three archive cvars surfaced by this lane. They're file-scoped to the
// Resolve TU so that any link reference to resolve() pulls the
// registrations in. (PSY_CVAR is a static initializer; a separate
// Cvars.cpp got dropped by the linker because nothing referenced it.)
namespace {

PSY_CVAR(r_dither, "0", "Post-resolve dither mode: 0=off, 1=Bayer 4x4, 2=blue-noise.", console::CVarFlags::Archive);

PSY_CVAR(r_scanline, "0", "Enable retro CRT scanline filter (0/1).", console::CVarFlags::Archive);

PSY_CVAR(r_scanline_strength,
         "0.2",
         "Scanline darkening strength on odd rows (0..1).",
         console::CVarFlags::Archive);

detail::DitherMode parse_dither_cvar(const console::CVar* cv) noexcept {
    if (!cv)
        return detail::DitherMode::Off;
    const int v = cv->GetInt();
    switch (v) {
        case 1:
            return detail::DitherMode::Bayer;
        case 2:
            return detail::DitherMode::Blue;
        default:
            return detail::DitherMode::Off;
    }
}

const console::CVar* dither_cvar() noexcept {
    // Late-binding lookup — Cvars.cpp does the actual PSY_CVAR registration.
    return console::Console::Get().FindCVar("r_dither");
}

PSY_FORCEINLINE u32 pack_rgba8(u8 r, u8 g, u8 b, u8 a) noexcept {
    // Little-endian RGBA8 = byte order R,G,B,A in memory.
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(a) << 24);
}

PSY_FORCEINLINE u32 pack_bgra8(u8 r, u8 g, u8 b, u8 a) noexcept {
    return static_cast<u32>(b) | (static_cast<u32>(g) << 8) | (static_cast<u32>(r) << 16) |
           (static_cast<u32>(a) << 24);
}

}  // namespace

void resolve(const Framebuffer& src_hdr, Framebuffer& dst_ldr, const ResolveParams& params) {
    if (!src_hdr.pixels || !dst_ldr.pixels)
        return;
    if (src_hdr.width == 0 || src_hdr.height == 0)
        return;
    if (src_hdr.width != dst_ldr.width || src_hdr.height != dst_ldr.height) {
        PSY_LOG_WARN("post/resolve: src/dst dimensions mismatch ({}x{} vs {}x{}); skipping",
                     src_hdr.width,
                     src_hdr.height,
                     dst_ldr.width,
                     dst_ldr.height);
        return;
    }
    const bool is_rgba = dst_ldr.format == PixelFormat::RGBA8;
    const bool is_bgra = dst_ldr.format == PixelFormat::BGRA8;
    if (!is_rgba && !is_bgra) {
        // Paletted8 / RGB565 not currently implemented in this lane; lane 07
        // owns the format roster. Bail rather than corrupt the buffer.
        return;
    }

    const detail::DitherMode dmode =
        params.dither ? parse_dither_cvar(dither_cvar()) : detail::DitherMode::Off;
    // If the user enabled dither but no cvar exists, fall back to Bayer.
    const detail::DitherMode emode =
        (params.dither && dmode == detail::DitherMode::Off) ? detail::DitherMode::Bayer : dmode;

    const f32 exposure = params.exposure;
    const f32 gamma = params.gamma > 0.0f ? params.gamma : 2.2f;

    const u32 w = src_hdr.width;
    const u32 h = src_hdr.height;

    const auto* src_bytes = src_hdr.pixels;
    const usize src_pitch_b = src_hdr.pitch ? src_hdr.pitch : (w * sizeof(detail::HdrPixel));

    for (u32 y = 0; y < h; ++y) {
        const auto* row_src =
            reinterpret_cast<const detail::HdrPixel*>(src_bytes + static_cast<usize>(y) * src_pitch_b);
        auto* row_dst = reinterpret_cast<u32*>(dst_ldr.pixels + static_cast<usize>(y) * dst_ldr.pitch);

        for (u32 x = 0; x < w; ++x) {
            const detail::HdrPixel p = row_src[x];
            f32 r = p.r;
            f32 g = p.g;
            f32 b = p.b;
            f32 a = p.a;

            if (params.tonemap_reinhard) {
                r = detail::reinhard(r, exposure);
                g = detail::reinhard(g, exposure);
                b = detail::reinhard(b, exposure);
            } else {
                r = std::clamp(r * exposure, 0.0f, 1.0f);
                g = std::clamp(g * exposure, 0.0f, 1.0f);
                b = std::clamp(b * exposure, 0.0f, 1.0f);
            }

            r = detail::linear_to_srgb(r, gamma);
            g = detail::linear_to_srgb(g, gamma);
            b = detail::linear_to_srgb(b, gamma);

            const f32 th =
                (emode == detail::DitherMode::Off) ? 0.5f : detail::dither_threshold(emode, x, y);

            const u8 r8 = detail::quantize_u8(r, th);
            const u8 g8 = detail::quantize_u8(g, th);
            const u8 b8 = detail::quantize_u8(b, th);
            const u8 a8 = detail::quantize_u8(std::clamp(a, 0.0f, 1.0f), 0.5f);

            const u32 packed = is_rgba ? pack_rgba8(r8, g8, b8, a8) : pack_bgra8(r8, g8, b8, a8);

            // Streaming store — destination is written once per frame and
            // read elsewhere (platform present), so we bypass the L1/L2 per
            // DESIGN §4.3. Falls back to a plain store on scalar targets.
            detail::stream_store_u32(row_dst + x, packed);
        }
    }
    detail::stream_fence();
}

}  // namespace psynder::render::post
