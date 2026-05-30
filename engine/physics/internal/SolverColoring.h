// SPDX-License-Identifier: MIT
// Psynder physics — deterministic graph coloring for the parallel
// projected-Gauss-Seidel solver (DESIGN.md §16 ADR-013).
//
// THE PROBLEM. The legacy per-island solver is sequential-impulse (a.k.a.
// projected Gauss-Seidel): contact i reads the body velocities that contact
// i-1 just wrote. That read-after-write chain is the source of PGS's good
// convergence, but it also makes the inner velocity-iteration loop strictly
// serial — there is no way to SIMD or thread it while reproducing the exact
// same numbers, because every constraint depends on its predecessor's writes.
//
// THE FIX (graph-colored parallel PGS). Within one island we 2-colour... no,
// k-colour the *constraint graph*: a vertex per contact, an edge between two
// contacts iff they share a DYNAMIC body. A proper colouring guarantees that
// all contacts of a single colour touch PAIRWISE-DISJOINT dynamic bodies, so
// applying their impulses can happen in any order — including fully in
// parallel — and the body-velocity writes never race. Colours are then solved
// SEQUENTIALLY (colour 0 fully, then colour 1, ...). Because each colour sees
// the velocities the previous colour wrote, the scheme stays Gauss-Seidel
// *across* colours (good convergence, unlike pure Jacobi) while being Jacobi
// (parallel) *within* a colour. This is the standard batched-constraint-solver
// technique (Tonge, "Solving Rigid Body Contacts", GDC 2012; Coumans' Bullet
// parallel constraint batching; "graph colouring for parallel impulse solvers").
//
// DETERMINISM (hard requirement, DESIGN.md §10.1). The colouring is a pure
// deterministic function of the input contact array:
//   * Greedy colouring in ASCENDING contact-index order. Contact i takes the
//     smallest colour not already used by an earlier (lower-index) contact
//     that shares a dynamic body with it.
//   * Within a colour the contacts are emitted in ascending contact-index
//     order (a stable bucket fill), so the per-colour iteration order is fixed.
//   * No RNG, no clock, no address-derived ordering, no atomics in the
//     colouring itself.
// Two runs on identical input therefore produce an identical colouring and an
// identical per-colour order. Combined with disjoint-body batches (so the
// parallel_for result is independent of thread scheduling) the whole solve is
// bit-for-bit reproducible run to run — even though the bit values differ from
// the OLD plain-index PGS, because the solve order changed BY DESIGN.
//
// STATIC BODIES never receive velocity writes (inv_mass == 0), so two contacts
// that share only a static body do NOT conflict and may land in the same
// colour. Treating the static body as a conflict would needlessly inflate the
// colour count for the common "many dynamic bodies resting on one big static
// floor" island — exactly the island we most want to parallelise.
//
// ZERO PER-FRAME HEAP. The colouring writes into caller-owned scratch
// (ColoringScratch) whose vectors are sized once to the island's contact count
// and reused every sub-tick (clear-not-free). The only auxiliary structure is a
// tiny fixed-capacity bitset of "colours already used by my neighbours", kept
// on the stack.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "physics/Body.h"
#include "physics/Narrowphase.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace psynder::physics::detail::kernels {

// Maximum colour count the fast (stack-bitset) greedy path supports. A planar
// resting contact graph rarely needs more than a handful of colours; even a
// dense pile tops out well under this. If an island somehow exceeds it (e.g. a
// single body sharing >kMaxColors simultaneous dynamic contacts) we still
// produce a CORRECT colouring by falling back to a wider linear scan — see
// pick_color below — so this is a perf knob, not a correctness bound.
inline constexpr u32 kMaxSolverColors = 64u;

// Island-level gate: below this contact count the whole island runs the serial
// colour walk. The per-contact solve is LIGHT, so in an optimised build the
// parallel_for fork/join only beats the (already vectorisable) serial colour
// walk for genuinely large islands — break-even measured at a few thousand
// contacts on a 15-worker M-series. Typical game islands sit well under this and
// stay on the cheap serial-colored path; only big piles (thousands of resting
// contacts) opt into multicore. Bit-identical either way, so this is purely a
// perf gate, never a correctness one.
inline constexpr u32 kColoredParallelThreshold = 2048u;

// Per-COLOUR gate inside the parallel dispatcher: a colour smaller than this
// runs inline (no task submit), so the many tiny box-box colours of a large
// island never pay a fork/join. Only the big colours (e.g. the hundreds/
// thousands of contacts that all share a single static floor) parallelise.
inline constexpr u32 kColoredColorMinParallel = 512u;

// parallel_for grain for a parallelised colour. Chunk-submission cost — not load
// imbalance — dominates at this work density, so the grain is large (fewer,
// fatter chunks). ~512 measured as the sweet spot; larger underutilises cores on
// the dominant colour, smaller over-submits.
inline constexpr u32 kColoredParallelGrain = 512u;

// Caller-owned, island-reused scratch for one colouring. All three vectors are
// indexed [0, contact_count); `color_offsets` is sized [0, num_colors] (CSR
// row pointers). Nothing here is freed between frames — only resized up.
struct ColoringScratch {
    // color_of[i] = colour assigned to contact i (island-local index).
    std::vector<u32> color_of;
    // CSR bucket of contact indices grouped by colour, ascending within a
    // colour. order[ color_offsets[c] .. color_offsets[c+1] ) are the contacts
    // of colour c, in ascending island-local index order.
    std::vector<u32> order;
    std::vector<u32> color_offsets;  // size num_colors+1
    std::vector<u32> cursor;         // per-colour write cursor (counting sort)
    u32 num_colors = 0;

    void ensure(usize contact_count) {
        if (color_of.size() < contact_count)
            color_of.resize(contact_count);
        if (order.size() < contact_count)
            order.resize(contact_count);
        // color_offsets/cursor are at most contact_count+1 (worst case: every
        // contact its own colour). Pre-size to the upper bound so the
        // counting-sort fill never grows mid-frame.
        if (color_offsets.size() < contact_count + 1)
            color_offsets.resize(contact_count + 1);
        if (cursor.size() < contact_count + 1)
            cursor.resize(contact_count + 1);
    }
};

// Per-body "highest colour seen so far on a contact touching this body" map,
// keyed by the GLOBAL body id. Greedy colouring needs, for contact i with
// dynamic bodies (a,b), the set of colours already used by lower-index contacts
// touching a or b. We avoid an O(colour) neighbour rescan by walking a tiny
// fixed bitset built from the two incident bodies' colour-usage rows. The rows
// live in `body_color_used`, a [num_bodies] x ceil(kMaxSolverColors/64) bitset
// matrix kept in caller scratch and cleared lazily via a stamp.
struct BodyColorUsage {
    static constexpr u32 kWords = (kMaxSolverColors + 63u) / 64u;
    // One bitset row per body slot; flat [body * kWords + word]. Plus a per-body
    // generation stamp so we can "clear" the whole structure in O(1) by bumping
    // the global stamp instead of zeroing every row each island.
    std::vector<u64> bits;     // size num_bodies * kWords
    std::vector<u32> stamp;    // size num_bodies
    u32 cur_stamp = 0;

    void ensure(usize num_bodies) {
        if (bits.size() < num_bodies * kWords)
            bits.assign(num_bodies * kWords, 0u);
        if (stamp.size() < num_bodies)
            stamp.assign(num_bodies, 0u);
    }
    // Start a fresh colouring: invalidate every row in O(1).
    void begin() { ++cur_stamp; }

    // The live colour-usage row for `body`. If its stamp is stale, treat it as
    // all-zero and re-stamp before returning (lazy clear).
    u64* row(u32 body) {
        u64* r = &bits[static_cast<usize>(body) * kWords];
        if (stamp[body] != cur_stamp) {
            for (u32 w = 0; w < kWords; ++w)
                r[w] = 0u;
            stamp[body] = cur_stamp;
        }
        return r;
    }
};

// Pick the smallest colour in [0, kMaxSolverColors) not present in `used` (the
// union of the two incident dynamic bodies' colour rows). On success sets
// `*sat = false` and returns that colour. If ALL kMaxSolverColors are already
// taken (pathological: a body with >= kMaxSolverColors simultaneous dynamic
// contacts) it sets `*sat = true` and returns kMaxSolverColors-1; the caller
// then drops the whole island to the serial path, where a re-used colour is
// harmless because serial never runs a colour's contacts in parallel.
PSY_FORCEINLINE u32 pick_color_from_words(const u64* used, bool* sat) noexcept {
    for (u32 w = 0; w < BodyColorUsage::kWords; ++w) {
        u64 free_bits = ~used[w];
        if (free_bits != 0u) {
            u32 bit = static_cast<u32>(__builtin_ctzll(free_bits));
            u32 color = w * 64u + bit;
            if (color < kMaxSolverColors) {
                *sat = false;
                return color;
            }
        }
    }
    *sat = true;
    return kMaxSolverColors - 1u;  // saturated
}

// Map a GLOBAL body id to its dense island-local slot via the island's sorted
// unique body-id set (`body_indices`, produced by kernel_detect_islands). This
// keeps the per-island colour-usage bitset sized to the island's body COUNT
// (not the world's max body id), so total scratch across N islands is O(total
// bodies), never O(N x max_body_id). Deterministic: binary search on a sorted
// array. Returns body_count (an out-of-set sentinel) if not found, which only
// happens for a static body absent from the dynamic set — callers gate on
// inv_mass before mapping, so that case never reaches here in practice.
PSY_FORCEINLINE u32 body_local_slot(u32 global, std::span<const u32> body_indices) noexcept {
    usize lo = 0, hi = body_indices.size();
    while (lo < hi) {
        usize mid = lo + (hi - lo) / 2;
        if (body_indices[mid] < global)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < body_indices.size() && body_indices[lo] == global)
               ? static_cast<u32>(lo)
               : static_cast<u32>(body_indices.size());
}

// Deterministic greedy colouring of one island's contacts.
//
// `contacts` is the island's contact slice; `bodies` is the FULL body array
// (contacts hold global body ids); `body_indices` is the island's sorted unique
// body-id set (for dense local remapping of the usage bitset). `usage` and
// `out` are caller scratch. `usage` must be ensure()d to >= body_indices.size()
// BEFORE this call. `saturated` is set true iff the colouring hit
// kMaxSolverColors (caller then takes the serial path: correctness preserved,
// parallelism skipped).
//
// Returns the number of colours used.
inline u32 kernel_color_island(std::span<const Contact> contacts,
                               std::span<const Body> bodies,
                               std::span<const u32> body_indices,
                               BodyColorUsage& usage,
                               ColoringScratch& out,
                               bool& saturated) {
    const usize n = contacts.size();
    out.ensure(n);
    usage.begin();
    saturated = false;
    u32 num_colors = 0;

    // Pass 1 — greedy colour in ascending contact order. Mark each chosen
    // colour on BOTH incident dynamic bodies so later contacts that share a
    // body avoid it.
    for (usize i = 0; i < n; ++i) {
        const Contact& c = contacts[i];
        const bool a_dyn = bodies[c.body_a].inv_mass > 0.0f;
        const bool b_dyn = bodies[c.body_b].inv_mass > 0.0f;

        // Union of colours already used by the two incident DYNAMIC bodies.
        // Rows are keyed by the dense island-LOCAL slot so the bitset is sized
        // to the island's body count, not the world's max body id.
        u64 used[BodyColorUsage::kWords];
        for (u32 w = 0; w < BodyColorUsage::kWords; ++w)
            used[w] = 0u;
        u64* ra = a_dyn ? usage.row(body_local_slot(c.body_a, body_indices)) : nullptr;
        u64* rb = b_dyn ? usage.row(body_local_slot(c.body_b, body_indices)) : nullptr;
        if (ra)
            for (u32 w = 0; w < BodyColorUsage::kWords; ++w)
                used[w] |= ra[w];
        if (rb)
            for (u32 w = 0; w < BodyColorUsage::kWords; ++w)
                used[w] |= rb[w];

        bool this_sat = false;
        u32 color = pick_color_from_words(used, &this_sat);
        if (this_sat)
            saturated = true;
        out.color_of[i] = color;
        num_colors = (color + 1u > num_colors) ? color + 1u : num_colors;

        const u32 word = color / 64u;
        const u64 mask = 1ull << (color & 63u);
        if (ra)
            ra[word] |= mask;
        if (rb)
            rb[word] |= mask;
    }

    // Pass 2 — counting sort into CSR buckets. We walk i ascending and place
    // each contact at its colour's running write cursor, so every colour's run
    // stays in ascending contact-index order (the fixed per-colour iteration
    // order the determinism argument relies on). cursor/color_offsets were
    // pre-sized in ensure(); no allocation here.
    for (u32 c = 0; c <= num_colors; ++c)
        out.color_offsets[c] = 0u;
    for (usize i = 0; i < n; ++i)
        ++out.color_offsets[out.color_of[i] + 1u];
    for (u32 c = 0; c < num_colors; ++c)
        out.color_offsets[c + 1u] += out.color_offsets[c];
    for (u32 c = 0; c < num_colors; ++c)
        out.cursor[c] = out.color_offsets[c];
    for (usize i = 0; i < n; ++i) {
        u32 col = out.color_of[i];
        out.order[out.cursor[col]++] = static_cast<u32>(i);
    }

    out.num_colors = num_colors;
    return num_colors;
}

// ── Solver-side shared types (used by Kernels.h's colored PGS) ────────────

// Per-contact precomputed constraint cache. One entry per contact in the
// island; index-aligned with the island's `contacts` slice.
struct ContactConstraint {
    math::Vec3 ra, rb;
    math::Vec3 t1, t2;
    f32 eff_n = 0.0f, eff_t1 = 0.0f, eff_t2 = 0.0f;
    f32 bias = 0.0f, e = 0.0f, mu = 0.0f;
};

// Per-colour batch dispatcher. The core solver hands each colour's contact
// count plus a body functor; the dispatcher invokes `fn(lo, hi)` over sub-
// ranges of [0, count) covering the whole range exactly once. The serial
// dispatcher runs it on the calling thread; the colored production dispatcher
// (bound in World.cpp) routes it through the job system's parallel_for. Because
// a colour's contacts touch pairwise-disjoint dynamic bodies, the sub-range
// split is race-free regardless of how it is partitioned. Held as std::function
// so Kernels.h needs no dependency on jobs/.
using ColorBatchDispatch =
    std::function<void(usize count, const std::function<void(usize, usize)>& fn)>;

// Serial dispatcher: the whole batch on this thread, ascending order. Default
// for kernel_solve_island (tests + small-island fallback); bit-identical to the
// parallel dispatcher (disjoint bodies => order-free).
inline void solver_serial_dispatch(usize count,
                                   const std::function<void(usize, usize)>& fn) {
    fn(0, count);
}

// Pooled scratch for ONE island's colored solve. Lives on the WorldState so the
// hot path never heap-allocates per frame (zero per-frame garbage, DESIGN §3.4).
// One instance per CONCURRENTLY-SOLVED island.
struct ColoredIslandScratch {
    std::vector<ContactConstraint> cache;
    ColoringScratch coloring;
    BodyColorUsage usage;
};

}  // namespace psynder::physics::detail::kernels
