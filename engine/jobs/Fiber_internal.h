// SPDX-License-Identifier: MIT
// Psynder — Wave B: Fiber primitive. Lane 04 owned.
//
// DESIGN.md §2.4 calls for fibers for cooperative long-running tasks
// (lightmap bake, BVH refit) that can yield mid-bounce without stalling a
// worker. This header exposes a thin, platform-agnostic Fiber type:
//
//   - Windows: `CreateFiberEx` / `SwitchToFiber` / `DeleteFiber`. A thread
//     must be a fiber (via `ConvertThreadToFiber`) before it can switch.
//   - POSIX:   `getcontext` / `makecontext` / `swapcontext`. `ucontext.h`
//     is deprecated on macOS but still functional (we silence the
//     deprecation diagnostic); it remains the most portable user-space
//     fiber primitive without dropping to per-arch assembly.
//
// The Fiber API is deliberately small:
//
//   - `Fiber::create(stack_size, fn, user)` allocates a fiber whose entry
//     point is `fn(user)`. The fiber starts suspended.
//   - `Fiber::resume()` switches the current OS thread *into* the fiber.
//     The thread's previous context is captured automatically as the
//     "host" — the fiber must yield back to it.
//   - `Fiber::yield()` (static, free function inside the fiber) switches
//     back to whoever resumed us.
//
// Reentry: a fiber may be resumed multiple times; each resume continues
// from the previous yield point. When `fn` returns, the fiber marks itself
// `done()` and yields one last time; further resumes are no-ops.
//
// Worker-thread integration:
//   `FiberWorker` is a thin helper that owns the current thread's host
//   context. A worker thread calls `make_thread_a_fiber_host()` once on
//   entry (on Windows that's `ConvertThreadToFiber`; POSIX captures
//   the running context). Then `resume(fiber)` is how the worker switches
//   to a cooperative job.

#pragma once

#include "core/Types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace psynder::jobs::detail {

using FiberFn = void (*)(void* user) noexcept;

// Default stack: 128 KiB. Lightmap bake / BVH refit stack usage is
// dominated by per-call locals; 128 KiB is the same default Windows uses
// for fiber threads and matches what `pthread_attr_setstacksize` permits
// on every supported host without page-allocator weirdness.
inline constexpr std::size_t kFiberDefaultStack = 128u * 1024u;

class Fiber {
  public:
    // Create a fiber that will execute `fn(user)` when first resumed.
    // `stack_size` is rounded up to the page size by the platform. Returns
    // nullptr on alloc failure.
    static Fiber* create(std::size_t stack_size, FiberFn fn, void* user) noexcept;

    // Destroy a fiber. The fiber MUST be in the "done" state (either
    // returned from its entry point or never resumed). Destroying a
    // currently-running or yielded fiber is undefined.
    static void destroy(Fiber* f) noexcept;

    // Convert (or re-convert) the calling thread to a fiber so that it
    // can switch into Fibers. On Windows this calls ConvertThreadToFiber
    // when the thread isn't already a fiber. On POSIX this is a no-op.
    // Idempotent.
    static void make_thread_a_fiber_host() noexcept;

    // If the calling thread was converted by `make_thread_a_fiber_host`,
    // undo the conversion. On Windows: `ConvertFiberToThread`. POSIX: no-op.
    static void unmake_thread_a_fiber_host() noexcept;

    // Switch the current OS thread *into* this fiber. The host context
    // (where resume() was called) is captured so the fiber can yield back
    // to it. Blocks until the fiber yields or completes.
    void resume() noexcept;

    // Switch *out of* the currently-running fiber, back to the host that
    // called resume(). Must be called from inside the fiber. Returns when
    // the fiber is next resumed.
    static void yield() noexcept;

    // True once the fiber's entry function has returned.
    bool done() const noexcept {
        return done_.load(std::memory_order_acquire) != 0u;
    }

    // The fiber currently running on this thread, or nullptr.
    static Fiber* current() noexcept;

    // Static entry point installed by makecontext / CreateFiberEx — pulls
    // the Fiber pointer out of the install slot and runs fn. Public so the
    // C trampoline (extern "C" on POSIX, CALLBACK on Win32) can invoke it,
    // but treat it as private implementation detail.
    static void platform_entry() noexcept;

  private:
    Fiber() = default;
    ~Fiber() = default;
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    // Platform context handle. On Windows this is a LPVOID; on POSIX it's
    // a pointer to a heap-allocated ucontext_t plus its private stack.
    void* native_  = nullptr;
    void* stack_   = nullptr;
    std::size_t stack_size_ = 0;
    FiberFn fn_    = nullptr;
    void*   user_  = nullptr;
    std::atomic<u32> done_{0};

    // The thread that resumed us; used so yield() knows where to go back.
    // For POSIX this points to the host ucontext_t. For Windows it's the
    // host fiber handle. Set on each resume(); read by yield().
    void*   host_   = nullptr;
};

}  // namespace psynder::jobs::detail
