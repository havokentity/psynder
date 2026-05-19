// SPDX-License-Identifier: MIT
// Psynder — Wave B: Fiber primitive impl. Lane 04 owned.
//
// See Fiber_internal.h for the API contract. Two backends:
//
//   - Windows: Win32 fibers (CreateFiberEx + SwitchToFiber).
//   - POSIX:   ucontext_t (getcontext + makecontext + swapcontext). The
//     ucontext API is deprecated on macOS but still ships and is the only
//     portable user-space context switch primitive we can rely on without
//     pulling in an arch-specific .S file. We silence the deprecation
//     warning at TU scope.

#include "Fiber_internal.h"

#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  // macOS guards ucontext.h behind _XOPEN_SOURCE because the routines are
  // formally deprecated; we still want them as the only portable user-
  // space context-switch primitive. Define before include. The deprecation
  // diagnostic is suppressed at the call sites (not here) because clang
  // emits the warning when the function is *referenced*, not when the
  // header is read.
  #if defined(__APPLE__)
    #ifndef _XOPEN_SOURCE
      #define _XOPEN_SOURCE 600
    #endif
  #endif
  #include <ucontext.h>
  #if defined(__APPLE__)
    // Re-poison once we're past the header. From here on, warnings about
    // get/make/swapcontext at *call sites* are silenced one block at a
    // time via local pragma. We deliberately don't disable globally so a
    // ucontext usage outside the documented backend would still warn.
  #endif
#endif

namespace psynder::jobs::detail {

namespace {

// Per-thread "running fiber" pointer. yield() needs this to find the host
// context to switch back to; current() exposes it.
thread_local Fiber* t_current_fiber = nullptr;

#if defined(_WIN32)
// On Windows we additionally track whether *this* thread was converted by
// make_thread_a_fiber_host. ConvertFiberToThread is required to clean up
// only if we created the host fiber ourselves; if the host had already
// been a fiber on entry, we must not undo it.
thread_local bool t_we_converted_thread = false;
#endif

// Hand-off slot used at fiber-create time to pass the Fiber pointer through
// to platform_entry(). On Windows CreateFiberEx accepts a void* arg, which
// we wire directly. On POSIX makecontext does not portably accept a 64-bit
// argument (the int-pair workaround is the textbook trick), so we hand the
// pointer via a thread-local just before swapcontext.
#if !defined(_WIN32)
thread_local Fiber* t_entry_handoff = nullptr;
#endif

}  // namespace

// ── Platform entry ──────────────────────────────────────────────────────
//
// On both backends, this is what executes inside the fiber stack when it
// is first resumed. Pull the Fiber* out of the install slot, run its fn,
// mark done, and yield one final time. yield() never returns from here.

void Fiber::platform_entry() noexcept {
#if defined(_WIN32)
    // Windows passes our user arg into the FIBER entry directly.
    Fiber* f = t_current_fiber;
#else
    Fiber* f = t_entry_handoff;
    t_entry_handoff = nullptr;
    t_current_fiber = f;
#endif

    if (f && f->fn_) {
        f->fn_(f->user_);
    }
    if (f) {
        f->done_.store(1u, std::memory_order_release);
    }
    // Final yield back to the host. The Fiber object is now "done" — the
    // host will see done()==true and is responsible for not resuming us.
    Fiber::yield();
    // If the host mistakenly resumes a done fiber, we land back here. To
    // avoid undefined behaviour (the Win32 fiber would otherwise unwind off
    // its stack and ExitThread), we keep yielding forever. The done flag
    // means callers should treat a finished fiber as terminal anyway.
    for (;;) {
        Fiber::yield();
    }
}

#if defined(_WIN32)

// ── Win32 backend ───────────────────────────────────────────────────────

namespace {
// CreateFiberEx callback signature: void CALLBACK fn(LPVOID). We trampoline
// into platform_entry which knows nothing about Win32 types.
VOID CALLBACK win_fiber_entry(LPVOID lp) {
    // Install the Fiber* into the current thread-local before the entry
    // pulls it out. The lp parameter IS the Fiber*; SwitchToFiber preserves
    // it across switches as the FiberData of the running fiber.
    t_current_fiber = static_cast<Fiber*>(lp);
    Fiber::platform_entry();
}
}  // namespace

Fiber* Fiber::create(std::size_t stack_size, FiberFn fn, void* user) noexcept {
    if (stack_size == 0) stack_size = kFiberDefaultStack;
    auto* f = new (std::nothrow) Fiber();
    if (!f) return nullptr;
    f->fn_         = fn;
    f->user_       = user;
    f->stack_size_ = stack_size;
    // CreateFiberEx: commit size 0 lets the OS commit on touch. Reserve =
    // stack_size. The FIBER_FLAG_FLOAT_SWITCH flag preserves FPU state.
    f->native_ = CreateFiberEx(
        /*dwStackCommitSize*/ 0,
        /*dwStackReserveSize*/ stack_size,
        /*dwFlags*/          FIBER_FLAG_FLOAT_SWITCH,
        /*lpStartAddress*/   &win_fiber_entry,
        /*lpParameter*/      f);
    if (!f->native_) {
        delete f;
        return nullptr;
    }
    return f;
}

void Fiber::destroy(Fiber* f) noexcept {
    if (!f) return;
    if (f->native_) {
        DeleteFiber(f->native_);
        f->native_ = nullptr;
    }
    delete f;
}

void Fiber::make_thread_a_fiber_host() noexcept {
    // IsThreadAFiber tells us whether this thread already has a fiber
    // context. If not, convert; mark so we know to convert back.
    if (!IsThreadAFiber()) {
        // FLOAT_SWITCH so FPU state crosses the switch on x86/x64.
        LPVOID host = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        (void)host;
        t_we_converted_thread = true;
    }
}

void Fiber::unmake_thread_a_fiber_host() noexcept {
    if (t_we_converted_thread) {
        ConvertFiberToThread();
        t_we_converted_thread = false;
    }
}

void Fiber::resume() noexcept {
    // The "host" is whoever called resume() — capture their fiber handle so
    // yield() can switch back. GetCurrentFiber returns the current fiber's
    // handle (which is the host fiber if the caller is a worker thread
    // that was converted via make_thread_a_fiber_host).
    host_ = GetCurrentFiber();
    Fiber* prev = t_current_fiber;
    t_current_fiber = this;
    SwitchToFiber(native_);
    // Back on the host stack.
    t_current_fiber = prev;
}

void Fiber::yield() noexcept {
    Fiber* f = t_current_fiber;
    if (!f || !f->host_) return;  // not inside a fiber
    SwitchToFiber(f->host_);
}

Fiber* Fiber::current() noexcept {
    return t_current_fiber;
}

#else  // !_WIN32

// ── POSIX (ucontext) backend ────────────────────────────────────────────
//
// Apple deprecated the ucontext family in 10.6 but never removed it. We
// silence the deprecation diagnostic locally around the call sites; the
// alternative (per-arch context switch in assembly) is much larger.
#if defined(__APPLE__) && defined(__clang__)
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace {

// Apple's ucontext_t stores `mcontext_t` as a POINTER (uc_mcontext) plus
// `uc_mcsize`. The caller must allocate the mcontext_t storage AND wire
// the pointer + size in BEFORE calling getcontext, or the kernel writes
// to a junk pointer. glibc's struct embeds the mcontext inline so this
// step is a no-op there. We always pair them to stay portable.
struct PosixCtx {
    ucontext_t uc;
#if defined(__APPLE__)
    // Holds the actual register snapshot; `uc.uc_mcontext` points here.
    _STRUCT_MCONTEXT mc;
#endif
};

// ucontext_t + stack live in a single heap block per fiber. The Fiber's
// `native_` points at the PosixCtx; `stack_` at the raw stack region.
//
// The trampoline pulls the Fiber pointer out of t_entry_handoff (set just
// before swapcontext). makecontext can only pass `int` arguments portably,
// so the install-slot dance is the standard way to hand a pointer in.
extern "C" void posix_fiber_entry() {
    Fiber::platform_entry();
}

}  // namespace

Fiber* Fiber::create(std::size_t stack_size, FiberFn fn, void* user) noexcept {
    if (stack_size < static_cast<std::size_t>(SIGSTKSZ)) {
        stack_size = static_cast<std::size_t>(SIGSTKSZ) * 2u;
    }
    if (stack_size < kFiberDefaultStack) {
        stack_size = kFiberDefaultStack;
    }
    auto* f = new (std::nothrow) Fiber();
    if (!f) return nullptr;
    f->fn_         = fn;
    f->user_       = user;
    f->stack_size_ = stack_size;

    auto* pctx = new (std::nothrow) PosixCtx();
    if (!pctx) { delete f; return nullptr; }
    std::memset(pctx, 0, sizeof(*pctx));
#if defined(__APPLE__)
    pctx->uc.uc_mcontext = &pctx->mc;
    pctx->uc.uc_mcsize   = sizeof(pctx->mc);
#endif

    void* stack = std::malloc(stack_size);
    if (!stack) {
        delete pctx;
        delete f;
        return nullptr;
    }

    if (getcontext(&pctx->uc) != 0) {
        std::free(stack);
        delete pctx;
        delete f;
        return nullptr;
    }
    pctx->uc.uc_stack.ss_sp   = stack;
    pctx->uc.uc_stack.ss_size = stack_size;
    pctx->uc.uc_link          = nullptr;  // entry must yield; never falls off
#if defined(__APPLE__)
    // getcontext may have re-zeroed uc_mcontext on some kernels — re-wire.
    pctx->uc.uc_mcontext = &pctx->mc;
    pctx->uc.uc_mcsize   = sizeof(pctx->mc);
#endif
    // 0-arg makecontext: portable across the BSDs and Linux glibc.
    makecontext(&pctx->uc, &posix_fiber_entry, 0);

    f->native_ = pctx;
    f->stack_  = stack;
    return f;
}

void Fiber::destroy(Fiber* f) noexcept {
    if (!f) return;
    if (f->native_) {
        delete static_cast<PosixCtx*>(f->native_);
        f->native_ = nullptr;
    }
    if (f->stack_) {
        std::free(f->stack_);
        f->stack_ = nullptr;
    }
    delete f;
}

void Fiber::make_thread_a_fiber_host() noexcept {
    // No-op on POSIX: ucontext just captures the current stack at the
    // swap point, no conversion required.
}

void Fiber::unmake_thread_a_fiber_host() noexcept {
    // No-op on POSIX.
}

void Fiber::resume() noexcept {
    // Allocate the host PosixCtx on this thread's stack — getcontext
    // captures the current execution point, swapcontext restores it when
    // the fiber yields. We hand a pointer to this local via host_.
    // Apple needs the mcontext pointer wired BEFORE swapcontext.
    PosixCtx host;
    std::memset(&host, 0, sizeof(host));
#if defined(__APPLE__)
    host.uc.uc_mcontext = &host.mc;
    host.uc.uc_mcsize   = sizeof(host.mc);
#endif
    host_ = &host;
    t_entry_handoff = this;
    Fiber* prev = t_current_fiber;
    t_current_fiber = this;
    swapcontext(&host.uc,
                &static_cast<PosixCtx*>(native_)->uc);
    // Returned: fiber yielded back to us. Restore previous current and
    // forget the host pointer (the local is now dead).
    t_current_fiber = prev;
    host_ = nullptr;
}

void Fiber::yield() noexcept {
    Fiber* f = t_current_fiber;
    if (!f || !f->host_) return;
    // Save fiber state into native_; restore host state from host_.
    swapcontext(&static_cast<PosixCtx*>(f->native_)->uc,
                &static_cast<PosixCtx*>(f->host_)->uc);
}

Fiber* Fiber::current() noexcept {
    return t_current_fiber;
}

#if defined(__APPLE__) && defined(__clang__)
  #pragma clang diagnostic pop
#endif

#endif  // _WIN32

}  // namespace psynder::jobs::detail
