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
#import <IOSurface/IOSurface.h>
#import <IOKit/hid/IOHIDManager.h>
#import <ForceFeedback/ForceFeedback.h>
#import <ForceFeedback/ForceFeedbackConstants.h>

#include "MacPlatform_internal.h"
#include "audio/internal/Backend.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <span>
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
    std::span<const u32> text_input() const override { return text_; }

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
    void on_text(u32 codepoint) { text_.push_back(codepoint); }
    void begin_frame() {
        mouse_state_.dx = 0;
        mouse_state_.dy = 0;
        mouse_state_.wheel = 0;
        text_.clear();  // text_input() reports only this frame's codepoints
    }

private:
    static constexpr usize kKeyCount = static_cast<usize>(KeyCode::Count);
    std::array<std::atomic<bool>, kKeyCount> down_{};
    std::array<std::atomic<bool>, kKeyCount> pressed_{};
    MouseState mouse_state_{};
    // Text-entry codepoints captured this frame. Filled by keyDown: (main
    // thread, inside poll_events) and read after the pump returns — same
    // thread, so no atomics needed (unlike the key arrays, which the design
    // keeps atomic for defensive single-window safety).
    std::vector<u32> text_{};
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

    // ── IOSurface-backed zero-copy upload path (DESIGN.md §11.3) ─────────
    // When the framebuffer's geometry / format is stable, we wrap a single
    // `IOSurfaceRef` once and bind a Metal texture to it via
    // `-newTextureWithDescriptor:iosurface:plane:`. The per-frame upload
    // then becomes a CPU-side memcpy into the surface's locked base
    // address, no `replaceRegion:withBytes:` and no second copy inside
    // Metal. On hosts where IOSurface allocation fails the field stays nil
    // and the legacy `replaceRegion` path is used as a fallback.
    IOSurfaceRef              iosurface           = nullptr;

    void shutdown() {
        pipeline         = nil;
        texture          = nil;
        sampler_nearest  = nil;
        sampler_linear   = nil;
        queue            = nil;
        device           = nil;
        if (iosurface) {
            CFRelease(iosurface);
            iosurface = nullptr;
        }
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

    // Text entry for the software console overlay. -characters is already
    // mapped through the active keyboard layout + Shift, so we get '@' for
    // Shift+2 on US, accented glyphs on dead-key layouts, etc. Skip Command
    // chords (those are shortcuts, not text) and C0/DEL control codes — the
    // console reads Enter/Backspace/arrows via key_pressed instead.
    if (([event modifierFlags] & NSEventModifierFlagCommand) != 0) return;
    NSString* chars = [event characters];
    const NSUInteger n = [chars length];
    for (NSUInteger i = 0; i < n;) {
        const unichar c = [chars characterAtIndex:i];
        uint32_t cp = c;
        // Recombine a UTF-16 surrogate pair into one scalar value.
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            const unichar lo = [chars characterAtIndex:i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000u + ((static_cast<uint32_t>(c) - 0xD800u) << 10) +
                     (static_cast<uint32_t>(lo) - 0xDC00u);
                i += 2;
            } else {
                i += 1;
            }
        } else {
            i += 1;
        }
        // Drop C0 controls + DEL, and AppKit's function-key encodings
        // (arrows, Delete, Home/End, F-keys, ...) which it reports in the
        // U+F700..U+F8FF private-use block — those are NOT text and would
        // otherwise insert missing-glyph boxes into the console prompt.
        if (cp >= 0x20 && cp != 0x7F && !(cp >= 0xF700 && cp <= 0xF8FF))
            psynder::platform::mac_input().on_text(cp);
    }
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

// NSWindow subclass that can become key / main even when borderless. A vanilla
// borderless NSWindow returns NO for -canBecomeKeyWindow and goes deaf to the
// keyboard — so our borderless full-screen mode would kill console + gameplay
// input. Overriding keeps the key path alive in every style.
@interface PsynderNSWindow : NSWindow
@end
@implementation PsynderNSWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
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

            ns_window_ = [[PsynderNSWindow alloc]
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

            // ESC closes the window per the M0 demo contract — UNLESS something
            // is capturing text (the console is open), where Esc is the
            // console's (clear the prompt / dismiss popup), not a quit.
            if (mac_input().key_down(KeyCode::Escape) && !text_input_capturing()) {
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
                // Tear down any previously-bound IOSurface; a fresh size /
                // format requires a fresh surface (its layout is baked in
                // at allocation time).
                if (scanout_.iosurface) {
                    CFRelease(scanout_.iosurface);
                    scanout_.iosurface = nullptr;
                }
                // Try the IOSurface-backed zero-copy path first for the
                // two GPU-sampleable framebuffer formats. RGB565 / Paletted8
                // still go through the staging path below (lane 09 owns
                // the real expand kernel; here we just zero-fill).
                const bool can_iosurface =
                    (fb.format == render::PixelFormat::RGBA8 ||
                     fb.format == render::PixelFormat::BGRA8);
                if (can_iosurface) {
                    // Per-pixel byte count is fixed at 4 for the formats we
                    // accept on this path. IOSurface needs an explicit
                    // pixel-format FourCC: 'BGRA' for both because Metal's
                    // sampleable IOSurface textures normalise to BGRA on
                    // import. The Metal texture format we use below still
                    // sees the requested `fmt` so colour ordering is
                    // honoured by the sampler swizzle.
                    constexpr u32 kBytesPerPixel = 4;
                    const size_t aligned_bpr = IOSurfaceAlignProperty(
                        kIOSurfaceBytesPerRow,
                        static_cast<size_t>(fb.width) * kBytesPerPixel);
                    NSDictionary* props = @{
                        (id)kIOSurfaceWidth:           @((NSInteger)fb.width),
                        (id)kIOSurfaceHeight:          @((NSInteger)fb.height),
                        (id)kIOSurfaceBytesPerElement: @((NSInteger)kBytesPerPixel),
                        (id)kIOSurfaceBytesPerRow:     @((NSInteger)aligned_bpr),
                        (id)kIOSurfacePixelFormat:     @((unsigned)'BGRA'),
                    };
                    scanout_.iosurface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
                    if (scanout_.iosurface) {
                        MTLTextureDescriptor* td = [MTLTextureDescriptor
                            texture2DDescriptorWithPixelFormat:fmt
                                                         width:fb.width
                                                        height:fb.height
                                                     mipmapped:NO];
                        td.usage       = MTLTextureUsageShaderRead;
                        td.storageMode = MTLStorageModeShared;
                        scanout_.texture =
                            [scanout_.device newTextureWithDescriptor:td
                                                            iosurface:scanout_.iosurface
                                                                plane:0];
                        if (!scanout_.texture) {
                            // IOSurface path refused (driver / format
                            // mismatch); fall back to a regular texture.
                            CFRelease(scanout_.iosurface);
                            scanout_.iosurface = nullptr;
                        }
                    }
                }
                if (!scanout_.texture) {
                    MTLTextureDescriptor* td =
                        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                          width:fb.width
                                                                         height:fb.height
                                                                       mipmapped:NO];
                    td.usage         = MTLTextureUsageShaderRead;
                    td.storageMode   = MTLStorageModeShared;
                    scanout_.texture = [scanout_.device newTextureWithDescriptor:td];
                }
                scanout_.tex_width  = fb.width;
                scanout_.tex_height = fb.height;
                scanout_.tex_format = fmt;
            }
            // ── CPU framebuffer → texture upload ─────────────────────────
            if (fb.format == render::PixelFormat::RGBA8 ||
                fb.format == render::PixelFormat::BGRA8) {
                if (scanout_.iosurface) {
                    // Zero-copy: write pixels directly into the IOSurface
                    // base address. The texture is already bound to the
                    // surface so Metal sees the new data without a
                    // `replaceRegion:withBytes:` call (which always copies
                    // through Metal's staging path).
                    IOSurfaceLock(scanout_.iosurface, 0, nullptr);
                    auto* dst = static_cast<u8*>(IOSurfaceGetBaseAddress(scanout_.iosurface));
                    const size_t dst_pitch = IOSurfaceGetBytesPerRow(scanout_.iosurface);
                    const size_t src_pitch = fb.pitch;
                    const size_t row_bytes = static_cast<size_t>(fb.width) * 4;
                    if (dst_pitch == src_pitch && src_pitch == row_bytes) {
                        std::memcpy(dst, fb.pixels, src_pitch * fb.height);
                    } else {
                        const auto* src = static_cast<const u8*>(fb.pixels);
                        for (u32 y = 0; y < fb.height; ++y) {
                            std::memcpy(dst + y * dst_pitch,
                                        src + y * src_pitch,
                                        row_bytes);
                        }
                    }
                    IOSurfaceUnlock(scanout_.iosurface, 0, nullptr);
                } else {
                    MTLRegion region = MTLRegionMake2D(0, 0, fb.width, fb.height);
                    [scanout_.texture replaceRegion:region
                                        mipmapLevel:0
                                          withBytes:fb.pixels
                                        bytesPerRow:fb.pitch];
                }
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

    // ─── Runtime display control ─────────────────────────────────────────
    // Borderless full-screen: drop the chrome, cover the screen. present()
    // already stretches the CPU framebuffer to the (now full-screen) view, so
    // the frame fills the display with no framebuffer realloc here.
    void set_fullscreen(bool on) override {
        if (!ns_window_ || on == fullscreen_) return;
        @autoreleasepool {
            if (on) {
                saved_frame_ = [ns_window_ frame];
                saved_style_ = [ns_window_ styleMask];
                NSScreen* screen = [ns_window_ screen] ?: [NSScreen mainScreen];
                [ns_window_ setStyleMask:NSWindowStyleMaskBorderless];
                [ns_window_ setFrame:[screen frame] display:YES];
                fullscreen_ = true;
            } else {
                [ns_window_ setStyleMask:saved_style_];
                [ns_window_ setFrame:saved_frame_ display:YES];
                fullscreen_ = false;
            }
            [ns_window_ makeFirstResponder:metal_view_];
            [ns_window_ makeKeyAndOrderFront:nil];
        }
    }
    bool is_fullscreen() const override { return fullscreen_; }

    void set_window_size(u32 w, u32 h) override {
        if (!ns_window_ || w == 0 || h == 0) return;
        @autoreleasepool {
            if (fullscreen_) set_fullscreen(false);  // leave full-screen first
            [ns_window_ setContentSize:NSMakeSize(static_cast<CGFloat>(w), static_cast<CGFloat>(h))];
            [ns_window_ center];
            [ns_window_ makeFirstResponder:metal_view_];
        }
        desc_.window_width  = w;
        desc_.window_height = h;
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

    // Borderless full-screen state: saved windowed frame + style to restore.
    bool                          fullscreen_      = false;
    NSRect                        saved_frame_     = NSZeroRect;
    NSWindowStyleMask             saved_style_     = 0;
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

// ─── CoreAudio strong override for lane-12's audio dispatcher ────────────
// Lane 12 (engine/audio/internal/Backend.cpp) declares `[[gnu::weak]]`
// fallbacks for `backend_init_coreaudio` / `backend_shutdown_coreaudio` so
// the Wave-A null-device path can run on hosts without a platform lane
// linked in. On macOS we ship a strong override here that wires lane 12's
// mixer pull (interleaved stereo f32) directly into the AUHAL render
// callback via our existing `audio_start` machinery. The platform lane's
// public surface stays unchanged — lane 12 still calls the dispatcher and
// is none the wiser about which backend won the link.
namespace psynder::audio {

namespace {

// Adapter — AUHAL's per-channel-buffer callback (see `audio_start` above)
// invokes this thunk with a stereo scratch buffer. We forward to lane 12's
// `MixerCallback`, which fills interleaved L/R floats. The AUHAL render
// then de-interleaves into the AudioBufferList's per-channel buffers.
struct CoreAudioAdapter {
    MixerCallback cb   = nullptr;
    void*         user = nullptr;
};

CoreAudioAdapter& ca_adapter() {
    static CoreAudioAdapter a;
    return a;
}

void coreaudio_render_thunk(void* /*user*/, psynder::f32* out,
                            psynder::u32 frame_count,
                            psynder::u32 channel_count,
                            psynder::u32 /*sample_rate*/) {
    auto& a = ca_adapter();
    const psynder::u32 stereo_floats = 2u * frame_count;
    if (!a.cb) {
        // Silence — keep the device alive without producing audio.
        for (psynder::u32 i = 0; i < frame_count * channel_count; ++i) out[i] = 0.0f;
        return;
    }
    if (channel_count == 2) {
        // Direct: lane 12 writes interleaved stereo straight into `out`.
        a.cb(out, frame_count, a.user);
        return;
    }
    // Channel-count mismatch: pull a stereo block into a small scratch buffer
    // and broadcast / downmix into the device's channel layout. Stack-bounded
    // to the same 4096-frame cap as the AUHAL render path.
    constexpr psynder::u32 kMaxFrames = 4096;
    psynder::f32 scratch[kMaxFrames * 2];
    const psynder::u32 frames = std::min<psynder::u32>(frame_count, kMaxFrames);
    a.cb(scratch, frames, a.user);
    for (psynder::u32 f = 0; f < frames; ++f) {
        const psynder::f32 l = scratch[2*f + 0];
        const psynder::f32 r = scratch[2*f + 1];
        const psynder::f32 m = 0.5f * (l + r);
        if (channel_count == 1) {
            out[f] = m;
        } else {
            // L, R, mono, mono, … — front-pair → engine stereo, rest → centre
            for (psynder::u32 c = 0; c < channel_count; ++c) {
                out[f * channel_count + c] =
                    (c == 0) ? l : (c == 1) ? r : m;
            }
        }
        (void)stereo_floats;
    }
    // Zero any remaining frames past `kMaxFrames` (shouldn't happen in practice).
    for (psynder::u32 f = frames; f < frame_count; ++f) {
        for (psynder::u32 c = 0; c < channel_count; ++c) {
            out[f * channel_count + c] = 0.0f;
        }
    }
}

}  // namespace

bool backend_init_coreaudio(const DeviceDesc& desc, MixerCallback cb, void* user) noexcept {
    auto& a = ca_adapter();
    a.cb   = cb;
    a.user = user;
    psynder::platform::macos::AudioDeviceDesc pd{};
    pd.sample_rate   = desc.sample_rate;
    pd.channels      = desc.channels;
    pd.buffer_frames = desc.buffer_frames;
    if (!psynder::platform::macos::audio_start(pd, &coreaudio_render_thunk, nullptr)) {
        // audio_start failed (no device on this host, etc.). Keep the
        // adapter populated so the engine's `mixer_pull` is still
        // reachable from tests / smoke samples; the failure is logged
        // by `audio_start` itself.
        return false;
    }
    return true;
}

void backend_shutdown_coreaudio() noexcept {
    psynder::platform::macos::audio_stop();
    auto& a = ca_adapter();
    a.cb   = nullptr;
    a.user = nullptr;
}

}  // namespace psynder::audio

// ─── HID Force Feedback (IOKit + ForceFeedback.framework) ────────────────
// Wave-B deliverable: a constant-force effect descriptor + best-effort
// submission against the first FFB-capable HID device discovered. The
// engine consumer (Wave-C lane 25 sample_04) calls `ffb_submit_constant_force`
// every physics step with the magnitude derived from the lateral slip
// angle; this TU translates that descriptor into a `FFEFFECT` and pushes
// it through ForceFeedback.framework.
//
// Unit tests under `tests/unit/platform_macos_ffb.cpp` exercise the
// descriptor builder only — `ffb_build_constant_force` is a pure function
// so it runs without any hardware.
namespace psynder::platform::macos {

namespace {

struct FfbState {
    io_service_t              hid_service     = MACH_PORT_NULL;
    FFDeviceObjectReference   device          = nullptr;
    FFEffectObjectReference   active_effect   = nullptr;
    bool                      device_opened   = false;
};

FfbState& ffb_state() {
    static FfbState s;
    return s;
}

// Walk the IOKit HID registry for the first device that advertises a
// ForceFeedback plug-in. Returns MACH_PORT_NULL when nothing is found.
// The returned io_service_t is owned by the caller (release via
// IOObjectRelease).
io_service_t ffb_find_first_capable_device() noexcept {
    CFMutableDictionaryRef match = IOServiceMatching(kIOHIDDeviceKey);
    if (!match) return MACH_PORT_NULL;
    io_iterator_t it = MACH_PORT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) {
        return MACH_PORT_NULL;
    }
    io_service_t hit = MACH_PORT_NULL;
    io_service_t svc;
    while ((svc = IOIteratorNext(it)) != MACH_PORT_NULL) {
        if (FFIsForceFeedback(svc) == FF_OK) {
            hit = svc;
            break;
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return hit;
}

}  // namespace

FfbConstantForceWire ffb_build_constant_force(const FfbConstantForce& d) noexcept {
    // Clamp + scale to the DirectInput integer convention.
    auto clamp01 = [](f32 v) -> f32 {
        if (v <  0.0f) return 0.0f;
        if (v >  1.0f) return 1.0f;
        return v;
    };
    auto clamp_signed = [](f32 v) -> f32 {
        if (v < -1.0f) return -1.0f;
        if (v >  1.0f) return  1.0f;
        return v;
    };
    FfbConstantForceWire w{};
    const f32 mag = clamp_signed(d.magnitude);
    w.magnitude = static_cast<i32>(std::round(mag * 10000.0f));

    // Cartesian planar direction: convert degrees to (x, y) unit vector and
    // scale to ±10000. We don't enforce normalization on the input — a zero-
    // magnitude direction is fine; we just emit (0, 0). FF expects the
    // direction array to be in axis-units of 10000.
    const f32 dir_rad = d.direction_deg * (math::kPi / 180.0f);
    const f32 cx = std::cos(dir_rad);
    const f32 cy = std::sin(dir_rad);
    w.direction_x = static_cast<i32>(std::round(cx * 10000.0f));
    w.direction_y = static_cast<i32>(std::round(cy * 10000.0f));

    w.duration      = (d.duration_us == 0u) ? FF_INFINITE : d.duration_us;
    w.sample_period = d.sample_period_us;
    w.start_delay   = d.start_delay_us;
    w.gain          = static_cast<u32>(std::round(clamp01(d.gain) * 10000.0f));
    return w;
}

bool ffb_submit_constant_force(const FfbConstantForce& d) {
    // Validate / build the wire descriptor regardless of device presence.
    // Tests rely on the build step running pure; submission itself is
    // best-effort and a no-op without hardware.
    FfbConstantForceWire w = ffb_build_constant_force(d);

    FfbState& s = ffb_state();
    if (!s.device_opened) {
        s.hid_service = ffb_find_first_capable_device();
        if (s.hid_service == MACH_PORT_NULL) return false;
        HRESULT rc = FFCreateDevice(s.hid_service, &s.device);
        if (rc != FF_OK) {
            IOObjectRelease(s.hid_service);
            s.hid_service = MACH_PORT_NULL;
            s.device      = nullptr;
            return false;
        }
        s.device_opened = true;
    }
    if (!s.device) return false;

    // Build the FFEFFECT shell. Two axes (X, Y) with Cartesian direction.
    DWORD axes[2] = { FFJOFS_X, FFJOFS_Y };
    LONG  dir[2]  = { static_cast<LONG>(w.direction_x),
                      static_cast<LONG>(w.direction_y) };
    FFCONSTANTFORCE cf{};
    cf.lMagnitude = static_cast<LONG>(w.magnitude);

    FFEFFECT eff{};
    eff.dwSize                  = sizeof(FFEFFECT);
    eff.dwFlags                 = FFEFF_CARTESIAN | FFEFF_OBJECTOFFSETS;
    eff.dwDuration              = w.duration;
    eff.dwSamplePeriod          = w.sample_period;
    eff.dwGain                  = w.gain;
    eff.dwTriggerButton         = FFEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes                   = 2;
    eff.rgdwAxes                = axes;
    eff.rglDirection            = dir;
    eff.lpEnvelope              = nullptr;
    eff.cbTypeSpecificParams    = sizeof(cf);
    eff.lpvTypeSpecificParams   = &cf;
    eff.dwStartDelay            = w.start_delay;

    if (s.active_effect) {
        // Update an in-flight effect rather than churning the slot.
        HRESULT rc = FFEffectSetParameters(s.active_effect, &eff,
                                           FFEP_TYPESPECIFICPARAMS |
                                           FFEP_DIRECTION |
                                           FFEP_DURATION |
                                           FFEP_GAIN |
                                           FFEP_START);
        return rc == FF_OK;
    }
    HRESULT rc = FFDeviceCreateEffect(s.device,
                                      kFFEffectType_ConstantForce_ID,
                                      &eff, &s.active_effect);
    if (rc != FF_OK || !s.active_effect) {
        s.active_effect = nullptr;
        return false;
    }
    FFEffectStart(s.active_effect, /*iterations*/ 1, /*flags*/ 0);
    return true;
}

void ffb_shutdown() {
    FfbState& s = ffb_state();
    if (s.active_effect && s.device) {
        FFDeviceReleaseEffect(s.device, s.active_effect);
    }
    s.active_effect = nullptr;
    if (s.device) {
        FFReleaseDevice(s.device);
        s.device = nullptr;
    }
    if (s.hid_service != MACH_PORT_NULL) {
        IOObjectRelease(s.hid_service);
        s.hid_service = MACH_PORT_NULL;
    }
    s.device_opened = false;
}

}  // namespace psynder::platform::macos
