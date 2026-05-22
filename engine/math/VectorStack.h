// SPDX-License-Identifier: MIT
// Psynder — VectorStack frame math batching.
//
// VectorStack turns many tiny gameplay/render math calls into a few large
// cache-linear jobs. Callers submit SoA spans directly when they already own
// packed component streams, or submit legacy Vec3 arrays while the engine
// migrates systems over. `flush()` executes the queued jobs in FIFO order.

#pragma once

#include "Math.h"

#include "core/Types.h"

#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace psynder::math {

namespace detail {

template <class T, usize Alignment = kCacheLine>
class VectorStackAllocator {
   public:
    using value_type = T;
    using is_always_equal = std::true_type;

    static_assert(Alignment >= alignof(T), "allocator alignment must satisfy T");

    VectorStackAllocator() noexcept = default;

    template <class U>
    constexpr VectorStackAllocator(const VectorStackAllocator<U, Alignment>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_array_new_length{};
        if (count == 0)
            return nullptr;
        return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{Alignment}));
    }

    void deallocate(T* ptr, std::size_t) noexcept {
        ::operator delete(ptr, std::align_val_t{Alignment});
    }

    template <class U>
    struct rebind {
        using other = VectorStackAllocator<U, Alignment>;
    };
};

template <class A, class B, usize Alignment>
constexpr bool operator==(const VectorStackAllocator<A, Alignment>&,
                          const VectorStackAllocator<B, Alignment>&) noexcept {
    return true;
}

template <class A, class B, usize Alignment>
constexpr bool operator!=(const VectorStackAllocator<A, Alignment>&,
                          const VectorStackAllocator<B, Alignment>&) noexcept {
    return false;
}

template <class T>
using VectorStackVector = std::vector<T, VectorStackAllocator<T>>;

}  // namespace detail

struct Vec3SoaView {
    const f32* x = nullptr;
    const f32* y = nullptr;
    const f32* z = nullptr;
    usize count = 0;
};

struct MutableVec3SoaView {
    f32* x = nullptr;
    f32* y = nullptr;
    f32* z = nullptr;
    usize count = 0;
};

class Vec3SoaBuffer {
   public:
    Vec3SoaBuffer() = default;
    explicit Vec3SoaBuffer(usize count) { resize(count); }

    void reserve(usize count) {
        x_.reserve(count);
        y_.reserve(count);
        z_.reserve(count);
    }

    void resize(usize count) {
        x_.resize(count);
        y_.resize(count);
        z_.resize(count);
    }

    [[nodiscard]] usize count() const noexcept { return x_.size(); }
    [[nodiscard]] bool empty() const noexcept { return x_.empty(); }

    [[nodiscard]] f32* x_data() noexcept { return x_.data(); }
    [[nodiscard]] f32* y_data() noexcept { return y_.data(); }
    [[nodiscard]] f32* z_data() noexcept { return z_.data(); }
    [[nodiscard]] const f32* x_data() const noexcept { return x_.data(); }
    [[nodiscard]] const f32* y_data() const noexcept { return y_.data(); }
    [[nodiscard]] const f32* z_data() const noexcept { return z_.data(); }

    [[nodiscard]] Vec3SoaView view() const noexcept {
        return Vec3SoaView{x_.data(), y_.data(), z_.data(), x_.size()};
    }
    [[nodiscard]] MutableVec3SoaView mutable_view() noexcept {
        return MutableVec3SoaView{x_.data(), y_.data(), z_.data(), x_.size()};
    }

   private:
    detail::VectorStackVector<f32> x_;
    detail::VectorStackVector<f32> y_;
    detail::VectorStackVector<f32> z_;
};

struct VectorStackStats {
    u32 ops_submitted = 0;
    u32 ops_flushed = 0;
    usize elements_flushed = 0;
    usize soa_elements = 0;
    usize aos_elements = 0;
    usize scratch_capacity = 0;
};

class VectorStack {
   public:
    explicit VectorStack(usize scratch_capacity = 4096);

    void reserve_ops(usize count);
    void reserve_scratch(usize elements);
    void clear() noexcept;

    [[nodiscard]] const VectorStackStats& stats() const noexcept { return stats_; }

    // Native path: callers provide SoA streams that are already packed and
    // cache-linear. This is the preferred DOTS/ECS data path.
    void transform_points(const Mat4& m, Vec3SoaView in, MutableVec3SoaView out);
    void transform_dirs(const Mat4& m, Vec3SoaView in, MutableVec3SoaView out);
    void integrate_positions(MutableVec3SoaView positions, Vec3SoaView velocities, f32 dt);

    // Migration path: queues legacy AoS Vec3 spans and lets VectorStack
    // gather/scatter through scratch SoA chunks during flush.
    void transform_points(const Mat4& m, const Vec3* in, Vec3* out, usize count);
    void transform_dirs(const Mat4& m, const Vec3* in, Vec3* out, usize count);
    void integrate_positions(Vec3* positions, const Vec3* velocities, usize count, f32 dt);

    void flush() noexcept;

   private:
    enum class OpKind : u8 {
        PointsSoa,
        DirsSoa,
        PointsAos,
        DirsAos,
        IntegrateSoa,
        IntegrateAos,
    };

    struct Op {
        OpKind kind{};
        Mat4 matrix{};
        Vec3SoaView in_soa{};
        MutableVec3SoaView out_soa{};
        const Vec3* in_aos = nullptr;
        const Vec3* aux_aos = nullptr;
        Vec3* out_aos = nullptr;
        usize count = 0;
        f32 scalar = 0.0f;
    };

    void push(Op op);
    void flush_soa(const Op& op) noexcept;
    void flush_aos(const Op& op) noexcept;
    void flush_integrate_soa(const Op& op) noexcept;
    void flush_integrate_aos(const Op& op) noexcept;
    void ensure_scratch(usize elements);

    detail::VectorStackVector<Op> ops_;
    detail::VectorStackVector<f32> scratch_x_;
    detail::VectorStackVector<f32> scratch_y_;
    detail::VectorStackVector<f32> scratch_z_;
    detail::VectorStackVector<f32> scratch_out_x_;
    detail::VectorStackVector<f32> scratch_out_y_;
    detail::VectorStackVector<f32> scratch_out_z_;
    VectorStackStats stats_{};
};

const char* vector_stack_backend() noexcept;

}  // namespace psynder::math
