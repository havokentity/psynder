// SPDX-License-Identifier: MIT
// Psynder — SIMD public surface impl. The frozen public header (Simd.h)
// declares the canonical 7-function pack-arithmetic API; this TU defines
// those functions in terms of the full intrinsic kernel set living in
// Simd_internal.h.
//
// Anything beyond the frozen surface (mask types, comparisons, blend,
// min/max/abs/rsqrt/sqrt, gather, reductions, AVX-512 f32x16, runtime
// dispatch) is in Simd_internal.h / Dispatch.{h,cpp} and is consumed by
// lane code via `#include "simd/Simd_internal.h"`.
//
// We define the public functions out-of-line (rather than `inline` in the
// header) because the header is FROZEN — changing it requires Wave-0
// re-coordination — and because the call sites that go through the .a
// boundary are the rare, non-hot ones; the hot loops include the internal
// header directly and inline everything.

#include "Simd.h"
#include "Simd_internal.h"

namespace psynder::simd {

f32x4 add(f32x4 a, f32x4 b) noexcept {
    return add4(a, b);
}
f32x4 sub(f32x4 a, f32x4 b) noexcept {
    return sub4(a, b);
}
f32x4 mul(f32x4 a, f32x4 b) noexcept {
    return mul4(a, b);
}
f32x4 div(f32x4 a, f32x4 b) noexcept {
    return div4(a, b);
}
f32x4 fma(f32x4 a, f32x4 b, f32x4 c) noexcept {
    return fma4(a, b, c);
}
f32x4 load(const f32* p) noexcept {
    return load_unaligned4(p);
}
void store(f32* p, f32x4 v) noexcept {
    store_unaligned4(p, v);
}
f32x4 broadcast(f32 s) noexcept {
    return broadcast4(s);
}

}  // namespace psynder::simd
