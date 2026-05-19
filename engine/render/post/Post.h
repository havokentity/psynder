// SPDX-License-Identifier: MIT
// Psynder — post-process / resolve. Lane 09 owns. Tonemap, dither (for
// paletted output), bloom (separable), motion blur (optional), scanline
// filter (retro opt-in).

#pragma once

#include "core/Types.h"
#include "render/Framebuffer.h"

namespace psynder::render::post {

struct ResolveParams {
    bool tonemap_reinhard = true;
    bool dither           = false;
    f32  exposure         = 1.0f;
    f32  gamma            = 2.2f;
};

void resolve(const Framebuffer& src_hdr, Framebuffer& dst_ldr, const ResolveParams& params);

struct BloomParams {
    f32 threshold = 1.0f;
    f32 intensity = 0.4f;
    int passes    = 4;
};

void apply_bloom(Framebuffer& fb, const BloomParams& params);

struct ScanlineParams {
    bool enabled  = false;
    f32  strength = 0.2f;
};

void apply_scanline(Framebuffer& fb, const ScanlineParams& params);

}  // namespace psynder::render::post
