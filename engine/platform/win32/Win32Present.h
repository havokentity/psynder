// SPDX-License-Identifier: MIT
// Psynder — DXGI flip-model swap chain + scaled-blit pass.
//
// The CPU framebuffer (lane 07 fills, the platform reads) is uploaded to a
// fixed-size D3D11 texture each frame; a single passthrough quad samples
// that texture into the swap chain back buffer with the user-selected
// ScaleMode + AspectMode (DESIGN.md §7.9, §11.1).
//
// The GPU does literally one textured triangle pair per present. No engine
// shader runs here.

#pragma once

#if defined(PSYNDER_PLATFORM_WIN32)

#include "Win32Common.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"

namespace psynder::platform::win32 {

class Win32Present {
public:
    Win32Present()  = default;
    ~Win32Present() { shutdown(); }

    Win32Present(const Win32Present&) = delete;
    Win32Present& operator=(const Win32Present&) = delete;

    // Initialize D3D11 device + swap chain. The swap chain is attached to
    // the given window handle. `render_w`/`render_h` are the framebuffer's
    // *internal* resolution — they fix the upload texture size for the
    // lifetime of the present surface (per §7.9: render resolution does not
    // change on window resize).
    bool init(HWND hwnd, u32 render_w, u32 render_h, bool vsync);

    // Releases the swap chain and the underlying device.
    void shutdown();

    // Called from WM_SIZE — resizes the swap chain buffers. The upload
    // texture (the framebuffer) is unaffected; only the present scale changes.
    void on_window_resize(u32 window_w, u32 window_h);

    // Per-frame entry. Uploads `fb` to the framebuffer texture and draws
    // one quad sized to the current window with the chosen ScaleMode +
    // AspectMode.
    bool present(const render::Framebuffer& fb,
                 ScaleMode  scale_mode,
                 AspectMode aspect_mode);

    u32 window_width()  const noexcept { return window_w_; }
    u32 window_height() const noexcept { return window_h_; }

private:
    // ── Internal helpers ────────────────────────────────────────────────
    bool create_device_and_swap_chain(HWND hwnd);
    bool create_render_target();
    bool create_pipeline();
    bool create_framebuffer_texture(u32 w, u32 h);
    void upload_framebuffer(const render::Framebuffer& fb);

    // Compute destination viewport for the quad given a scale + aspect mode.
    D3D11_VIEWPORT compute_viewport(ScaleMode  scale_mode,
                                    AspectMode aspect_mode) const noexcept;

    // ── State ───────────────────────────────────────────────────────────
    HWND hwnd_     = nullptr;
    u32  render_w_ = 0;
    u32  render_h_ = 0;
    u32  window_w_ = 0;
    u32  window_h_ = 0;
    bool vsync_    = true;

    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    context_;
    ComPtr<IDXGISwapChain1>        swap_chain_;
    ComPtr<ID3D11RenderTargetView> rtv_;

    ComPtr<ID3D11Texture2D>          fb_tex_;
    ComPtr<ID3D11ShaderResourceView> fb_srv_;
    ComPtr<ID3D11SamplerState>       sampler_linear_;
    ComPtr<ID3D11SamplerState>       sampler_point_;

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader>  ps_;
    ComPtr<ID3D11InputLayout>  input_layout_;
    ComPtr<ID3D11Buffer>       vbo_;
    ComPtr<ID3D11RasterizerState> rast_;
};

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
