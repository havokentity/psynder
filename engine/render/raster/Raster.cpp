// SPDX-License-Identifier: MIT
// Psynder — rasterizer stub. Lane 07 implements the tiled sort-middle
// pipeline (vertex transform, triangle setup with Q24.8 edge functions,
// tile binning, per-tile coverage walk, perspective-correct attribute
// interpolation, bilinear/trilinear/aniso sampling, surface cache auto-
// dispatch).

#include "Raster.h"

namespace psynder::render::raster {

Rasterizer& Rasterizer::Get() {
    static Rasterizer r;
    return r;
}

void Rasterizer::begin_frame(const ViewState& /*view*/) {}
void Rasterizer::submit(const DrawItem& /*draw*/)        {}
void Rasterizer::end_frame()                              {}

void clear_framebuffer(Framebuffer& fb, u32 rgba) noexcept {
    if (!fb.pixels || fb.width == 0 || fb.height == 0) return;
    if (fb.format == PixelFormat::RGBA8 || fb.format == PixelFormat::BGRA8) {
        auto* pixels = reinterpret_cast<u32*>(fb.pixels);
        const usize count = static_cast<usize>(fb.width) * fb.height;
        for (usize i = 0; i < count; ++i) pixels[i] = rgba;
    }
}

}  // namespace psynder::render::raster
