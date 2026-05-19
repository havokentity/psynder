// SPDX-License-Identifier: MIT
// Psynder — lane 23 / macOS Apple Silicon. AppKit window + CAMetalLayer scanout
// + CoreAudio AUHAL audio + GameController gamepad. Metal is used exclusively
// to draw a single passthrough quad that presents the CPU framebuffer
// (DESIGN.md §11.3, §7.9, §1: zero GPU engine code, the GPU is a blitter).
//
// One Objective-C++ translation unit keeps the AppKit / Metal / CoreAudio /
// GameController surfaces together; the filesystem helpers live in a plain
// C++ TU so the unit test stays free of Cocoa.

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include "MacPlatform_internal.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace psynder::platform {
namespace {

// ─── Key mapping ─────────────────────────────────────────────────────────
// macOS virtual key codes from <Carbon/HIToolbox/Events.h>. We avoid the
// Carbon import (deprecated) and hard-code the values we actually use.
constexpr u16 kVK_ANSI_A = 0x00; constexpr u16 kVK_ANSI_S = 0x01;
constexpr u16 kVK_ANSI_D = 0x02; constexpr u16 kVK_ANSI_F = 0x03;
constexpr u16 kVK_ANSI_H = 0x04; constexpr u16 kVK_ANSI_G = 0x05;
constexpr u16 kVK_ANSI_Z = 0x06; constexpr u16 kVK_ANSI_X = 0x07;
constexpr u16 kVK_ANSI_C = 0x08; constexpr u16 kVK_ANSI_V = 0x09;
constexpr u16 kVK_ANSI_B = 0x0B; constexpr u16 kVK_ANSI_Q = 0x0C;
constexpr u16 kVK_ANSI_W = 0x0D; constexpr u16 kVK_ANSI_E = 0x0E;
constexpr u16 kVK_ANSI_R = 0x0F; constexpr u16 kVK_ANSI_Y = 0x10;
constexpr u16 kVK_ANSI_T = 0x11; constexpr u16 kVK_ANSI_O = 0x1F;
constexpr u16 kVK_ANSI_U = 0x20; constexpr u16 kVK_ANSI_I = 0x22;
constexpr u16 kVK_ANSI_P = 0x23; constexpr u16 kVK_ANSI_L = 0x25;
constexpr u16 kVK_ANSI_J = 0x26; constexpr u16 kVK_ANSI_K = 0x28;
constexpr u16 kVK_ANSI_N = 0x2D; constexpr u16 kVK_ANSI_M = 0x2E;
constexpr u16 kVK_Return    = 0x24; constexpr u16 kVK_Tab    = 0x30;
constexpr u16 kVK_Space     = 0x31; constexpr u16 kVK_Delete = 0x33;
constexpr u16 kVK_Escape    = 0x35; constexpr u16 kVK_LShift = 0x38;
constexpr u16 kVK_LControl  = 0x3B; constexpr u16 kVK_LOption= 0x3A;
constexpr u16 kVK_RShift    = 0x3C; constexpr u16 kVK_RControl=0x3E;
constexpr u16 kVK_ROption   = 0x3D;
constexpr u16 kVK_LeftArrow = 0x7B; constexpr u16 kVK_RightArrow = 0x7C;
constexpr u16 kVK_DownArrow = 0x7D; constexpr u16 kVK_UpArrow    = 0x7E;
constexpr u16 kVK_F1=0x7A; constexpr u16 kVK_F2=0x78; constexpr u16 kVK_F3=0x63;
constexpr u16 kVK_F4=0x76; constexpr u16 kVK_F5=0x60; constexpr u16 kVK_F6=0x61;
constexpr u16 kVK_F7=0x62; constexpr u16 kVK_F8=0x64; constexpr u16 kVK_F9=0x65;
constexpr u16 kVK_F10=0x6D; constexpr u16 kVK_F11=0x67; constexpr u16 kVK_F12=0x6F;
constexpr u16 kVK_ANSI_Grave = 0x32;   // backtick / tilde

KeyCode translate_key(unsigned short vk) {
    switch (vk) {
        case kVK_Escape:      return KeyCode::Escape;
        case kVK_Return:      return KeyCode::Enter;
        case kVK_Space:       return KeyCode::Space;
        case kVK_Tab:         return KeyCode::Tab;
        case kVK_Delete:      return KeyCode::Backspace;
        case kVK_LeftArrow:   return KeyCode::Left;
        case kVK_RightArrow:  return KeyCode::Right;
        case kVK_UpArrow:     return KeyCode::Up;
        case kVK_DownArrow:   return KeyCode::Down;
        case kVK_ANSI_A:      return KeyCode::A;
        case kVK_ANSI_B:      return KeyCode::B;
        case kVK_ANSI_C:      return KeyCode::C;
        case kVK_ANSI_D:      return KeyCode::D;
        case kVK_ANSI_E:      return KeyCode::E;
        case kVK_ANSI_F:      return KeyCode::F;
        case kVK_ANSI_G:      return KeyCode::G;
        case kVK_ANSI_H:      return KeyCode::H;
        case kVK_ANSI_I:      return KeyCode::I;
        case kVK_ANSI_J:      return KeyCode::J;
        case kVK_ANSI_K:      return KeyCode::K;
        case kVK_ANSI_L:      return KeyCode::L;
        case kVK_ANSI_M:      return KeyCode::M;
        case kVK_ANSI_N:      return KeyCode::N;
        case kVK_ANSI_O:      return KeyCode::O;
        case kVK_ANSI_P:      return KeyCode::P;
        case kVK_ANSI_Q:      return KeyCode::Q;
        case kVK_ANSI_R:      return KeyCode::R;
        case kVK_ANSI_S:      return KeyCode::S;
        case kVK_ANSI_T:      return KeyCode::T;
        case kVK_ANSI_U:      return KeyCode::U;
        case kVK_ANSI_V:      return KeyCode::V;
        case kVK_ANSI_W:      return KeyCode::W;
        case kVK_ANSI_X:      return KeyCode::X;
        case kVK_ANSI_Y:      return KeyCode::Y;
        case kVK_ANSI_Z:      return KeyCode::Z;
        case kVK_F1:          return KeyCode::F1;
        case kVK_F2:          return KeyCode::F2;
        case kVK_F3:          return KeyCode::F3;
        case kVK_F4:          return KeyCode::F4;
        case kVK_F5:          return KeyCode::F5;
        case kVK_F6:          return KeyCode::F6;
        case kVK_F7:          return KeyCode::F7;
        case kVK_F8:          return KeyCode::F8;
        case kVK_F9:          return KeyCode::F9;
        case kVK_F10:         return KeyCode::F10;
        case kVK_F11:         return KeyCode::F11;
        case kVK_F12:         return KeyCode::F12;
        case kVK_ANSI_Grave:  return KeyCode::Tilde;
        case kVK_LShift:      return KeyCode::LeftShift;
        case kVK_RShift:      return KeyCode::RightShift;
        case kVK_LControl:    return KeyCode::LeftCtrl;
        case kVK_RControl:    return KeyCode::RightCtrl;
        case kVK_LOption:     return KeyCode::LeftAlt;
        case kVK_ROption:     return KeyCode::RightAlt;
        default:              return KeyCode::Unknown;
    }
}

// ─── Per-process Input state (single window per process is the design) ───
class MacInput final : public Input {
public:
    bool key_down(KeyCode k) const override {
        auto i = static_cast<usize>(k);
        return i < down_.size() && down_[i].load(std::memory_order_relaxed);
    }
    bool key_pressed(KeyCode k) const override {
        auto i = static_cast<usize>(k);
        if (i >= pressed_.size()) return false;
        // exchange() is non-const on std::atomic but the read-and-clear is
        // the documented semantic of key_pressed (edge-triggered query),
        // so we cast through the const this pointer.
        auto& slot = const_cast<std::atomic<bool>&>(pressed_[i]);
        return slot.exchange(false, std::memory_order_relaxed);
    }
    const MouseState& mouse() const override { return mouse_state_; }

    // ─── Internal event injection (used by NSView delegates) ─────────────
    void on_key(KeyCode k, bool is_down) {
        auto i = static_cast<usize>(k);
        if (i >= down_.size()) return;
        bool was = down_[i].load(std::memory_order_relaxed);
        down_[i].store(is_down, std::memory_order_relaxed);
        if (is_down && !was) {
            pressed_[i].store(true, std::memory_order_relaxed);
        }
    }
    void on_mouse_move(f32 x, f32 y) {
        mouse_state_.dx += x - mouse_state_.x;
        mouse_state_.dy += y - mouse_state_.y;
        mouse_state_.x = x;
        mouse_state_.y = y;
    }
    void on_mouse_button(u8 button, bool down) {
        switch (button) {
            case 0: mouse_state_.left   = down; break;
            case 1: mouse_state_.right  = down; break;
            case 2: mouse_state_.middle = down; break;
            default: break;
        }
    }
    void on_mouse_wheel(f32 dy) { mouse_state_.wheel += dy; }
    void begin_frame()          { mouse_state_.dx = 0; mouse_state_.dy = 0; mouse_state_.wheel = 0; }

private:
    static constexpr usize kKeyCount = static_cast<usize>(KeyCode::Count);
    std::array<std::atomic<bool>, kKeyCount> down_{};
    std::array<std::atomic<bool>, kKeyCount> pressed_{};
    MouseState mouse_state_{};
};

MacInput& mac_input() {
    static MacInput i;
    return i;
}

// ─── NSApp lazy bootstrap ────────────────────────────────────────────────
// AppKit must be initialised on the main thread before any NSWindow is
// constructed. Idempotent so re-entering create_window from samples that
// recreate windows does not re-install policies.
void ensure_app_initialised() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        // Note: we drive the event loop manually via nextEventMatchingMask
        // each frame, so we never call [NSApp run].
    });
}

// ─── Metal scanout state ─────────────────────────────────────────────────
struct ScanoutState {
    id<MTLDevice>             device              = nil;
    id<MTLCommandQueue>       queue               = nil;
    id<MTLRenderPipelineState> pipeline           = nil;
    id<MTLTexture>            texture             = nil;     // CPU framebuffer mirror
    id<MTLSamplerState>       sampler_nearest     = nil;
    id<MTLSamplerState>       sampler_linear      = nil;
    MTLPixelFormat            tex_format          = MTLPixelFormatRGBA8Unorm;
    u32                       tex_width           = 0;
    u32                       tex_height          = 0;

    void shutdown() {
        pipeline         = nil;
        texture          = nil;
        sampler_nearest  = nil;
        sampler_linear   = nil;
        queue            = nil;
        device           = nil;
    }
};

// Passthrough MSL — drawn as a triangle strip of 4 vertices. The vertex
// shader pulls the four NDC corners + the matching UVs from constants so
// we don't need a vertex buffer; the fragment shader samples the texture
// using a sampler whose filter mode is bound by the host based on
// ScaleMode (Nearest / Linear).
NSString* const kScanoutMSL = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct V2F {
    float4 pos [[position]];
    float2 uv;
};

// (uv_min, uv_max) and (ndc_min, ndc_max) baked into push constants so
// letterbox / integer-scale viewports are a constant-buffer update.
struct ScanoutParams {
    float4 ndc_rect;   // (min.x, min.y, max.x, max.y)
    float4 uv_rect;    // (min.x, min.y, max.x, max.y)
};

vertex V2F scanout_vs(uint vid [[vertex_id]],
                      constant ScanoutParams& p [[buffer(0)]]) {
    float2 corners[4] = {
        float2(p.ndc_rect.x, p.ndc_rect.y),
        float2(p.ndc_rect.z, p.ndc_rect.y),
        float2(p.ndc_rect.x, p.ndc_rect.w),
        float2(p.ndc_rect.z, p.ndc_rect.w),
    };
    float2 uvs[4] = {
        float2(p.uv_rect.x, p.uv_rect.w),
        float2(p.uv_rect.z, p.uv_rect.w),
        float2(p.uv_rect.x, p.uv_rect.y),
        float2(p.uv_rect.z, p.uv_rect.y),
    };
    V2F out;
    out.pos = float4(corners[vid], 0.0, 1.0);
    out.uv  = uvs[vid];
    return out;
}

fragment float4 scanout_fs(V2F in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           sampler samp         [[sampler(0)]]) {
    return tex.sample(samp, in.uv);
}
)MSL";

struct ScanoutParams {
    float ndc[4];
    float uv[4];
};

// ─── Framebuffer → Metal pixel format ────────────────────────────────────
MTLPixelFormat mtl_format_for(render::PixelFormat fmt) {
    switch (fmt) {
        case render::PixelFormat::RGBA8: return MTLPixelFormatRGBA8Unorm;
        case render::PixelFormat::BGRA8: return MTLPixelFormatBGRA8Unorm;
        // 16-bit retro / palette formats: cannot be sampled directly. Caller
        // is expected to expand them into an RGBA8 staging buffer (M2+);
        // for Wave A we fall back to RGBA8 and let the upload zero-fill.
        case render::PixelFormat::RGB565:
        case render::PixelFormat::Paletted8:
            return MTLPixelFormatRGBA8Unorm;
    }
    return MTLPixelFormatRGBA8Unorm;
}

}  // anonymous namespace
}  // namespace psynder::platform

// ─── NSWindow delegate: bridges the close button ─────────────────────────
@interface PsynderWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation PsynderWindowDelegate {
@public
    std::atomic<bool>* _should_close;
}
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (_should_close) _should_close->store(true, std::memory_order_relaxed);
    return NO;   // we close the window ourselves on shutdown
}
@end

// ─── NSView subclass — owns the CAMetalLayer ─────────────────────────────
// AppKit's layer-hosting rules: `wantsLayer = YES` + `-makeBackingLayer`
// returning our `CAMetalLayer` makes `self.layer` the metal layer. We do
// NOT override `+layerClass` because that path only fires for layer-backed
// views created with `setWantsLayer:YES` *after* AppKit has decided to use
// the default makeBackingLayer; supplying both was redundant and risked
// the default CALayer winning the race when the view re-encodes itself.
@interface PsynderMetalView : NSView
- (CAMetalLayer*)metalLayer;
@end

@implementation PsynderMetalView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.opaque                 = YES;
    layer.framebufferOnly        = YES;
    layer.presentsWithTransaction= NO;
    // contentsScale is set by the host after the window is created (needs
    // the NSWindow's backingScaleFactor).
    return layer;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView      { return YES; }
- (BOOL)isOpaque              { return YES; }
- (CAMetalLayer*)metalLayer {
    // self.layer is the CAMetalLayer returned from -makeBackingLayer; we
    // bridge through static_cast to satisfy -Wold-style-cast under -Wpedantic.
    return static_cast<CAMetalLayer*>(self.layer);
}

// ── Keyboard ─────────────────────────────────────────────────────────────
- (void)keyDown:(NSEvent*)event {
    psynder::platform::mac_input().on_key(
        psynder::platform::translate_key([event keyCode]), true);
}
- (void)keyUp:(NSEvent*)event {
    psynder::platform::mac_input().on_key(
        psynder::platform::translate_key([event keyCode]), false);
}
- (void)flagsChanged:(NSEvent*)event {
    NSEventModifierFlags f = [event modifierFlags];
    using psynder::platform::KeyCode;
    auto set = [&](KeyCode k, BOOL on) {
        psynder::platform::mac_input().on_key(k, on ? true : false);
    };
    set(KeyCode::LeftShift, (f & NSEventModifierFlagShift)   != 0);
    set(KeyCode::LeftCtrl,  (f & NSEventModifierFlagControl) != 0);
    set(KeyCode::LeftAlt,   (f & NSEventModifierFlagOption)  != 0);
}

// ── Mouse ────────────────────────────────────────────────────────────────
- (void)mouseMoved:(NSEvent*)event       { [self forwardMouseMove:event]; }
- (void)mouseDragged:(NSEvent*)event     { [self forwardMouseMove:event]; }
- (void)rightMouseDragged:(NSEvent*)e    { [self forwardMouseMove:e]; }
- (void)otherMouseDragged:(NSEvent*)e    { [self forwardMouseMove:e]; }
- (void)forwardMouseMove:(NSEvent*)event {
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    // AppKit's Y is bottom-up; the engine wants top-down. We don't yet know
    // the framebuffer's height here, so we report view-local pixels and let
    // the engine flip if it cares.
    psynder::platform::mac_input().on_mouse_move(
        static_cast<psynder::f32>(p.x),
        static_cast<psynder::f32>(self.bounds.size.height - p.y));
}
- (void)mouseDown:(NSEvent*)event        { psynder::platform::mac_input().on_mouse_button(0, true);  }
- (void)mouseUp:(NSEvent*)event          { psynder::platform::mac_input().on_mouse_button(0, false); }
- (void)rightMouseDown:(NSEvent*)event   { psynder::platform::mac_input().on_mouse_button(1, true);  }
- (void)rightMouseUp:(NSEvent*)event     { psynder::platform::mac_input().on_mouse_button(1, false); }
- (void)otherMouseDown:(NSEvent*)event   { psynder::platform::mac_input().on_mouse_button(2, true);  }
- (void)otherMouseUp:(NSEvent*)event     { psynder::platform::mac_input().on_mouse_button(2, false); }
- (void)scrollWheel:(NSEvent*)event {
    psynder::platform::mac_input().on_mouse_wheel(static_cast<psynder::f32>([event scrollingDeltaY]));
}
@end

// ─── Window impl ─────────────────────────────────────────────────────────
namespace psynder::platform {
namespace {

class MacWindow final : public Window {
public:
    explicit MacWindow(const WindowDesc& desc) : desc_(desc) {
        @autoreleasepool {
            ensure_app_initialised();

            NSRect frame = NSMakeRect(0, 0, desc.window_width, desc.window_height);
            NSWindowStyleMask style =
                NSWindowStyleMaskTitled |
                NSWindowStyleMaskClosable |
                NSWindowStyleMaskMiniaturizable;
            if (desc.resizable) style |= NSWindowStyleMaskResizable;

            ns_window_ = [[NSWindow alloc]
                initWithContentRect:frame
                          styleMask:style
                            backing:NSBackingStoreBuffered
                              defer:NO];

            NSString* title = [NSString stringWithUTF8String:desc.title.c_str()];
            [ns_window_ setTitle:title];
            [ns_window_ setReleasedWhenClosed:NO];
            [ns_window_ center];

            // Delegate hooks up the close button
            PsynderWindowDelegate* del = [[PsynderWindowDelegate alloc] init];
            del->_should_close = &should_close_;
            window_delegate_ = del;
            [ns_window_ setDelegate:del];

            metal_view_ = [[PsynderMetalView alloc] initWithFrame:frame];
            [metal_view_ setWantsLayer:YES];
            [ns_window_ setContentView:metal_view_];
            [ns_window_ makeFirstResponder:metal_view_];
            [ns_window_ setAcceptsMouseMovedEvents:YES];

            // Initialise Metal scanout
            scanout_.device = MTLCreateSystemDefaultDevice();
            if (!scanout_.device) {
                PSY_LOG_ERROR("MacWindow: no Metal device — running headless (no present)");
            } else {
                scanout_.queue = [scanout_.device newCommandQueue];
                auto* layer = [metal_view_ metalLayer];
                if (layer) {
                    layer.device          = scanout_.device;
                    layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
                    layer.framebufferOnly = YES;
                    layer.contentsScale   = [ns_window_ backingScaleFactor];
                    backing_scale_ = layer.contentsScale;
                    // Track the layer for present
                    metal_layer_ = layer;
                }
                build_pipeline_();
                build_samplers_();
            }

            // GameController.framework: arm connect/disconnect listeners.
            // Audio is NOT auto-started — lane 12 owns the policy and will
            // call audio_start() once its mixer is initialised. Starting
            // CoreAudio here would hold the device for every sample.
            macos::gamepad_arm();

            // Show window and bring app forward
            [ns_window_ makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }
    }

    ~MacWindow() override {
        @autoreleasepool {
            scanout_.shutdown();
            if (ns_window_) {
                [ns_window_ setDelegate:nil];
                [ns_window_ orderOut:nil];
                [ns_window_ close];
                ns_window_ = nil;
            }
            metal_view_      = nil;
            metal_layer_     = nil;
            window_delegate_ = nil;
        }
    }

    // ─── Window contract ─────────────────────────────────────────────────
    void poll_events() override {
        @autoreleasepool {
            mac_input().begin_frame();
            // Drain every pending event without blocking
            NSEvent* event;
            do {
                event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate distantPast]
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES];
                if (event) [NSApp sendEvent:event];
            } while (event);
            [NSApp updateWindows];

            // ESC closes the window per the M0 demo contract
            if (mac_input().key_down(KeyCode::Escape)) {
                should_close_.store(true, std::memory_order_relaxed);
            }
        }
    }

    bool should_close() const override {
        return should_close_.load(std::memory_order_relaxed);
    }

    void present(const render::Framebuffer& fb) override {
        if (!fb.pixels || fb.width == 0 || fb.height == 0) return;
        if (!scanout_.device) return;
        // Lazy layer fixup: AppKit may not have produced the backing layer
        // by the time the constructor reached us; pick it up now if so.
        if (!metal_layer_) {
            metal_layer_ = [metal_view_ metalLayer];
            if (metal_layer_) {
                metal_layer_.device          = scanout_.device;
                metal_layer_.pixelFormat     = MTLPixelFormatBGRA8Unorm;
                metal_layer_.framebufferOnly = YES;
                metal_layer_.contentsScale   = [ns_window_ backingScaleFactor];
            }
        }
        if (!metal_layer_) return;
        @autoreleasepool {
            // ── Drawable resize bookkeeping ──────────────────────────────
            CGFloat bs = [ns_window_ backingScaleFactor];
            backing_scale_ = bs;
            NSSize content = [metal_view_ bounds].size;
            CGFloat drawable_w = std::max<CGFloat>(content.width  * bs, 1.0);
            CGFloat drawable_h = std::max<CGFloat>(content.height * bs, 1.0);
            metal_layer_.drawableSize = CGSizeMake(drawable_w, drawable_h);
            // ── Texture realloc if framebuffer geometry changed ──────────
            MTLPixelFormat fmt = mtl_format_for(fb.format);
            if (scanout_.texture == nil ||
                scanout_.tex_width  != fb.width ||
                scanout_.tex_height != fb.height ||
                scanout_.tex_format != fmt) {
                MTLTextureDescriptor* td =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                      width:fb.width
                                                                     height:fb.height
                                                                   mipmapped:NO];
                td.usage         = MTLTextureUsageShaderRead;
                td.storageMode   = MTLStorageModeShared;
                scanout_.texture = [scanout_.device newTextureWithDescriptor:td];
                scanout_.tex_width  = fb.width;
                scanout_.tex_height = fb.height;
                scanout_.tex_format = fmt;
            }
            // ── CPU framebuffer → texture upload ─────────────────────────
            if (fb.format == render::PixelFormat::RGBA8 ||
                fb.format == render::PixelFormat::BGRA8) {
                MTLRegion region = MTLRegionMake2D(0, 0, fb.width, fb.height);
                [scanout_.texture replaceRegion:region
                                    mipmapLevel:0
                                      withBytes:fb.pixels
                                    bytesPerRow:fb.pitch];
            } else {
                // 16-bit / palette: write zeros for now — lane 09 owns expand
                std::vector<u8> staging(static_cast<usize>(fb.width) * fb.height * 4, 0);
                MTLRegion region = MTLRegionMake2D(0, 0, fb.width, fb.height);
                [scanout_.texture replaceRegion:region
                                    mipmapLevel:0
                                      withBytes:staging.data()
                                    bytesPerRow:fb.width * 4];
            }
            // ── Encode the present pass ─────────────────────────────────
            id<CAMetalDrawable> drawable = [metal_layer_ nextDrawable];
            if (!drawable) return;

            MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture     = drawable.texture;
            rpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            rpd.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1);

            id<MTLCommandBuffer> cmd = [scanout_.queue commandBuffer];
            id<MTLRenderCommandEncoder> enc =
                [cmd renderCommandEncoderWithDescriptor:rpd];

            // Compute letterbox / integer / stretch viewport in NDC.
            ScanoutParams params = compute_viewport_(fb.width, fb.height,
                                                     drawable_w, drawable_h);
            [enc setRenderPipelineState:scanout_.pipeline];
            [enc setVertexBytes:&params length:sizeof(params) atIndex:0];
            [enc setFragmentTexture:scanout_.texture atIndex:0];
            id<MTLSamplerState> sampler =
                (desc_.scale_mode == ScaleMode::Linear)
                    ? scanout_.sampler_linear
                    : scanout_.sampler_nearest;
            [enc setFragmentSamplerState:sampler atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0
                    vertexCount:4];
            [enc endEncoding];

            [cmd presentDrawable:drawable];
            [cmd commit];
        }
    }

    void set_title(std::string_view t) override {
        desc_.title = std::string{t};
        @autoreleasepool {
            NSString* s = [NSString stringWithUTF8String:desc_.title.c_str()];
            [ns_window_ setTitle:s];
        }
    }

    u32 window_width()  const override {
        if (!ns_window_) return desc_.window_width;
        NSSize sz = [[ns_window_ contentView] bounds].size;
        return static_cast<u32>(sz.width);
    }
    u32 window_height() const override {
        if (!ns_window_) return desc_.window_height;
        NSSize sz = [[ns_window_ contentView] bounds].size;
        return static_cast<u32>(sz.height);
    }

private:
    // ─── Metal pipeline / sampler setup ──────────────────────────────────
    void build_pipeline_() {
        NSError* err = nil;
        id<MTLLibrary> lib =
            [scanout_.device newLibraryWithSource:kScanoutMSL
                                          options:nil
                                            error:&err];
        if (!lib) {
            PSY_LOG_ERROR("MacWindow: MSL compile failed: {}",
                          [[err localizedDescription] UTF8String]);
            return;
        }
        id<MTLFunction> vs = [lib newFunctionWithName:@"scanout_vs"];
        id<MTLFunction> fs = [lib newFunctionWithName:@"scanout_fs"];
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction   = vs;
        pd.fragmentFunction = fs;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        scanout_.pipeline =
            [scanout_.device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!scanout_.pipeline) {
            PSY_LOG_ERROR("MacWindow: pipeline build failed: {}",
                          [[err localizedDescription] UTF8String]);
        }
    }
    void build_samplers_() {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.magFilter    = MTLSamplerMinMagFilterNearest;
        sd.minFilter    = MTLSamplerMinMagFilterNearest;
        scanout_.sampler_nearest = [scanout_.device newSamplerStateWithDescriptor:sd];
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        scanout_.sampler_linear  = [scanout_.device newSamplerStateWithDescriptor:sd];
    }

    // ─── Viewport math (DESIGN.md §7.9: integer / linear / nearest) ──────
    ScanoutParams compute_viewport_(u32 fb_w, u32 fb_h,
                                    f64 win_w, f64 win_h) const {
        ScanoutParams p{};
        p.uv[0] = 0.0f; p.uv[1] = 0.0f;
        p.uv[2] = 1.0f; p.uv[3] = 1.0f;

        // Pick the drawn size in pixels first, then convert to NDC.
        f64 drawn_w = win_w;
        f64 drawn_h = win_h;
        if (desc_.aspect_mode == AspectMode::Stretch) {
            // Full window; no aspect preservation
        } else if (desc_.scale_mode == ScaleMode::Integer) {
            // Largest integer N such that N*fb fits in the window
            u32 nx = static_cast<u32>(std::max<f64>(1.0, std::floor(win_w / fb_w)));
            u32 ny = static_cast<u32>(std::max<f64>(1.0, std::floor(win_h / fb_h)));
            u32 n  = std::max<u32>(1, std::min(nx, ny));
            drawn_w = static_cast<f64>(fb_w) * n;
            drawn_h = static_cast<f64>(fb_h) * n;
        } else if (desc_.aspect_mode == AspectMode::Crop) {
            // Fill window, clip the smaller side. We expand `drawn` past
            // the window and adjust UVs.
            f64 a_win = win_w / win_h;
            f64 a_fb  = static_cast<f64>(fb_w) / fb_h;
            if (a_win > a_fb) {
                drawn_w = win_w;
                drawn_h = win_w / a_fb;
                f64 over = (drawn_h - win_h) / drawn_h;
                p.uv[1] = static_cast<float>(0.5 * over);
                p.uv[3] = static_cast<float>(1.0 - 0.5 * over);
                drawn_h = win_h;
            } else {
                drawn_h = win_h;
                drawn_w = win_h * a_fb;
                f64 over = (drawn_w - win_w) / drawn_w;
                p.uv[0] = static_cast<float>(0.5 * over);
                p.uv[2] = static_cast<float>(1.0 - 0.5 * over);
                drawn_w = win_w;
            }
        } else {
            // Letterbox (default): fit the larger axis exactly
            f64 a_win = win_w / win_h;
            f64 a_fb  = static_cast<f64>(fb_w) / fb_h;
            if (a_win > a_fb) {
                drawn_h = win_h;
                drawn_w = drawn_h * a_fb;
            } else {
                drawn_w = win_w;
                drawn_h = drawn_w / a_fb;
            }
        }
        // Convert drawn rect (centred in window) to NDC [-1, 1]
        f64 nx_half = drawn_w / win_w;   // 0..1
        f64 ny_half = drawn_h / win_h;   // 0..1
        p.ndc[0] = static_cast<float>(-nx_half);
        p.ndc[1] = static_cast<float>(-ny_half);
        p.ndc[2] = static_cast<float>( nx_half);
        p.ndc[3] = static_cast<float>( ny_half);
        return p;
    }

    WindowDesc                    desc_;
    NSWindow*                     ns_window_       = nil;
    PsynderMetalView*             metal_view_      = nil;
    CAMetalLayer*                 metal_layer_     = nil;
    id /* PsynderWindowDelegate* */ window_delegate_ = nil;
    ScanoutState                  scanout_{};
    CGFloat                       backing_scale_   = 1.0;
    std::atomic<bool>             should_close_{false};
};

}  // anonymous namespace
}  // namespace psynder::platform

// ─── Public factory + input glue ─────────────────────────────────────────
namespace psynder::platform {

Window* create_window_impl(const WindowDesc& desc) { return new MacWindow(desc); }
void    destroy_window_impl(Window* w)             { delete w; }

Input* input() { return &mac_input(); }

}  // namespace psynder::platform

// ─── CoreAudio AUHAL backend ─────────────────────────────────────────────
// Self-contained device opener that lane 12 (audio) will drive once its
// mixer lands. Provided here so the platform layer is a complete Wave-A
// deliverable; lane 12 will call `psynder::platform::macos::audio_start`
// from its CoreAudio backend translation unit.
namespace psynder::platform::macos {

namespace {
struct CoreAudioState {
    AudioUnit              unit       = nullptr;
    AudioRenderCallback    callback   = nullptr;
    void*                  user       = nullptr;
    std::atomic<bool>      running{false};
    u32                    sample_rate = 48000;
    u32                    channels    = 2;
};

CoreAudioState& ca_state() {
    static CoreAudioState s;
    return s;
}

OSStatus core_audio_render(void* in_ref_con,
                           AudioUnitRenderActionFlags* /*io_action_flags*/,
                           const AudioTimeStamp* /*in_time_stamp*/,
                           UInt32 /*in_bus_number*/,
                           UInt32 in_number_frames,
                           AudioBufferList* io_data) {
    auto* st = static_cast<CoreAudioState*>(in_ref_con);
    if (!st || !st->running.load(std::memory_order_acquire) || !io_data) {
        // Silence on fast exit
        if (io_data) {
            for (UInt32 b = 0; b < io_data->mNumberBuffers; ++b) {
                std::memset(io_data->mBuffers[b].mData, 0,
                            io_data->mBuffers[b].mDataByteSize);
            }
        }
        return noErr;
    }
    // We requested kAudioFormatFlagIsPacked + non-interleaved float32 below,
    // so each channel arrives in its own buffer of length in_number_frames.
    const UInt32 ch = io_data->mNumberBuffers;
    if (ch == 0) return noErr;

    if (st->callback) {
        // Interleave float scratch the callback fills, then de-interleave
        // into AudioBufferList. Buffer size is small (typ. 512), stack-OK.
        constexpr UInt32 kMaxFrames = 4096;
        UInt32 frames = std::min<UInt32>(in_number_frames, kMaxFrames);
        f32 scratch[kMaxFrames * 8];
        UInt32 use_ch = std::min<UInt32>(ch, 8);
        st->callback(st->user, scratch, frames, use_ch, st->sample_rate);
        for (UInt32 c = 0; c < ch; ++c) {
            f32* dst = static_cast<f32*>(io_data->mBuffers[c].mData);
            const UInt32 dst_frames =
                io_data->mBuffers[c].mDataByteSize / sizeof(f32);
            UInt32 n = std::min(dst_frames, frames);
            for (UInt32 f = 0; f < n; ++f) dst[f] = scratch[f * use_ch + (c % use_ch)];
            for (UInt32 f = n; f < dst_frames; ++f) dst[f] = 0.0f;
        }
    } else {
        // No callback registered → emit silence (still drives the device)
        for (UInt32 c = 0; c < ch; ++c) {
            std::memset(io_data->mBuffers[c].mData, 0,
                        io_data->mBuffers[c].mDataByteSize);
        }
    }
    return noErr;
}
}  // namespace

bool audio_start(const AudioDeviceDesc& desc, AudioRenderCallback cb, void* user) {
    auto& st = ca_state();
    if (st.running.load(std::memory_order_acquire)) return true;
    st.callback    = cb;
    st.user        = user;
    st.sample_rate = desc.sample_rate;
    st.channels    = desc.channels;

    // Locate DefaultOutput AudioComponent
    AudioComponentDescription cd{};
    cd.componentType         = kAudioUnitType_Output;
    cd.componentSubType      = kAudioUnitSubType_DefaultOutput;
    cd.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &cd);
    if (!comp) {
        PSY_LOG_WARN("CoreAudio: no DefaultOutput AudioComponent");
        return false;
    }
    OSStatus rc = AudioComponentInstanceNew(comp, &st.unit);
    if (rc != noErr) {
        PSY_LOG_WARN("CoreAudio: AudioComponentInstanceNew failed ({})", static_cast<int>(rc));
        return false;
    }

    // Stream format: 32-bit float, non-interleaved
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = desc.sample_rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                          | kAudioFormatFlagIsNonInterleaved;
    fmt.mFramesPerPacket  = 1;
    fmt.mChannelsPerFrame = desc.channels;
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerPacket   = 4;
    fmt.mBytesPerFrame    = 4;

    rc = AudioUnitSetProperty(st.unit, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Input, 0,
                              &fmt, sizeof(fmt));
    if (rc != noErr) {
        PSY_LOG_WARN("CoreAudio: SetProperty(StreamFormat) failed ({})", static_cast<int>(rc));
        AudioComponentInstanceDispose(st.unit);
        st.unit = nullptr;
        return false;
    }

    AURenderCallbackStruct cbs{};
    cbs.inputProc       = core_audio_render;
    cbs.inputProcRefCon = &st;
    rc = AudioUnitSetProperty(st.unit, kAudioUnitProperty_SetRenderCallback,
                              kAudioUnitScope_Input, 0,
                              &cbs, sizeof(cbs));
    if (rc != noErr) {
        PSY_LOG_WARN("CoreAudio: SetRenderCallback failed ({})", static_cast<int>(rc));
        AudioComponentInstanceDispose(st.unit);
        st.unit = nullptr;
        return false;
    }

    rc = AudioUnitInitialize(st.unit);
    if (rc != noErr) {
        PSY_LOG_WARN("CoreAudio: AudioUnitInitialize failed ({})", static_cast<int>(rc));
        AudioComponentInstanceDispose(st.unit);
        st.unit = nullptr;
        return false;
    }

    st.running.store(true, std::memory_order_release);
    rc = AudioOutputUnitStart(st.unit);
    if (rc != noErr) {
        PSY_LOG_WARN("CoreAudio: AudioOutputUnitStart failed ({})", static_cast<int>(rc));
        st.running.store(false, std::memory_order_release);
        AudioUnitUninitialize(st.unit);
        AudioComponentInstanceDispose(st.unit);
        st.unit = nullptr;
        return false;
    }
    return true;
}

void audio_stop() {
    auto& st = ca_state();
    if (!st.unit) return;
    st.running.store(false, std::memory_order_release);
    AudioOutputUnitStop(st.unit);
    AudioUnitUninitialize(st.unit);
    AudioComponentInstanceDispose(st.unit);
    st.unit = nullptr;
}
bool audio_running()            { return ca_state().running.load(std::memory_order_acquire); }
u32  audio_actual_sample_rate() { return ca_state().sample_rate; }
u32  audio_actual_channels()    { return ca_state().channels; }

}  // namespace psynder::platform::macos

// ─── GameController.framework — gamepad enumeration ──────────────────────
// Wave-A scope: just keep the controller list alive so connect/disconnect
// notifications are received. The full gamepad input API will be threaded
// through `psynder::platform::Input` once lanes 13 / 16 land their event
// requirements. The pump runs on the main thread inside poll_events so it
// is cheap to call every frame.
namespace psynder::platform::macos {

namespace {
struct GamepadState {
    std::atomic<u32>    connected_count{0};
    std::atomic<bool>   notifications_armed{false};
};
GamepadState& gp_state() {
    static GamepadState s;
    return s;
}
}  // namespace

void gamepad_arm() {
    if (gp_state().notifications_armed.exchange(true)) return;
    @autoreleasepool {
        // GCController posts NSNotifications on connect / disconnect. We
        // just hold the count; the runloop dispatch happens on the main
        // thread (where poll_events is also driven).
        [[NSNotificationCenter defaultCenter]
            addObserverForName:GCControllerDidConnectNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification* /*note*/) {
            gp_state().connected_count.fetch_add(1, std::memory_order_relaxed);
        }];
        [[NSNotificationCenter defaultCenter]
            addObserverForName:GCControllerDidDisconnectNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification* /*note*/) {
            // Saturating decrement
            u32 cur = gp_state().connected_count.load(std::memory_order_relaxed);
            while (cur > 0 && !gp_state().connected_count.compare_exchange_weak(
                                  cur, cur - 1, std::memory_order_relaxed)) {}
        }];
        // Seed with anyone already plugged in
        gp_state().connected_count.store(
            static_cast<u32>([[GCController controllers] count]),
            std::memory_order_relaxed);
    }
}

u32 gamepad_count() { return gp_state().connected_count.load(std::memory_order_relaxed); }

}  // namespace psynder::platform::macos
