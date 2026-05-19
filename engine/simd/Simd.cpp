// SPDX-License-Identifier: MIT
// Psynder — SIMD impl. Lane 03 fleshes this out; Phase-0 stub provides the
// fallback scalar path so links succeed everywhere.

#include "Simd.h"

namespace psynder::simd {

// Minimal scalar fallback for f32x4 ops so dependents compile.
// Lane 03 will replace these with platform-native intrinsic implementations
// and add f32x8 / i32x4 / i32x8 + mask + comparison ops.

namespace {
inline const f32* as_f32(const f32x4& v) {
    return reinterpret_cast<const f32*>(&v);
}
inline f32* as_f32(f32x4& v) {
    return reinterpret_cast<f32*>(&v);
}
}  // namespace

f32x4 add(f32x4 a, f32x4 b) noexcept {
    f32x4 r{};
    for (int i = 0; i < 4; ++i) as_f32(r)[i] = as_f32(a)[i] + as_f32(b)[i];
    return r;
}
f32x4 sub(f32x4 a, f32x4 b) noexcept {
    f32x4 r{};
    for (int i = 0; i < 4; ++i) as_f32(r)[i] = as_f32(a)[i] - as_f32(b)[i];
    return r;
}
f32x4 mul(f32x4 a, f32x4 b) noexcept {
    f32x4 r{};
    for (int i = 0; i < 4; ++i) as_f32(r)[i] = as_f32(a)[i] * as_f32(b)[i];
    return r;
}
f32x4 div(f32x4 a, f32x4 b) noexcept {
    f32x4 r{};
    for (int i = 0; i < 4; ++i) as_f32(r)[i] = as_f32(a)[i] / as_f32(b)[i];
    return r;
}
f32x4 fma(f32x4 a, f32x4 b, f32x4 c) noexcept {
    f32x4 r{};
    for (int i = 0; i < 4; ++i) as_f32(r)[i] = as_f32(a)[i] * as_f32(b)[i] + as_f32(c)[i];
    return r;
}
f32x4 load(const f32* p) noexcept {
    f32x4 r{};
    for (int i = 0; i < 4; ++i) as_f32(r)[i] = p[i];
    return r;
}
void store(f32* p, f32x4 v) noexcept {
    for (int i = 0; i < 4; ++i) p[i] = as_f32(v)[i];
}
f32x4 broadcast(f32 s) noexcept {
    f32x4 r{};
    for (int i = 0; i < 4; ++i) as_f32(r)[i] = s;
    return r;
}

}  // namespace psynder::simd
