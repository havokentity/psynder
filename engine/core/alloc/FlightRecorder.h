// SPDX-License-Identifier: MIT
// Psynder — allocation flight recorder (debug builds only).
//
// DESIGN.md §4.7: "every alloc/free recorded with a small stack hash;
// leak detection at scope exit prints the offending site." The recorder
// is a circular ring of the last N entries -- enough context to find the
// offending site without paying for full backtraces every alloc. The ring
// is lock-free for the writer (one relaxed atomic counter), so the cost
// per recorded allocation is one bounded write + one fetch_add.
//
// The recorder is a header-declared API; the ring buffer lives in
// Allocator.cpp behind a file-static array. In NDEBUG builds every entry
// point degrades to a cheap inline no-op so release-mode allocations
// don't pay for the recorder. We still expose the C++ symbols so callers
// can compile-test the recorder API in release builds without #ifdef
// guards at every call site.

#pragma once

#include "../Types.h"
#include "Allocator.h"

namespace psynder::mem {

// One entry in the ring buffer. `site` is a stable hash of the call site
// (`call_site_hash(file, line)` below); `size` is the requested byte
// count; `tag` is the allocation category; `op` is +1 for alloc, -1 for
// free. The serial counter is the monotonically-increasing record index
// (used for ordering when the ring has wrapped).
struct FlightEntry {
    u64 serial = 0;
    u32 site = 0;
    i32 op = 0;  // +1 alloc, -1 free
    u32 size = 0;
    Tag tag = Tag::Misc;
    u32 _pad = 0;  // keep 32-byte size for cache-friendly scans
};

// Stable 32-bit hash of (file, line). The hash collapses the (file*, line)
// pair into a u32 -- collisions are tolerated, the goal is leak triage,
// not a guaranteed-unique site ID. The implementation is FNV-1a over the
// basename + an interleaved line counter, kept simple and inline so the
// hot path (PSY_ALLOC_TRACK) doesn't pay a function call.
constexpr u32 call_site_hash(const char* file, int line) noexcept {
    u32 h = 0x811C9DC5u;
    for (const char* c = file; c && *c; ++c) {
        h ^= static_cast<u8>(*c);
        h *= 0x01000193u;
    }
    h ^= static_cast<u32>(line);
    h *= 0x01000193u;
    return h;
}

// Configure the ring. Power-of-two `capacity` (else clamped down). Defaults
// at first record to 1024 entries.
void flight_recorder_init(usize capacity) noexcept;

// Record one allocation (op = +1) or free (op = -1). Lock-free for the
// writer. Always safe to call from any thread.
void flight_recorder_record(u32 site, i32 op, u32 size, Tag tag) noexcept;

// Number of entries successfully recorded so far. May exceed capacity if
// the ring has wrapped.
u64 flight_recorder_count() noexcept;

// Configured capacity (0 if not initialised yet).
usize flight_recorder_capacity() noexcept;

// Drop all entries; resets the serial counter to 0.
void flight_recorder_clear() noexcept;

// Dump the ring to a file at `path`. Format is one record per line:
//   `serial,site,op,size,tag`
// The file is overwritten on each call. Returns the number of records
// written (0 on I/O failure). Records are emitted oldest-to-newest based
// on the serial counter; wrapped slots are reconstructed correctly.
usize flight_recorder_dump(const char* path) noexcept;

// Convenience macro that records the file/line of the call site. Use this
// from within allocator paths that want to participate in the flight
// recorder; passing 0 for the hash skips hashing entirely.
#define PSY_FLIGHT_RECORD(op, size, tag)                                                       \
    ::psynder::mem::flight_recorder_record(::psynder::mem::call_site_hash(__FILE__, __LINE__), \
                                           (op),                                               \
                                           static_cast<::psynder::u32>(size),                  \
                                           (tag))

}  // namespace psynder::mem
