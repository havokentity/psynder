// SPDX-License-Identifier: MIT
// Psynder jobs — portable cache-line-aligned allocator shim.
//
// MSVC's STL doesn't expose `std::aligned_alloc` (C++17), so we route
// through `_aligned_malloc` / `_aligned_free` on Windows and through
// the POSIX `std::aligned_alloc` + `std::free` pair everywhere else.
// Note: MSVC's _aligned_malloc REQUIRES _aligned_free — calling free()
// on _aligned_malloc-allocated memory is undefined behavior — so both
// directions have to flow through these helpers.
//
// `bytes` is rounded up to a multiple of `align` per C11's aligned_alloc
// contract (POSIX glibc enforces this; we do it explicitly so MSVC and
// macOS behave the same way regardless of libc strictness).

#pragma once

#include <cstddef>
#include <cstdlib>

#if defined(_WIN32)
#include <malloc.h>  // _aligned_malloc / _aligned_free
#endif

namespace psynder::jobs::detail {

inline void* aligned_xalloc(std::size_t align, std::size_t bytes) noexcept {
    // Round bytes up to a multiple of align (POSIX aligned_alloc contract).
    const std::size_t padded = ((bytes + align - 1) / align) * align;
#if defined(_WIN32)
    return _aligned_malloc(padded, align);
#else
    return std::aligned_alloc(align, padded);
#endif
}

inline void aligned_xfree(void* p) noexcept {
    if (!p)
        return;
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

}  // namespace psynder::jobs::detail
