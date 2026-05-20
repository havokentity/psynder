// SPDX-License-Identifier: MIT
// Psynder — DXGI flip-model + scaled-blit pass implementation.
//
// Pipeline:
//   1. Create a D3D11 device (flip-model swap chain wants D3D11/12 — we
//      pick 11 because that's all this pass needs).
//   2. Each frame, `upload_framebuffer()` does a `Map(WRITE_DISCARD)` of
//      the upload texture and memcpys the CPU framebuffer in.
//   3. Draw a passthrough textured fullscreen quad sized per ScaleMode +
//      AspectMode. The vertex shader emits clip-space positions and UVs
//      that the pixel shader samples with a bilinear or point sampler.
//   4. `Present(sync_interval, 0)` — sync_interval = vsync ? 1 : 0.
//
// Shaders are compiled at startup with D3DCompile so we don't pull in
// shader source-pack tools at runtime. They're tiny (a passthrough quad).

#include "Win32Present.h"

#if defined(PSYNDER_PLATFORM_WIN32)

#include "core/Log.h"

#include <algorithm>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace psynder::platform::win32 {

namespace {

// ── Shader source (passthrough textured quad) ─────────────────────────────
// We use vertex_id-driven geometry — no IB needed, just 6 vertices via a
// vertex buffer carrying (pos.xy, uv.xy). Keeping the VB explicit makes it
// trivial to recompute UV bounds for aspect modes that crop.

constexpr const char* kVS = R"(
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.uv  = i.uv;
    return o;
}
)";

constexpr const char* kPS = R"(
Texture2D    fb_tex   : register(t0);
SamplerState fb_samp  : register(s0);
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 main(PSIn i) : SV_Target { return fb_tex.Sample(fb_samp, i.uv); }
)";

struct Vertex {
    f32 x, y;
    f32 u, v;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// init / shutdown
// ─────────────────────────────────────────────────────────────────────────

bool Win32Present::init(HWND hwnd, u32 render_w, u32 render_h, bool vsync) {
    hwnd_ = hwnd;
    render_w_ = render_w;
    render_h_ = render_h;
    vsync_ = vsync;

    // Pull initial window dimensions from the HWND so the first present's
    // viewport sizing is correct even before WM_SIZE fires.
    RECT rc{};
    if (::GetClientRect(hwnd, &rc)) {
        window_w_ = static_cast<u32>(std::max(1L, rc.right - rc.left));
        window_h_ = static_cast<u32>(std::max(1L, rc.bottom - rc.top));
    } else {
        window_w_ = render_w_;
        window_h_ = render_h_;
    }

    if (!create_device_and_swap_chain(hwnd))
        return false;
    if (!create_render_target())
        return false;
    if (!create_pipeline())
        return false;
    if (!create_framebuffer_texture(render_w, render_h))
        return false;
    return true;
}

void Win32Present::shutdown() {
    rast_.Reset();
    vbo_.Reset();
    input_layout_.Reset();
    ps_.Reset();
    vs_.Reset();
    sampler_point_.Reset();
    sampler_linear_.Reset();
    fb_srv_.Reset();
    fb_tex_.Reset();
    rtv_.Reset();
    swap_chain_.Reset();
    if (context_)
        context_->ClearState();
    context_.Reset();
    device_.Reset();
}

void Win32Present::on_window_resize(u32 window_w, u32 window_h) {
    window_w = std::max<u32>(window_w, 1);
    window_h = std::max<u32>(window_h, 1);
    if (window_w == window_w_ && window_h == window_h_)
        return;
    window_w_ = window_w;
    window_h_ = window_h;
    if (!swap_chain_)
        return;

    rtv_.Reset();
    const HRESULT hr = swap_chain_->ResizeBuffers(0, window_w_, window_h_, DXGI_FORMAT_UNKNOWN, 0);
    if (!psy_hr_ok(hr, "IDXGISwapChain::ResizeBuffers"))
        return;
    create_render_target();
}

// ─────────────────────────────────────────────────────────────────────────
// device + swap chain
// ─────────────────────────────────────────────────────────────────────────

bool Win32Present::create_device_and_swap_chain(HWND hwnd) {
    UINT flags = 0;
#if !defined(NDEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = ::D3D11CreateDevice(nullptr,
                                     D3D_DRIVER_TYPE_HARDWARE,
                                     nullptr,
                                     flags,
                                     levels,
                                     _countof(levels),
                                     D3D11_SDK_VERSION,
                                     device_.GetAddressOf(),
                                     &got,
                                     context_.GetAddressOf());
    if (FAILED(hr)) {
        // Retry without the debug layer (the Win SDK runtime might not be installed).
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = ::D3D11CreateDevice(nullptr,
                                 D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr,
                                 flags,
                                 levels,
                                 _countof(levels),
                                 D3D11_SDK_VERSION,
                                 device_.GetAddressOf(),
                                 &got,
                                 context_.GetAddressOf());
    }
    if (!psy_hr_ok(hr, "D3D11CreateDevice"))
        return false;

    ComPtr<IDXGIDevice> dxgi_device;
    if (!psy_hr_ok(device_.As(&dxgi_device), "QI IDXGIDevice"))
        return false;

    ComPtr<IDXGIAdapter> adapter;
    if (!psy_hr_ok(dxgi_device->GetAdapter(adapter.GetAddressOf()), "IDXGIDevice::GetAdapter"))
        return false;

    ComPtr<IDXGIFactory2> factory;
    if (!psy_hr_ok(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())),
                   "IDXGIAdapter::GetParent"))
        return false;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = window_w_;
    scd.Height = window_h_;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags = 0;

    if (!psy_hr_ok(factory->CreateSwapChainForHwnd(device_.Get(),
                                                   hwnd,
                                                   &scd,
                                                   nullptr,
                                                   nullptr,
                                                   swap_chain_.GetAddressOf()),
                   "CreateSwapChainForHwnd")) {
        return false;
    }

    // Disable Alt+Enter exclusive-fullscreen — DESIGN §7.9 mandates the
    // platform never changes render resolution, so we handle fullscreen
    // via window-style toggle inside Win32Window, not via DXGI exclusive mode.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool Win32Present::create_render_target() {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (!psy_hr_ok(swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf())),
                   "SwapChain::GetBuffer"))
        return false;
    if (!psy_hr_ok(device_->CreateRenderTargetView(back_buffer.Get(), nullptr, rtv_.GetAddressOf()),
                   "CreateRenderTargetView"))
        return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// pipeline (passthrough quad)
// ─────────────────────────────────────────────────────────────────────────

bool Win32Present::create_pipeline() {
    // Compile shaders. We're targeting fl 11.0 → vs_5_0 / ps_5_0 is fine
    // on every D3D11 driver.
    ComPtr<ID3DBlob> vs_blob, ps_blob, err;
    HRESULT hr = ::D3DCompile(kVS,
                              std::strlen(kVS),
                              "win32_present.vs",
                              nullptr,
                              nullptr,
                              "main",
                              "vs_5_0",
                              0,
                              0,
                              vs_blob.GetAddressOf(),
                              err.GetAddressOf());
    if (FAILED(hr)) {
        const char* msg = err ? static_cast<const char*>(err->GetBufferPointer()) : "(no log)";
        PSY_LOG_ERROR("[win32] vs compile failed: {}", msg);
        return false;
    }
    err.Reset();
    hr = ::D3DCompile(kPS,
                      std::strlen(kPS),
                      "win32_present.ps",
                      nullptr,
                      nullptr,
                      "main",
                      "ps_5_0",
                      0,
                      0,
                      ps_blob.GetAddressOf(),
                      err.GetAddressOf());
    if (FAILED(hr)) {
        const char* msg = err ? static_cast<const char*>(err->GetBufferPointer()) : "(no log)";
        PSY_LOG_ERROR("[win32] ps compile failed: {}", msg);
        return false;
    }
    if (!psy_hr_ok(device_->CreateVertexShader(vs_blob->GetBufferPointer(),
                                               vs_blob->GetBufferSize(),
                                               nullptr,
                                               vs_.GetAddressOf()),
                   "CreateVertexShader"))
        return false;
    if (!psy_hr_ok(device_->CreatePixelShader(ps_blob->GetBufferPointer(),
                                              ps_blob->GetBufferSize(),
                                              nullptr,
                                              ps_.GetAddressOf()),
                   "CreatePixelShader"))
        return false;

    const D3D11_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(f32) * 2, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (!psy_hr_ok(device_->CreateInputLayout(elems,
                                              _countof(elems),
                                              vs_blob->GetBufferPointer(),
                                              vs_blob->GetBufferSize(),
                                              input_layout_.GetAddressOf()),
                   "CreateInputLayout"))
        return false;

    // Two triangles covering NDC space (-1..+1). UVs are (0..1) with V
    // flipped because D3D's texture coordinate origin is top-left while
    // our framebuffer (Y down) is row-major top-to-bottom; the framebuffer
    // pixel at row 0 should appear at the top of the screen. With UV.v
    // running 0..1 top→bottom and our texture row 0 being the top, the
    // mapping is straightforward — but D3D's NDC has +Y up, so we put
    // (-1,+1) as top-left.
    const Vertex verts[] = {
        {-1.0f, 1.0f, 0.0f, 0.0f},   // top-left
        {1.0f, 1.0f, 1.0f, 0.0f},    // top-right
        {-1.0f, -1.0f, 0.0f, 1.0f},  // bot-left
        {-1.0f, -1.0f, 0.0f, 1.0f},  // bot-left
        {1.0f, 1.0f, 1.0f, 0.0f},    // top-right
        {1.0f, -1.0f, 1.0f, 1.0f},   // bot-right
    };
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA srd{};
    srd.pSysMem = verts;
    if (!psy_hr_ok(device_->CreateBuffer(&bd, &srd, vbo_.GetAddressOf()), "CreateBuffer(VBO)"))
        return false;

    // Samplers — bilinear and point. We pick between them per-frame from
    // ScaleMode (Integer also uses point, with viewport-snapped sizing).
    D3D11_SAMPLER_DESC sd_lin{};
    sd_lin.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd_lin.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd_lin.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd_lin.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd_lin.MaxLOD = D3D11_FLOAT32_MAX;
    sd_lin.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (!psy_hr_ok(device_->CreateSamplerState(&sd_lin, sampler_linear_.GetAddressOf()),
                   "CreateSamplerState(linear)"))
        return false;

    D3D11_SAMPLER_DESC sd_pt = sd_lin;
    sd_pt.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (!psy_hr_ok(device_->CreateSamplerState(&sd_pt, sampler_point_.GetAddressOf()),
                   "CreateSamplerState(point)"))
        return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (!psy_hr_ok(device_->CreateRasterizerState(&rd, rast_.GetAddressOf()),
                   "CreateRasterizerState"))
        return false;

    return true;
}

bool Win32Present::create_framebuffer_texture(u32 w, u32 h) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // framebuffer is RGBA8
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (!psy_hr_ok(device_->CreateTexture2D(&td, nullptr, fb_tex_.GetAddressOf()),
                   "CreateTexture2D(framebuffer)"))
        return false;
    if (!psy_hr_ok(device_->CreateShaderResourceView(fb_tex_.Get(), nullptr, fb_srv_.GetAddressOf()),
                   "CreateShaderResourceView(framebuffer)"))
        return false;
    return true;
}

void Win32Present::upload_framebuffer(const render::Framebuffer& fb) {
    if (!fb_tex_ || !fb.pixels)
        return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (!psy_hr_ok(context_->Map(fb_tex_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m),
                   "Map(framebuffer)"))
        return;

    const u32 src_pitch = fb.pitch ? fb.pitch : fb.width * 4;
    const u32 row_bytes = std::min<u32>(src_pitch, m.RowPitch);
    auto* dst = static_cast<u8*>(m.pData);
    const auto* src = fb.pixels;
    for (u32 y = 0; y < fb.height; ++y) {
        std::memcpy(dst + static_cast<usize>(y) * m.RowPitch,
                    src + static_cast<usize>(y) * src_pitch,
                    row_bytes);
    }
    context_->Unmap(fb_tex_.Get(), 0);
}

// ─────────────────────────────────────────────────────────────────────────
// viewport math
// ─────────────────────────────────────────────────────────────────────────

D3D11_VIEWPORT Win32Present::compute_viewport(ScaleMode scale_mode,
                                              AspectMode aspect_mode) const noexcept {
    const f32 ww = static_cast<f32>(window_w_);
    const f32 wh = static_cast<f32>(window_h_);
    const f32 fw = static_cast<f32>(render_w_);
    const f32 fh = static_cast<f32>(render_h_);

    f32 vp_w = ww;
    f32 vp_h = wh;

    switch (aspect_mode) {
        case AspectMode::Stretch:
            // Fill the window, ignore aspect.
            vp_w = ww;
            vp_h = wh;
            break;
        case AspectMode::Letterbox: {
            // Largest rect that preserves fb aspect inside the window.
            const f32 src_aspect = fw / fh;
            const f32 dst_aspect = ww / wh;
            if (dst_aspect > src_aspect) {
                vp_h = wh;
                vp_w = wh * src_aspect;
            } else {
                vp_w = ww;
                vp_h = ww / src_aspect;
            }
            break;
        }
        case AspectMode::Crop: {
            // Smallest rect that covers the window while preserving aspect.
            // Pixels outside the window get scissored away by D3D viewport
            // clipping naturally.
            const f32 src_aspect = fw / fh;
            const f32 dst_aspect = ww / wh;
            if (dst_aspect < src_aspect) {
                vp_h = wh;
                vp_w = wh * src_aspect;
            } else {
                vp_w = ww;
                vp_h = ww / src_aspect;
            }
            break;
        }
    }

    // Integer scaling: snap viewport to integer multiples of (fw, fh) that fit.
    if (scale_mode == ScaleMode::Integer) {
        const u32 sx = std::max<u32>(1, static_cast<u32>(vp_w / fw));
        const u32 sy = std::max<u32>(1, static_cast<u32>(vp_h / fh));
        const u32 s = std::min(sx, sy);
        vp_w = fw * static_cast<f32>(s);
        vp_h = fh * static_cast<f32>(s);
    }

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = (ww - vp_w) * 0.5f;
    vp.TopLeftY = (wh - vp_h) * 0.5f;
    vp.Width = vp_w;
    vp.Height = vp_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    return vp;
}

// ─────────────────────────────────────────────────────────────────────────
// per-frame present
// ─────────────────────────────────────────────────────────────────────────

bool Win32Present::present(const render::Framebuffer& fb, ScaleMode scale_mode, AspectMode aspect_mode) {
    if (!swap_chain_ || !rtv_)
        return false;

    // If the framebuffer resolution changed (e.g. resolution-menu pick),
    // re-allocate the upload texture. Resolution changes are rare —
    // ResizeBuffers is heavy, but we only do this when the renderer asks.
    if (fb.width != render_w_ || fb.height != render_h_) {
        fb_srv_.Reset();
        fb_tex_.Reset();
        render_w_ = fb.width;
        render_h_ = fb.height;
        if (!create_framebuffer_texture(render_w_, render_h_))
            return false;
    }
    upload_framebuffer(fb);

    // Clear back buffer (matters for Letterbox bars and Integer scale gutters).
    const FLOAT clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context_->ClearRenderTargetView(rtv_.Get(), clear);

    ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
    context_->OMSetRenderTargets(1, rtvs, nullptr);

    const D3D11_VIEWPORT vp = compute_viewport(scale_mode, aspect_mode);
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(rast_.Get());

    UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer* vbs[] = {vbo_.Get()};
    context_->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    context_->IASetInputLayout(input_layout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[] = {fb_srv_.Get()};
    context_->PSSetShaderResources(0, 1, srvs);
    // Pick sampler. Nearest + Integer both use point sampling; Linear uses bilinear.
    ID3D11SamplerState* samp =
        (scale_mode == ScaleMode::Linear) ? sampler_linear_.Get() : sampler_point_.Get();
    ID3D11SamplerState* samps[] = {samp};
    context_->PSSetSamplers(0, 1, samps);

    context_->Draw(6, 0);

    const UINT sync_interval = vsync_ ? 1u : 0u;
    const HRESULT hr = swap_chain_->Present(sync_interval, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // Device lost — log; the recovery story is a Wave-B item. For now we
        // just bail out of present until the engine notices and reinitializes.
        PSY_LOG_ERROR("[win32] DXGI device lost (hr=0x{:08x})", static_cast<u32>(hr));
        return false;
    }
    return SUCCEEDED(hr);
}

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
