// SPDX-License-Identifier: MIT
// Psynder — VectorStack frame math batching implementation.

#include "VectorStack.h"

#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace psynder::math {

#if defined(_MSC_VER)
#define PSY_VECTORSTACK_RESTRICT __restrict
#elif defined(__clang__) || defined(__GNUC__)
#define PSY_VECTORSTACK_RESTRICT __restrict__
#else
#define PSY_VECTORSTACK_RESTRICT
#endif

namespace {

struct LinearRows {
    f32 m00, m01, m02;
    f32 m10, m11, m12;
    f32 m20, m21, m22;
};

LinearRows linear_rows(const Mat4& m) noexcept {
    return LinearRows{
        m.m[0],
        m.m[4],
        m.m[8],
        m.m[1],
        m.m[5],
        m.m[9],
        m.m[2],
        m.m[6],
        m.m[10],
    };
}

void transform_soa(const Mat4& m, Vec3SoaView in, MutableVec3SoaView out, bool points, usize count) noexcept {
    const LinearRows r = linear_rows(m);
    const f32 tx = points ? m.m[12] : 0.0f;
    const f32 ty = points ? m.m[13] : 0.0f;
    const f32 tz = points ? m.m[14] : 0.0f;

    const f32* PSY_VECTORSTACK_RESTRICT ix = in.x;
    const f32* PSY_VECTORSTACK_RESTRICT iy = in.y;
    const f32* PSY_VECTORSTACK_RESTRICT iz = in.z;
    f32* PSY_VECTORSTACK_RESTRICT ox = out.x;
    f32* PSY_VECTORSTACK_RESTRICT oy = out.y;
    f32* PSY_VECTORSTACK_RESTRICT oz = out.z;

    usize i = 0;
#if defined(__AVX512F__)
    const __m512 m00 = _mm512_set1_ps(r.m00);
    const __m512 m01 = _mm512_set1_ps(r.m01);
    const __m512 m02 = _mm512_set1_ps(r.m02);
    const __m512 m10 = _mm512_set1_ps(r.m10);
    const __m512 m11 = _mm512_set1_ps(r.m11);
    const __m512 m12 = _mm512_set1_ps(r.m12);
    const __m512 m20 = _mm512_set1_ps(r.m20);
    const __m512 m21 = _mm512_set1_ps(r.m21);
    const __m512 m22 = _mm512_set1_ps(r.m22);
    const __m512 t0 = _mm512_set1_ps(tx);
    const __m512 t1 = _mm512_set1_ps(ty);
    const __m512 t2 = _mm512_set1_ps(tz);
    for (; i + 16 <= count; i += 16) {
        const __m512 x = _mm512_loadu_ps(ix + i);
        const __m512 y = _mm512_loadu_ps(iy + i);
        const __m512 z = _mm512_loadu_ps(iz + i);
        __m512 rx = _mm512_add_ps(_mm512_mul_ps(x, m00), _mm512_mul_ps(y, m01));
        rx = _mm512_add_ps(_mm512_add_ps(rx, _mm512_mul_ps(z, m02)), t0);
        __m512 ry = _mm512_add_ps(_mm512_mul_ps(x, m10), _mm512_mul_ps(y, m11));
        ry = _mm512_add_ps(_mm512_add_ps(ry, _mm512_mul_ps(z, m12)), t1);
        __m512 rz = _mm512_add_ps(_mm512_mul_ps(x, m20), _mm512_mul_ps(y, m21));
        rz = _mm512_add_ps(_mm512_add_ps(rz, _mm512_mul_ps(z, m22)), t2);
        _mm512_storeu_ps(ox + i, rx);
        _mm512_storeu_ps(oy + i, ry);
        _mm512_storeu_ps(oz + i, rz);
    }
#elif defined(__AVX2__)
    const __m256 m00 = _mm256_set1_ps(r.m00);
    const __m256 m01 = _mm256_set1_ps(r.m01);
    const __m256 m02 = _mm256_set1_ps(r.m02);
    const __m256 m10 = _mm256_set1_ps(r.m10);
    const __m256 m11 = _mm256_set1_ps(r.m11);
    const __m256 m12 = _mm256_set1_ps(r.m12);
    const __m256 m20 = _mm256_set1_ps(r.m20);
    const __m256 m21 = _mm256_set1_ps(r.m21);
    const __m256 m22 = _mm256_set1_ps(r.m22);
    const __m256 t0 = _mm256_set1_ps(tx);
    const __m256 t1 = _mm256_set1_ps(ty);
    const __m256 t2 = _mm256_set1_ps(tz);
    for (; i + 8 <= count; i += 8) {
        const __m256 x = _mm256_loadu_ps(ix + i);
        const __m256 y = _mm256_loadu_ps(iy + i);
        const __m256 z = _mm256_loadu_ps(iz + i);
        __m256 rx = _mm256_add_ps(_mm256_mul_ps(x, m00), _mm256_mul_ps(y, m01));
        rx = _mm256_add_ps(_mm256_add_ps(rx, _mm256_mul_ps(z, m02)), t0);
        __m256 ry = _mm256_add_ps(_mm256_mul_ps(x, m10), _mm256_mul_ps(y, m11));
        ry = _mm256_add_ps(_mm256_add_ps(ry, _mm256_mul_ps(z, m12)), t1);
        __m256 rz = _mm256_add_ps(_mm256_mul_ps(x, m20), _mm256_mul_ps(y, m21));
        rz = _mm256_add_ps(_mm256_add_ps(rz, _mm256_mul_ps(z, m22)), t2);
        _mm256_storeu_ps(ox + i, rx);
        _mm256_storeu_ps(oy + i, ry);
        _mm256_storeu_ps(oz + i, rz);
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    const float32x4_t m00 = vdupq_n_f32(r.m00);
    const float32x4_t m01 = vdupq_n_f32(r.m01);
    const float32x4_t m02 = vdupq_n_f32(r.m02);
    const float32x4_t m10 = vdupq_n_f32(r.m10);
    const float32x4_t m11 = vdupq_n_f32(r.m11);
    const float32x4_t m12 = vdupq_n_f32(r.m12);
    const float32x4_t m20 = vdupq_n_f32(r.m20);
    const float32x4_t m21 = vdupq_n_f32(r.m21);
    const float32x4_t m22 = vdupq_n_f32(r.m22);
    const float32x4_t t0 = vdupq_n_f32(tx);
    const float32x4_t t1 = vdupq_n_f32(ty);
    const float32x4_t t2 = vdupq_n_f32(tz);
    for (; i + 4 <= count; i += 4) {
        const float32x4_t x = vld1q_f32(ix + i);
        const float32x4_t y = vld1q_f32(iy + i);
        const float32x4_t z = vld1q_f32(iz + i);
        float32x4_t rx = vfmaq_f32(vmulq_f32(x, m00), y, m01);
        rx = vaddq_f32(vfmaq_f32(rx, z, m02), t0);
        float32x4_t ry = vfmaq_f32(vmulq_f32(x, m10), y, m11);
        ry = vaddq_f32(vfmaq_f32(ry, z, m12), t1);
        float32x4_t rz = vfmaq_f32(vmulq_f32(x, m20), y, m21);
        rz = vaddq_f32(vfmaq_f32(rz, z, m22), t2);
        vst1q_f32(ox + i, rx);
        vst1q_f32(oy + i, ry);
        vst1q_f32(oz + i, rz);
    }
#elif defined(__x86_64__) || defined(_M_X64)
    const __m128 m00 = _mm_set1_ps(r.m00);
    const __m128 m01 = _mm_set1_ps(r.m01);
    const __m128 m02 = _mm_set1_ps(r.m02);
    const __m128 m10 = _mm_set1_ps(r.m10);
    const __m128 m11 = _mm_set1_ps(r.m11);
    const __m128 m12 = _mm_set1_ps(r.m12);
    const __m128 m20 = _mm_set1_ps(r.m20);
    const __m128 m21 = _mm_set1_ps(r.m21);
    const __m128 m22 = _mm_set1_ps(r.m22);
    const __m128 t0 = _mm_set1_ps(tx);
    const __m128 t1 = _mm_set1_ps(ty);
    const __m128 t2 = _mm_set1_ps(tz);
    for (; i + 4 <= count; i += 4) {
        const __m128 x = _mm_loadu_ps(ix + i);
        const __m128 y = _mm_loadu_ps(iy + i);
        const __m128 z = _mm_loadu_ps(iz + i);
        __m128 rx = _mm_add_ps(_mm_mul_ps(x, m00), _mm_mul_ps(y, m01));
        rx = _mm_add_ps(_mm_add_ps(rx, _mm_mul_ps(z, m02)), t0);
        __m128 ry = _mm_add_ps(_mm_mul_ps(x, m10), _mm_mul_ps(y, m11));
        ry = _mm_add_ps(_mm_add_ps(ry, _mm_mul_ps(z, m12)), t1);
        __m128 rz = _mm_add_ps(_mm_mul_ps(x, m20), _mm_mul_ps(y, m21));
        rz = _mm_add_ps(_mm_add_ps(rz, _mm_mul_ps(z, m22)), t2);
        _mm_storeu_ps(ox + i, rx);
        _mm_storeu_ps(oy + i, ry);
        _mm_storeu_ps(oz + i, rz);
    }
#endif

    for (; i < count; ++i) {
        const f32 x = ix[i];
        const f32 y = iy[i];
        const f32 z = iz[i];
        ox[i] = r.m00 * x + r.m01 * y + r.m02 * z + tx;
        oy[i] = r.m10 * x + r.m11 * y + r.m12 * z + ty;
        oz[i] = r.m20 * x + r.m21 * y + r.m22 * z + tz;
    }
}

usize valid_count(Vec3SoaView in, MutableVec3SoaView out) noexcept {
    if (in.x == nullptr || in.y == nullptr || in.z == nullptr || out.x == nullptr ||
        out.y == nullptr || out.z == nullptr) {
        return 0;
    }
    return std::min(in.count, out.count);
}

void integrate_soa(MutableVec3SoaView positions, Vec3SoaView velocities, f32 dt, usize count) noexcept {
    f32* PSY_VECTORSTACK_RESTRICT px = positions.x;
    f32* PSY_VECTORSTACK_RESTRICT py = positions.y;
    f32* PSY_VECTORSTACK_RESTRICT pz = positions.z;
    const f32* PSY_VECTORSTACK_RESTRICT vx = velocities.x;
    const f32* PSY_VECTORSTACK_RESTRICT vy = velocities.y;
    const f32* PSY_VECTORSTACK_RESTRICT vz = velocities.z;

    usize i = 0;
#if defined(__AVX512F__)
    const __m512 step = _mm512_set1_ps(dt);
    for (; i + 16 <= count; i += 16) {
        _mm512_storeu_ps(px + i,
                         _mm512_add_ps(_mm512_loadu_ps(px + i),
                                       _mm512_mul_ps(_mm512_loadu_ps(vx + i), step)));
        _mm512_storeu_ps(py + i,
                         _mm512_add_ps(_mm512_loadu_ps(py + i),
                                       _mm512_mul_ps(_mm512_loadu_ps(vy + i), step)));
        _mm512_storeu_ps(pz + i,
                         _mm512_add_ps(_mm512_loadu_ps(pz + i),
                                       _mm512_mul_ps(_mm512_loadu_ps(vz + i), step)));
    }
#elif defined(__AVX2__)
    const __m256 step = _mm256_set1_ps(dt);
    for (; i + 8 <= count; i += 8) {
        _mm256_storeu_ps(px + i,
                         _mm256_add_ps(_mm256_loadu_ps(px + i),
                                       _mm256_mul_ps(_mm256_loadu_ps(vx + i), step)));
        _mm256_storeu_ps(py + i,
                         _mm256_add_ps(_mm256_loadu_ps(py + i),
                                       _mm256_mul_ps(_mm256_loadu_ps(vy + i), step)));
        _mm256_storeu_ps(pz + i,
                         _mm256_add_ps(_mm256_loadu_ps(pz + i),
                                       _mm256_mul_ps(_mm256_loadu_ps(vz + i), step)));
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    const float32x4_t step = vdupq_n_f32(dt);
    for (; i + 4 <= count; i += 4) {
        vst1q_f32(px + i, vfmaq_f32(vld1q_f32(px + i), vld1q_f32(vx + i), step));
        vst1q_f32(py + i, vfmaq_f32(vld1q_f32(py + i), vld1q_f32(vy + i), step));
        vst1q_f32(pz + i, vfmaq_f32(vld1q_f32(pz + i), vld1q_f32(vz + i), step));
    }
#elif defined(__x86_64__) || defined(_M_X64)
    const __m128 step = _mm_set1_ps(dt);
    for (; i + 4 <= count; i += 4) {
        _mm_storeu_ps(px + i, _mm_add_ps(_mm_loadu_ps(px + i), _mm_mul_ps(_mm_loadu_ps(vx + i), step)));
        _mm_storeu_ps(py + i, _mm_add_ps(_mm_loadu_ps(py + i), _mm_mul_ps(_mm_loadu_ps(vy + i), step)));
        _mm_storeu_ps(pz + i, _mm_add_ps(_mm_loadu_ps(pz + i), _mm_mul_ps(_mm_loadu_ps(vz + i), step)));
    }
#endif

    for (; i < count; ++i) {
        px[i] += vx[i] * dt;
        py[i] += vy[i] * dt;
        pz[i] += vz[i] * dt;
    }
}

usize valid_count(MutableVec3SoaView positions, Vec3SoaView velocities) noexcept {
    if (positions.x == nullptr || positions.y == nullptr || positions.z == nullptr ||
        velocities.x == nullptr || velocities.y == nullptr || velocities.z == nullptr) {
        return 0;
    }
    return std::min(positions.count, velocities.count);
}

}  // namespace

VectorStack::VectorStack(usize scratch_capacity) {
    reserve_scratch(scratch_capacity);
}

void VectorStack::reserve_ops(usize count) {
    ops_.reserve(count);
}

void VectorStack::reserve_scratch(usize elements) {
    ensure_scratch(elements);
}

void VectorStack::clear() noexcept {
    ops_.clear();
    stats_ = VectorStackStats{};
    stats_.scratch_capacity = scratch_x_.size();
}

void VectorStack::transform_points(const Mat4& m, Vec3SoaView in, MutableVec3SoaView out) {
    const usize count = valid_count(in, out);
    if (count == 0)
        return;
    in.count = count;
    out.count = count;
    Op op{};
    op.kind = OpKind::PointsSoa;
    op.matrix = m;
    op.in_soa = in;
    op.out_soa = out;
    op.count = count;
    push(op);
}

void VectorStack::transform_dirs(const Mat4& m, Vec3SoaView in, MutableVec3SoaView out) {
    const usize count = valid_count(in, out);
    if (count == 0)
        return;
    in.count = count;
    out.count = count;
    Op op{};
    op.kind = OpKind::DirsSoa;
    op.matrix = m;
    op.in_soa = in;
    op.out_soa = out;
    op.count = count;
    push(op);
}

void VectorStack::integrate_positions(MutableVec3SoaView positions, Vec3SoaView velocities, f32 dt) {
    const usize count = valid_count(positions, velocities);
    if (count == 0)
        return;
    positions.count = count;
    velocities.count = count;
    Op op{};
    op.kind = OpKind::IntegrateSoa;
    op.in_soa = velocities;
    op.out_soa = positions;
    op.count = count;
    op.scalar = dt;
    push(op);
}

void VectorStack::transform_points(const Mat4& m, const Vec3* in, Vec3* out, usize count) {
    if (in == nullptr || out == nullptr || count == 0)
        return;
    Op op{};
    op.kind = OpKind::PointsAos;
    op.matrix = m;
    op.in_aos = in;
    op.out_aos = out;
    op.count = count;
    push(op);
}

void VectorStack::transform_dirs(const Mat4& m, const Vec3* in, Vec3* out, usize count) {
    if (in == nullptr || out == nullptr || count == 0)
        return;
    Op op{};
    op.kind = OpKind::DirsAos;
    op.matrix = m;
    op.in_aos = in;
    op.out_aos = out;
    op.count = count;
    push(op);
}

void VectorStack::integrate_positions(Vec3* positions, const Vec3* velocities, usize count, f32 dt) {
    if (positions == nullptr || velocities == nullptr || count == 0)
        return;
    Op op{};
    op.kind = OpKind::IntegrateAos;
    op.aux_aos = velocities;
    op.out_aos = positions;
    op.count = count;
    op.scalar = dt;
    push(op);
}

void VectorStack::flush() noexcept {
    stats_.ops_flushed = 0;
    stats_.elements_flushed = 0;
    stats_.soa_elements = 0;
    stats_.aos_elements = 0;
    stats_.scratch_capacity = scratch_x_.size();

    for (const Op& op : ops_) {
        switch (op.kind) {
            case OpKind::PointsSoa:
            case OpKind::DirsSoa:
                flush_soa(op);
                break;
            case OpKind::IntegrateSoa:
                flush_integrate_soa(op);
                break;
            case OpKind::PointsAos:
            case OpKind::DirsAos:
                flush_aos(op);
                break;
            case OpKind::IntegrateAos:
                flush_integrate_aos(op);
                break;
        }
        ++stats_.ops_flushed;
        stats_.elements_flushed += op.count;
    }
    ops_.clear();
}

void VectorStack::push(Op op) {
    ops_.push_back(op);
    ++stats_.ops_submitted;
}

void VectorStack::flush_soa(const Op& op) noexcept {
    const bool points = op.kind == OpKind::PointsSoa;
    transform_soa(op.matrix, op.in_soa, op.out_soa, points, op.count);
    stats_.soa_elements += op.count;
}

void VectorStack::flush_aos(const Op& op) noexcept {
    const bool points = op.kind == OpKind::PointsAos;
    usize done = 0;
    while (done < op.count) {
        const usize batch = std::min(scratch_x_.size(), op.count - done);
        for (usize i = 0; i < batch; ++i) {
            const Vec3 v = op.in_aos[done + i];
            scratch_x_[i] = v.x;
            scratch_y_[i] = v.y;
            scratch_z_[i] = v.z;
        }

        const Vec3SoaView in{
            scratch_x_.data(),
            scratch_y_.data(),
            scratch_z_.data(),
            batch,
        };
        const MutableVec3SoaView out{
            scratch_out_x_.data(),
            scratch_out_y_.data(),
            scratch_out_z_.data(),
            batch,
        };
        transform_soa(op.matrix, in, out, points, batch);

        for (usize i = 0; i < batch; ++i) {
            op.out_aos[done + i] = Vec3{scratch_out_x_[i], scratch_out_y_[i], scratch_out_z_[i]};
        }
        done += batch;
    }
    stats_.aos_elements += op.count;
}

void VectorStack::flush_integrate_soa(const Op& op) noexcept {
    integrate_soa(op.out_soa, op.in_soa, op.scalar, op.count);
    stats_.soa_elements += op.count;
}

void VectorStack::flush_integrate_aos(const Op& op) noexcept {
    usize done = 0;
    while (done < op.count) {
        const usize batch = std::min(scratch_x_.size(), op.count - done);
        for (usize i = 0; i < batch; ++i) {
            const Vec3 p = op.out_aos[done + i];
            const Vec3 v = op.aux_aos[done + i];
            scratch_x_[i] = p.x;
            scratch_y_[i] = p.y;
            scratch_z_[i] = p.z;
            scratch_out_x_[i] = v.x;
            scratch_out_y_[i] = v.y;
            scratch_out_z_[i] = v.z;
        }

        const MutableVec3SoaView positions{
            scratch_x_.data(),
            scratch_y_.data(),
            scratch_z_.data(),
            batch,
        };
        const Vec3SoaView velocities{
            scratch_out_x_.data(),
            scratch_out_y_.data(),
            scratch_out_z_.data(),
            batch,
        };
        integrate_soa(positions, velocities, op.scalar, batch);

        for (usize i = 0; i < batch; ++i) {
            op.out_aos[done + i] = Vec3{scratch_x_[i], scratch_y_[i], scratch_z_[i]};
        }
        done += batch;
    }
    stats_.aos_elements += op.count;
}

void VectorStack::ensure_scratch(usize elements) {
    elements = std::max<usize>(elements, 16);
    scratch_x_.resize(elements);
    scratch_y_.resize(elements);
    scratch_z_.resize(elements);
    scratch_out_x_.resize(elements);
    scratch_out_y_.resize(elements);
    scratch_out_z_.resize(elements);
    stats_.scratch_capacity = elements;
}

const char* vector_stack_backend() noexcept {
#if defined(__AVX512F__)
    return "avx512";
#elif defined(__AVX2__)
    return "avx2";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "neon4";
#elif defined(__x86_64__) || defined(_M_X64)
    return "sse4";
#else
    return "scalar";
#endif
}

#undef PSY_VECTORSTACK_RESTRICT

}  // namespace psynder::math
