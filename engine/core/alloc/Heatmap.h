// SPDX-License-Identifier: MIT
// Psynder — allocator heatmap surface for the editor / bench gate.
//
// Wave B addition (DESIGN.md §4.7 "Live heatmap" + "Per-frame memory map
// dump"). The frozen public Allocator.h exposes per-tag scalar accessors
// (current_usage / peak_usage / set_budget); the editor heatmap wants all
// three values for every tag in a single non-tearing read so it can render
// a bar chart per frame. tag_stats() snapshots them. reset_peak_all() is
// the frame-boundary "high-water mark since last reset" hook the editor
// uses for per-frame peaks (not the lifetime peak).
//
// All entry points are pure reads against the atomics that back
// current_usage / peak_usage in Allocator.cpp, so this header doesn't
// widen the contract — it just packages what's already there.

#pragma once

#include "../Types.h"
#include "Allocator.h"

#include <array>

namespace psynder::mem {

// One row of the heatmap: current bytes outstanding, lifetime peak, and
// the configured budget. The bench gate (lane 25) compares `peak` against
// `budget`; the editor heatmap (lane 16) draws `current / budget` as a
// filled bar and `peak / budget` as an outline.
struct TagStat {
    Tag    tag     = Tag::Misc;
    usize  current = 0;
    usize  peak    = 0;
    usize  budget  = 0;
};

// Snapshot every tag in order [0 .. Tag::Count). The returned array is
// statically sized so callers don't need to allocate. Each row is read
// independently (no global lock), so two rows may reflect different
// points in time under contention — that's fine for a UI heatmap.
std::array<TagStat, static_cast<usize>(Tag::Count)> tag_stats() noexcept;

// Convenience single-row snapshot.
TagStat tag_stat(Tag tag) noexcept;

// Drop the per-tag peak watermarks back to the current `current` value.
// Called at frame boundary by the engine when the editor heatmap is in
// "per-frame peaks" mode. The lifetime peak (used by the bench gate) is
// recovered by *not* calling this from the bench path.
void reset_peak_all() noexcept;

// Single-tag variant for tests / spot resets.
void reset_peak(Tag tag) noexcept;

}  // namespace psynder::mem
