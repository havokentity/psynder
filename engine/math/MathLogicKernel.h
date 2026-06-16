// SPDX-License-Identifier: MIT
// Psynder — init-time math logic recording and SoA kernel execution.
//
// App/game code records ordinary vector math into MathLogicKernelBuilder once,
// then compile() emits a small reusable program. Runtime execution is chunked
// over SoA streams with kernel-owned scratch, so frames do not allocate.

#pragma once

#include "Math.h"
#include "VectorStack.h"

#include "core/Types.h"

#include <span>

namespace psynder::math {

class MathLogicKernelBuilder;
class LogicVec3;

class LogicScalar {
   public:
    LogicScalar() = default;

   private:
    friend class MathLogicKernelBuilder;
    friend class LogicVec3Ref;
    friend LogicScalar operator-(LogicScalar v);
    friend LogicVec3 operator*(LogicVec3 v, LogicScalar s);
    friend LogicVec3 operator*(LogicScalar s, LogicVec3 v);

    LogicScalar(MathLogicKernelBuilder* builder, u16 node) : builder_(builder), node_(node) {}

    MathLogicKernelBuilder* builder_ = nullptr;
    u16 node_ = 0xFFFFu;
};

class LogicVec3 {
   public:
    LogicVec3() = default;

   private:
    friend class MathLogicKernelBuilder;
    friend class LogicVec3Ref;
    friend LogicVec3 operator+(LogicVec3 a, LogicVec3 b);
    friend LogicVec3 operator-(LogicVec3 a, LogicVec3 b);
    friend LogicVec3 operator*(LogicVec3 v, LogicScalar s);
    friend LogicVec3 operator*(LogicScalar s, LogicVec3 v);
    friend LogicVec3 operator*(LogicVec3 v, f32 s);
    friend LogicVec3 operator*(f32 s, LogicVec3 v);

    LogicVec3(MathLogicKernelBuilder* builder, u16 node) : builder_(builder), node_(node) {}

    MathLogicKernelBuilder* builder_ = nullptr;
    u16 node_ = 0xFFFFu;
};

class LogicVec3Ref {
   public:
    LogicVec3Ref() = default;

    operator LogicVec3() const noexcept { return LogicVec3{builder_, node_}; }
    LogicVec3Ref& operator=(LogicVec3 expr);

   private:
    friend class MathLogicKernelBuilder;

    LogicVec3Ref(MathLogicKernelBuilder* builder, u16 stream, u16 node)
        : builder_(builder), stream_(stream), node_(node) {}

    MathLogicKernelBuilder* builder_ = nullptr;
    u16 stream_ = 0xFFFFu;
    u16 node_ = 0xFFFFu;
};

struct MathLogicKernelStats {
    u32 instructions = 0;
    u32 vec3_registers = 0;
    u32 vec3_streams = 0;
    u32 f32_uniforms = 0;
    u32 vec3_uniforms = 0;
    u32 stores = 0;
    u32 fused_madd = 0;
    usize chunk_elements = 0;
};

class MathLogicKernel {
   public:
    MathLogicKernel() = default;

    [[nodiscard]] const MathLogicKernelStats& stats() const noexcept { return stats_; }

    bool set_f32_uniform(u16 slot, f32 value) noexcept;
    bool set_vec3_uniform(u16 slot, Vec3 value) noexcept;

    [[nodiscard]] usize execute(std::span<MutableVec3SoaView> vec3_streams) noexcept;

   private:
    friend class MathLogicKernelBuilder;

    enum class InstrKind : u8 {
        LoadVec3Stream,
        LoadVec3Uniform,
        Vec3Add,
        Vec3Sub,
        Vec3MulScalar,
        Vec3Madd,
        Vec3StreamMaddStore,
        Vec3ChainedMaddStore,
        StoreVec3Stream,
    };

    enum class ScalarKind : u8 {
        Constant,
        Uniform,
    };

    struct ScalarSource {
        ScalarKind kind = ScalarKind::Constant;
        u16 slot = 0;
        f32 value = 0.0f;
        f32 scale = 1.0f;
    };

    struct Instruction {
        InstrKind kind = InstrKind::LoadVec3Stream;
        u16 dst = 0;
        u16 a = 0;
        u16 b = 0;
        u16 stream = 0;
        u16 uniform = 0;
        ScalarSource scalar{};
        ScalarSource scalar_b{};
        ScalarSource scalar_c{};
        u8 scalar_count = 0;
    };

    void ensure_scratch(usize registers, usize chunk_elements);
    [[nodiscard]] usize resolve_count(std::span<MutableVec3SoaView> vec3_streams) const noexcept;

    detail::VectorStackVector<Instruction> program_;
    detail::VectorStackVector<f32> f32_uniforms_;
    detail::VectorStackVector<Vec3> vec3_uniforms_;
    detail::VectorStackVector<u8> vec3_stream_used_;
    detail::VectorStackVector<f32> scratch_x_;
    detail::VectorStackVector<f32> scratch_y_;
    detail::VectorStackVector<f32> scratch_z_;
    MathLogicKernelStats stats_{};
};

class MathLogicKernelBuilder {
   public:
    MathLogicKernelBuilder() = default;

    void begin_record();
    [[nodiscard]] MathLogicKernel end_record() const;

    LogicVec3Ref vec3_stream(u16 stream);
    LogicVec3 vec3_uniform(u16 slot, Vec3 default_value = Vec3{0.0f, 0.0f, 0.0f});
    LogicScalar f32_uniform(u16 slot, f32 default_value = 0.0f);
    LogicScalar f32_constant(f32 value);

    [[nodiscard]] MathLogicKernel compile() const;

   private:
    friend class LogicVec3Ref;
    friend LogicScalar operator-(LogicScalar v);
    friend LogicVec3 operator+(LogicVec3 a, LogicVec3 b);
    friend LogicVec3 operator-(LogicVec3 a, LogicVec3 b);
    friend LogicVec3 operator*(LogicVec3 v, LogicScalar s);
    friend LogicVec3 operator*(LogicScalar s, LogicVec3 v);
    friend LogicVec3 operator*(LogicVec3 v, f32 s);
    friend LogicVec3 operator*(f32 s, LogicVec3 v);

    enum class NodeKind : u8 {
        Vec3Stream,
        Vec3Uniform,
        F32Constant,
        F32Uniform,
        F32Neg,
        Vec3Add,
        Vec3Sub,
        Vec3MulScalar,
    };

    struct Node {
        NodeKind kind = NodeKind::Vec3Stream;
        u16 a = 0xFFFFu;
        u16 b = 0xFFFFu;
        u16 slot = 0;
        f32 scalar = 0.0f;
        Vec3 vec3{};
    };

    struct Store {
        u16 stream = 0;
        u16 node = 0xFFFFu;
    };

    u16 add_node(Node node);
    u16 add_vec3_stream_node(u16 stream);
    void store_vec3(u16 stream, LogicVec3 expr);
    LogicVec3 make_vec3(NodeKind kind, LogicVec3 a, LogicVec3 b);
    LogicVec3 make_vec3_scalar(NodeKind kind, LogicVec3 v, LogicScalar s);
    LogicScalar make_scalar(NodeKind kind, LogicScalar v);

    detail::VectorStackVector<Node> nodes_;
    detail::VectorStackVector<Store> stores_;
    detail::VectorStackVector<f32> f32_uniform_defaults_;
    detail::VectorStackVector<Vec3> vec3_uniform_defaults_;
};

LogicScalar operator-(LogicScalar v);
LogicVec3 operator+(LogicVec3 a, LogicVec3 b);
LogicVec3 operator-(LogicVec3 a, LogicVec3 b);
LogicVec3 operator*(LogicVec3 v, LogicScalar s);
LogicVec3 operator*(LogicScalar s, LogicVec3 v);
LogicVec3 operator*(LogicVec3 v, f32 s);
LogicVec3 operator*(f32 s, LogicVec3 v);

}  // namespace psynder::math
