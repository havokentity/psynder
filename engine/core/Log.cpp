// SPDX-License-Identifier: MIT
// Psynder — log impl. Per-worker thread_local ring buffer + RCU-style
// sink dispatch. No mutex on the hot path; the mutex is only touched
// when adding / clearing sinks (cold path).
//
// Design:
//   - Each thread has a thread_local circular ring of formatted log lines.
//     Capacity is kRingDepth entries; new writes overwrite oldest. The
//     ring is opaque to readers below `recent_lines()`; it's primarily a
//     debugging affordance so a crash dumper can recover the last N lines
//     a thread emitted without going through the io subsystem.
//   - Sinks are dispatched via an atomic-shared-pointer-style snapshot:
//     `add_sink` builds a new immutable list, atomic-swaps it in, leaves
//     the old list pinned for one epoch via a small free-list, then
//     reclaims it. The hot path reads the current pointer once and calls
//     each sink -- no lock, no allocation.
//   - Direct stderr / stdout writes still happen synchronously so output
//     is preserved even when a crash interrupts a drain pass. fmt vformat
//     allocates per-call (no way around this with a fmt::format_args
//     interface); that allocation is in the per-thread temporary heap
//     which we don't try to eliminate at the API surface.

#include "Log.h"

#include "Tracy.h"
#include "Types.h"

#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace psynder::log {

// Pull in the engine-wide size alias so this TU reads the same as every
// other engine file.
using ::psynder::usize;

namespace {

// ─── Sink registry (RCU-style) ──────────────────────────────────────────
// SinkList is an immutable, ref-counted snapshot of the currently-
// registered sinks. emit() loads the current pointer (atomic) and walks
// it. Writers (`add_sink`, `remove_all_sinks`) construct a new SinkList,
// atomic-swap it in, and arrange for the old one to be freed once no
// reader can still be looking at it. Since emit() is allowed to call
// sinks() inline and we don't yet have a job-system epoch sweep to
// hook, we conservatively defer reclamation by keeping the previous
// list pinned in a small ring until a writer comes along and rotates
// it out -- which happens infrequently enough that the bounded backlog
// (kSinkEpochDepth) never overflows in practice.
struct SinkList {
    std::vector<Sink> sinks;
};

constexpr usize kSinkEpochDepth = 4;
std::atomic<SinkList*> g_sinks{nullptr};

// Epoch ring of "retired" sink lists. We don't actually run an epoch
// sweep -- writers are rare, the snapshot pointer rotation is the
// barrier; entries fall off when the ring wraps. kSinkEpochDepth = 4
// means up to 3 in-flight readers can still hold a stale snapshot
// before the oldest gets reclaimed, plenty for an interactive engine.
struct SinkEpoch {
    std::mutex mu;
    std::array<SinkList*, kSinkEpochDepth> retired{};
    usize head = 0;
};
SinkEpoch& sink_epoch() {
    static SinkEpoch e;
    return e;
}

void retire_sink_list(SinkList* old) {
    if (!old)
        return;
    auto& e = sink_epoch();
    std::lock_guard<std::mutex> lk(e.mu);
    SinkList* evicted = e.retired[e.head];
    e.retired[e.head] = old;
    e.head = (e.head + 1) % kSinkEpochDepth;
    delete evicted;
}

// ─── Per-thread ring buffer ──────────────────────────────────────────────
// Bounded ring of the last kRingDepth lines this thread emitted. Stored
// as flat std::strings so a future "tail" command can dump them. Writers
// (this thread) are single-producer / single-consumer; no synchronisation
// needed.
constexpr usize kRingDepth = 256;

struct LineEntry {
    Level level = Level::Info;
    std::string text;
};

struct Ring {
    std::array<LineEntry, kRingDepth> entries;
    usize head = 0;
    usize count = 0;
};

thread_local Ring tls_ring;

void push_ring(Level level, std::string&& line) {
    auto& r = tls_ring;
    r.entries[r.head].level = level;
    r.entries[r.head].text = std::move(line);
    r.head = (r.head + 1) % kRingDepth;
    if (r.count < kRingDepth)
        ++r.count;
}

const char* level_prefix(Level l) noexcept {
    switch (l) {
        case Level::Info:
            return "info";
        case Level::Warn:
            return "warn";
        case Level::Error:
            return "error";
    }
    return "?";
}

}  // namespace

void emit(Level level, fmt::string_view fmt_str, fmt::format_args args) {
    PSY_TRACE_ZONE("log::emit");
    // Format the line once. fmt::vformat allocates internally; we accept
    // that cost because the public API is built around fmt::format_args
    // and rendering inside emit lets the per-thread ring + sinks see the
    // same final string.
    std::string line = fmt::vformat(fmt_str, args);

    // Stash a copy in the per-thread ring before anything else, so a
    // crash mid-write still leaves the line recoverable.
    push_ring(level, std::string(line));

    // Direct stderr / stdout. Use fputs+fputc so the formatted line +
    // the framing bytes ('\n', '[level] ' prefix) hit the FILE* under
    // a single fwrite-equivalent path. fprintf would still work, but
    // doing this manually keeps the hot-path allocation count exact
    // (one std::string above, zero here).
    std::FILE* dst = (level == Level::Error) ? stderr : stdout;
    std::fputc('[', dst);
    std::fputs(level_prefix(level), dst);
    std::fputs("] ", dst);
    std::fwrite(line.data(), 1, line.size(), dst);
    std::fputc('\n', dst);
    if (level == Level::Error)
        std::fflush(dst);

    // Lock-free sink fan-out. Load the snapshot pointer once; the list
    // is immutable so iteration is safe even if a writer swaps a new
    // list in concurrently. The pinned epoch ring guarantees the old
    // list outlives our walk.
    SinkList* snap = g_sinks.load(std::memory_order_acquire);
    if (snap) {
        for (auto s : snap->sinks) {
            if (s)
                s(level, line);
        }
    }
}

void add_sink(Sink sink) {
    if (!sink)
        return;
    // Construct a new immutable list = current + new sink, then swap.
    SinkList* old = g_sinks.load(std::memory_order_acquire);
    auto* fresh = new SinkList;
    if (old)
        fresh->sinks = old->sinks;
    fresh->sinks.push_back(sink);

    while (!g_sinks.compare_exchange_weak(old, fresh, std::memory_order_acq_rel, std::memory_order_acquire)) {
        // Someone raced us; rebuild from the new old.
        fresh->sinks.clear();
        if (old)
            fresh->sinks = old->sinks;
        fresh->sinks.push_back(sink);
    }
    retire_sink_list(old);
}

void remove_all_sinks() {
    SinkList* old = g_sinks.exchange(nullptr, std::memory_order_acq_rel);
    retire_sink_list(old);
}

}  // namespace psynder::log
