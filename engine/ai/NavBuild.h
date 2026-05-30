// SPDX-License-Identifier: MIT
// Psynder — M-AI navigation OCCUPANCY BUILDER + true-funnel string-pull.
//
// Auto-build the NavGrid occupancy from a host's STATIC box geometry so AI can
// path INDOORS (around walls / cover) without hand-marking cells. Today a host
// either hand-calls NavGrid::set_blocked per cell or block_aabb per collider;
// for a rotated box or a corner-tight indoor room that is fiddly and error
// prone. This module rasterizes a LIST of static boxes (axis-aligned OR rotated
// about the up axis) into the grid in ONE call, with an AGENT-RADIUS inflation
// so agents do not clip the corners of those obstacles.
//
// HOST-AGNOSTIC, like the rest of engine/ai: this module pulls in NOTHING from
// physics / render / world / host. The host hands us its own static box list
// (center / half-extent / yaw — exactly the fields a physics::Box body already
// carries) and we stamp the grid the host owns. No dependency flows the other
// way (no physics -> ai).
//
// DETERMINISM / DOTS contract:
//   * Pure integer-cell rasterization: a box's world AABB (grown by the agent
//     radius) bounds the cell scan; each candidate cell is tested for overlap
//     with the (optionally rotated) box inflated by the agent radius via an
//     exact box-vs-box separating-axis distance in the box's LOCAL frame. No
//     RNG, no clock, no float-order dependence — the same box list + grid always
//     yields the identical occupancy across runs / threads / builds.
//   * ALLOC-FREE: the grid is host-owned (sized once at NavGrid::resize); the
//     builder only writes occupancy bytes into it. No scratch is allocated here
//     at all — the box list is a caller-owned span. Steady-state rebuilds (call
//     NavGrid::clear_blocked then build_nav_occupancy again) touch zero heap.
//
// The TRUE FUNNEL (funnel_path) is the "simple stupid funnel algorithm" over the
// grid path's portal edges — a tighter, optimal-within-the-corridor string-pull
// that the greedy LOS-skip smoother in NavGrid.cpp approximates. It is offered
// as an alternative smoother; the existing smooth_path is left untouched.

#pragma once

#include "ai/NavGrid.h"
#include "core/Types.h"
#include "math/Math.h"

#include <span>

namespace psynder::ai {

using ::psynder::f32;
using ::psynder::i32;
using ::psynder::u32;

// ─── NavBox ──────────────────────────────────────────────────────────────────
// One static box obstacle in world space, in exactly the terms a host already
// has for a physics::Shape::Box body: a world-space CENTER, positive HALF-EXTENT
// per axis, and a YAW rotation (radians, counter-clockwise about the world +Y up
// axis). yaw == 0 is an axis-aligned box. Y is ignored for occupancy (the grid
// is 2.5D, XZ plane) but kept in the struct so a host can pass its bodies
// verbatim; only half_extent.x / half_extent.z and the yaw drive rasterization.
//
// Trivially copyable POD so a host can store a static array of these and hand a
// span to the builder with zero conversion.
struct NavBox {
    math::Vec3 center{0.0f, 0.0f, 0.0f};
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};
    f32 yaw = 0.0f;  // radians about world +Y; 0 => axis-aligned
};

// ─── build_nav_occupancy ─────────────────────────────────────────────────────
// Rasterize a list of STATIC boxes into `grid`'s occupancy: every cell whose
// CENTER lies within `agent_radius` metres of a box (i.e. the box inflated by the
// agent radius, Minkowski-style) is marked blocked, so an agent walking
// cell-to-cell keeps at least its radius of clearance from the obstacle and does
// not clip a corner. Existing blocked cells are PRESERVED (this OR-s into the
// grid); call NavGrid::clear_blocked() first for a clean rebuild.
//
// agent_radius >= 0 (a value < 0 is clamped to 0 => boxes rasterize to exactly
// the cells they overlap, no inflation). Boxes whose AABB falls entirely outside
// the grid contribute nothing. Deterministic + alloc-free: the grid is the only
// thing mutated; `boxes` is a caller-owned span.
//
// Returns the number of cells newly marked blocked by this call (cells already
// blocked are not double-counted), so a host / test can sanity-check the build.
u32 build_nav_occupancy(NavGrid& grid,
                        std::span<const NavBox> boxes,
                        f32 agent_radius) noexcept;

// Convenience single-box overload (axis-aligned or rotated). Same semantics.
u32 build_nav_occupancy(NavGrid& grid, const NavBox& box, f32 agent_radius) noexcept;

// ─── True funnel string-pull (simple stupid funnel algorithm) ────────────────
// Tighten a raw grid path (cell-centre waypoints, as find_path returns) into the
// shortest path that stays inside the path's corridor, using the classic funnel
// algorithm over the corridor's PORTAL edges. On a uniform grid the corridor is
// the strip of cells the raw path threads; each step between consecutive path
// cells defines a portal — the shared edge (orthogonal step) or shared corner
// (diagonal step) between the two cells, shrunk by the agent radius so the pulled
// string keeps clearance. The funnel then walks left/right apex bounds and emits
// a tight turn point only where the corridor actually bends.
//
// This replaces the greedy supersampled LOS-skip (smooth_path) with the exact
// corridor string-pull: the result is never LONGER than the greedy smoother and
// stays collision-free (it lives inside the radius-shrunk corridor the A* path
// already proved walkable). Operates in place on `path`; alloc-free +
// deterministic. Returns the new waypoint count.
//
// `agent_radius` should match the value passed to build_nav_occupancy so the
// pulled string keeps the same clearance the occupancy already guarantees; a
// radius of 0 pulls the string to the raw portal edges (cell boundaries).
u32 funnel_path(const NavGrid& grid, NavPath& path, f32 agent_radius) noexcept;

}  // namespace psynder::ai
