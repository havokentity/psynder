// SPDX-License-Identifier: MIT
// Psynder — Tracy profiler wrapper. Header-only; expands to either real
// Tracy macros or no-op stubs depending on `PSYNDER_ENABLE_TRACY`.
//
// Usage:
//   #include "core/Tracy.h"
//   void run_frame() {
//       PSY_TRACE_FRAME("main");         // marks one full frame (Wave D)
//       {
//           PSY_TRACE_ZONE("vertex transform");
//           // ...
//       }
//       PSY_TRACE_PLOT_F32("draws", float(draw_count));   // Wave D
//       PSY_TRACE_MESSAGE("frame submitted");             // Wave D
//   }
//
// When PSYNDER_ENABLE_TRACY is off (the default), every macro collapses
// to `do {} while(0)`. The compiler eliminates the call sites entirely,
// so leaving zones sprinkled through the codebase costs nothing.
//
// When PSYNDER_ENABLE_TRACY is on, the macros forward to <tracy/Tracy.hpp>:
//   PSY_ZONE(name)              -> ZoneScopedN(name)
//   PSY_TRACE_ZONE(name)        -> ZoneScopedN(name)  (Wave B preferred spelling)
//   PSY_TRACE_ZONE_COLOR(n, rgb)-> ZoneScopedNC(n, rgb)
//   PSY_ZONE_FRAME()            -> FrameMark
//   PSY_TRACE_FRAME(name)       -> FrameMark / FrameMarkNamed(name) (Wave D)
//   PSY_TRACE_PLOT_F32(name, v) -> TracyPlot(name, v) for live counter plots (Wave D)
//   PSY_TRACE_MESSAGE(text)     -> TracyMessageL(text) one-off event log (Wave D)
//   PSY_ZONE_ALLOC(p,n)         -> TracyAlloc(p,n) for allocator instrumentation
//   PSY_ZONE_FREE(p)            -> TracyFree(p)
//   PSY_ZONE_MSG(s,n)           -> TracyMessage(s,n)
//
// The tracy CMake option lives in the root CMakeLists.txt; lane 01 owns
// the FetchContent wiring under engine/core/CMakeLists.txt so callers
// don't need to thread CMake conditionals through their own subdirectory.

#pragma once

#if defined(PSYNDER_ENABLE_TRACY) && PSYNDER_ENABLE_TRACY

#include <tracy/Tracy.hpp>

#define PSY_ZONE(name) ZoneScopedN(name)
#define PSY_ZONE_NAMED(var, name) ZoneNamedN(var, name, true)
#define PSY_ZONE_FRAME() FrameMark
#define PSY_ZONE_FRAME_NAMED(name) FrameMarkNamed(name)
#define PSY_ZONE_PLOT(name, value) TracyPlot(name, value)
#define PSY_ZONE_MSG(str, n) TracyMessage(str, n)
#define PSY_ZONE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define PSY_ZONE_FREE(ptr) TracyFree(ptr)

// ─── Wave B preferred spellings ──────────────────────────────────────────
// PSY_TRACE_ZONE(name) is an alias of PSY_ZONE(name); PSY_TRACE_ZONE_COLOR
// adds a uint32_t 0xRRGGBB tint so callers can colour their per-subsystem
// zones (raster=blue, physics=green, audio=purple, ...) in the Tracy GUI.
#define PSY_TRACE_ZONE(name) ZoneScopedN(name)
#define PSY_TRACE_ZONE_COLOR(name, rgb) ZoneScopedNC(name, (rgb))

// ─── Wave D additions ────────────────────────────────────────────────────
// PSY_TRACE_FRAME(name)     — call once per top-level frame to delimit a
//                              frame on the Tracy timeline. When (name) is
//                              a string literal we forward to FrameMarkNamed
//                              so multiple top-level loops (main, audio,
//                              physics tick) can each have their own track.
// PSY_TRACE_PLOT_F32(n,v)   — push a float counter value at `now`. Tracy
//                              graphs the value as a continuous plot;
//                              callers use this to surface live counters
//                              (allocator current/peak, queue depth, ...).
// PSY_TRACE_MESSAGE(text)   — emit a one-off log line on the Tracy timeline.
//                              Use for state transitions / one-shot events
//                              (level loaded, world streamed in, etc.).
#define PSY_TRACE_FRAME(name) FrameMarkNamed(name)
#define PSY_TRACE_PLOT_F32(name, val) TracyPlot(name, static_cast<float>(val))
#define PSY_TRACE_MESSAGE(text) TracyMessageL(text)

#else

#define PSY_ZONE(name) \
    do {               \
    } while (0)
#define PSY_ZONE_NAMED(var, name) \
    do {                          \
    } while (0)
#define PSY_ZONE_FRAME() \
    do {                 \
    } while (0)
#define PSY_ZONE_FRAME_NAMED(name) \
    do {                           \
    } while (0)
#define PSY_ZONE_PLOT(name, value) \
    do {                           \
    } while (0)
#define PSY_ZONE_MSG(str, n) \
    do {                     \
    } while (0)
#define PSY_ZONE_ALLOC(ptr, size) \
    do {                          \
    } while (0)
#define PSY_ZONE_FREE(ptr) \
    do {                   \
    } while (0)

#define PSY_TRACE_ZONE(name) \
    do {                     \
    } while (0)
#define PSY_TRACE_ZONE_COLOR(name, rgb) \
    do {                                \
    } while (0)

// Wave D no-op stubs. The cast to (void) inside the do-block keeps the
// arguments from being flagged as unused while still expanding to a
// zero-cost statement the optimizer drops outright.
#define PSY_TRACE_FRAME(name) \
    do {                      \
        (void)sizeof(name);   \
    } while (0)
#define PSY_TRACE_PLOT_F32(name, val) \
    do {                              \
        (void)sizeof(name);           \
        (void)sizeof(val);            \
    } while (0)
#define PSY_TRACE_MESSAGE(text) \
    do {                        \
        (void)sizeof(text);     \
    } while (0)

#endif
