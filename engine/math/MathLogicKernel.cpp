// SPDX-License-Identifier: MIT
// Psynder — MathLogicKernel implementation.

#include "MathLogicKernel.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace psynder::math {

#if defined(_MSC_VER)
#define PSY_MATHLOGIC_RESTRICT __restrict
#elif defined(__clang__) || defined(__GNUC__)
#define PSY_MATHLOGIC_RESTRICT __restrict__
#else
#define PSY_MATHLOGIC_RESTRICT
#endif

namespace {

inline constexpr u16 kInvalidNode = 0xFFFFu;
inline constexpr usize kKernelChunkElements = 1024;

bool same_builder(MathLogicKernelBuilder* a, MathLogicKernelBuilder* b) noexcept {
    return a != nullptr && a == b;
}

}  // namespace

LogicVec3Ref& LogicVec3Ref::operator=(LogicVec3 expr) {
    if (same_builder(builder_, expr.builder_)) {
        builder_->store_vec3(stream_, expr);
        node_ = builder_->add_vec3_stream_node(stream_);
    }
    return *this;
}

bool MathLogicKernel::set_f32_uniform(u16 slot, f32 value) noexcept {
    if (slot >= f32_uniforms_.size())
        return false;
    f32_uniforms_[slot] = value;
    return true;
}

bool MathLogicKernel::set_vec3_uniform(u16 slot, Vec3 value) noexcept {
    if (slot >= vec3_uniforms_.size())
        return false;
    vec3_uniforms_[slot] = value;
    return true;
}

usize MathLogicKernel::execute(std::span<MutableVec3SoaView> vec3_streams) noexcept {
    const usize count = resolve_count(vec3_streams);
    if (count == 0 || program_.empty())
        return count;

    const usize chunk_capacity =
        stats_.chunk_elements != 0 ? stats_.chunk_elements : kKernelChunkElements;
    ensure_scratch(stats_.vec3_registers, chunk_capacity);

    for (usize base = 0; base < count; base += chunk_capacity) {
        const usize n = std::min(chunk_capacity, count - base);

        for (const Instruction& instr : program_) {
            f32* PSY_MATHLOGIC_RESTRICT rx =
                scratch_x_.data() + static_cast<usize>(instr.dst) * chunk_capacity;
            f32* PSY_MATHLOGIC_RESTRICT ry =
                scratch_y_.data() + static_cast<usize>(instr.dst) * chunk_capacity;
            f32* PSY_MATHLOGIC_RESTRICT rz =
                scratch_z_.data() + static_cast<usize>(instr.dst) * chunk_capacity;

            switch (instr.kind) {
                case InstrKind::LoadVec3Stream: {
                    const MutableVec3SoaView stream = vec3_streams[instr.stream];
                    const f32* PSY_MATHLOGIC_RESTRICT sx = stream.x + base;
                    const f32* PSY_MATHLOGIC_RESTRICT sy = stream.y + base;
                    const f32* PSY_MATHLOGIC_RESTRICT sz = stream.z + base;
                    for (usize i = 0; i < n; ++i) {
                        rx[i] = sx[i];
                        ry[i] = sy[i];
                        rz[i] = sz[i];
                    }
                    break;
                }
                case InstrKind::LoadVec3Uniform: {
                    const Vec3 v = vec3_uniforms_[instr.uniform];
                    for (usize i = 0; i < n; ++i) {
                        rx[i] = v.x;
                        ry[i] = v.y;
                        rz[i] = v.z;
                    }
                    break;
                }
                case InstrKind::Vec3Add: {
                    const f32* PSY_MATHLOGIC_RESTRICT ax =
                        scratch_x_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT ay =
                        scratch_y_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT az =
                        scratch_z_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT bx =
                        scratch_x_.data() + static_cast<usize>(instr.b) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT by =
                        scratch_y_.data() + static_cast<usize>(instr.b) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT bz =
                        scratch_z_.data() + static_cast<usize>(instr.b) * chunk_capacity;
                    for (usize i = 0; i < n; ++i) {
                        rx[i] = ax[i] + bx[i];
                        ry[i] = ay[i] + by[i];
                        rz[i] = az[i] + bz[i];
                    }
                    break;
                }
                case InstrKind::Vec3Sub: {
                    const f32* PSY_MATHLOGIC_RESTRICT ax =
                        scratch_x_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT ay =
                        scratch_y_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT az =
                        scratch_z_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT bx =
                        scratch_x_.data() + static_cast<usize>(instr.b) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT by =
                        scratch_y_.data() + static_cast<usize>(instr.b) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT bz =
                        scratch_z_.data() + static_cast<usize>(instr.b) * chunk_capacity;
                    for (usize i = 0; i < n; ++i) {
                        rx[i] = ax[i] - bx[i];
                        ry[i] = ay[i] - by[i];
                        rz[i] = az[i] - bz[i];
                    }
                    break;
                }
                case InstrKind::Vec3MulScalar: {
                    const f32 s =
                        (instr.scalar.kind == ScalarKind::Uniform ? f32_uniforms_[instr.scalar.slot]
                                                                  : instr.scalar.value) *
                        instr.scalar.scale;
                    const f32* PSY_MATHLOGIC_RESTRICT ax =
                        scratch_x_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT ay =
                        scratch_y_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT az =
                        scratch_z_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    for (usize i = 0; i < n; ++i) {
                        rx[i] = ax[i] * s;
                        ry[i] = ay[i] * s;
                        rz[i] = az[i] * s;
                    }
                    break;
                }
                case InstrKind::Vec3Madd: {
                    const f32 s =
                        (instr.scalar.kind == ScalarKind::Uniform ? f32_uniforms_[instr.scalar.slot]
                                                                  : instr.scalar.value) *
                        instr.scalar.scale;
                    const f32* PSY_MATHLOGIC_RESTRICT ax =
                        scratch_x_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT ay =
                        scratch_y_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT az =
                        scratch_z_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT bx =
                        scratch_x_.data() + static_cast<usize>(instr.b) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT by =
                        scratch_y_.data() + static_cast<usize>(instr.b) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT bz =
                        scratch_z_.data() + static_cast<usize>(instr.b) * chunk_capacity;
                    for (usize i = 0; i < n; ++i) {
                        rx[i] = ax[i] + bx[i] * s;
                        ry[i] = ay[i] + by[i] * s;
                        rz[i] = az[i] + bz[i] * s;
                    }
                    break;
                }
                case InstrKind::Vec3StreamMaddStore: {
                    f32 s = instr.scalar.kind == ScalarKind::Uniform
                                ? f32_uniforms_[instr.scalar.slot]
                                : instr.scalar.value;
                    s *= instr.scalar.scale;
                    if (instr.scalar_count > 1) {
                        const f32 sb = instr.scalar_b.kind == ScalarKind::Uniform
                                           ? f32_uniforms_[instr.scalar_b.slot]
                                           : instr.scalar_b.value;
                        s *= sb * instr.scalar_b.scale;
                    }

                    const MutableVec3SoaView dst = vec3_streams[instr.stream];
                    const MutableVec3SoaView base_stream = vec3_streams[instr.a];
                    const MutableVec3SoaView mul_stream = vec3_streams[instr.b];
                    // NO restrict here: the fusion builder guarantees instr.a ==
                    // instr.stream (base_stream is matched as `a.slot ==
                    // store.stream`), so dst and base_stream are the SAME stream
                    // and dx/bx always alias. A degenerate `x = x + x*s` makes mx
                    // alias too. Marking these restrict was a lie the optimizer is
                    // free to miscompile under -O2 (invisible at -O0/ASan).
                    f32* dx = dst.x + base;
                    f32* dy = dst.y + base;
                    f32* dz = dst.z + base;
                    const f32* bx = base_stream.x + base;
                    const f32* by = base_stream.y + base;
                    const f32* bz = base_stream.z + base;
                    const f32* mx = mul_stream.x + base;
                    const f32* my = mul_stream.y + base;
                    const f32* mz = mul_stream.z + base;
                    for (usize i = 0; i < n; ++i) {
                        dx[i] = bx[i] + mx[i] * s;
                        dy[i] = by[i] + my[i] * s;
                        dz[i] = bz[i] + mz[i] * s;
                    }
                    break;
                }
                case InstrKind::Vec3ChainedMaddStore: {
                    f32 s0 = instr.scalar.kind == ScalarKind::Uniform
                                 ? f32_uniforms_[instr.scalar.slot]
                                 : instr.scalar.value;
                    s0 *= instr.scalar.scale;
                    if (instr.scalar_count > 1) {
                        const f32 sb = instr.scalar_b.kind == ScalarKind::Uniform
                                           ? f32_uniforms_[instr.scalar_b.slot]
                                           : instr.scalar_b.value;
                        s0 *= sb * instr.scalar_b.scale;
                    }
                    const f32 s1 = (instr.scalar_c.kind == ScalarKind::Uniform
                                        ? f32_uniforms_[instr.scalar_c.slot]
                                        : instr.scalar_c.value) *
                                   instr.scalar_c.scale;

                    const MutableVec3SoaView movement = vec3_streams[instr.stream];
                    const MutableVec3SoaView accel = vec3_streams[instr.a];
                    const MutableVec3SoaView position = vec3_streams[instr.b];
                    f32* PSY_MATHLOGIC_RESTRICT mx = movement.x + base;
                    f32* PSY_MATHLOGIC_RESTRICT my = movement.y + base;
                    f32* PSY_MATHLOGIC_RESTRICT mz = movement.z + base;
                    const f32* PSY_MATHLOGIC_RESTRICT ax = accel.x + base;
                    const f32* PSY_MATHLOGIC_RESTRICT ay = accel.y + base;
                    const f32* PSY_MATHLOGIC_RESTRICT az = accel.z + base;
                    f32* PSY_MATHLOGIC_RESTRICT px = position.x + base;
                    f32* PSY_MATHLOGIC_RESTRICT py = position.y + base;
                    f32* PSY_MATHLOGIC_RESTRICT pz = position.z + base;
                    for (usize i = 0; i < n; ++i) {
                        const f32 nx = mx[i] + ax[i] * s0;
                        const f32 ny = my[i] + ay[i] * s0;
                        const f32 nz = mz[i] + az[i] * s0;
                        mx[i] = nx;
                        my[i] = ny;
                        mz[i] = nz;
                        px[i] += nx * s1;
                        py[i] += ny * s1;
                        pz[i] += nz * s1;
                    }
                    break;
                }
                case InstrKind::StoreVec3Stream: {
                    const f32* PSY_MATHLOGIC_RESTRICT ax =
                        scratch_x_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT ay =
                        scratch_y_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const f32* PSY_MATHLOGIC_RESTRICT az =
                        scratch_z_.data() + static_cast<usize>(instr.a) * chunk_capacity;
                    const MutableVec3SoaView stream = vec3_streams[instr.stream];
                    f32* PSY_MATHLOGIC_RESTRICT sx = stream.x + base;
                    f32* PSY_MATHLOGIC_RESTRICT sy = stream.y + base;
                    f32* PSY_MATHLOGIC_RESTRICT sz = stream.z + base;
                    for (usize i = 0; i < n; ++i) {
                        sx[i] = ax[i];
                        sy[i] = ay[i];
                        sz[i] = az[i];
                    }
                    break;
                }
            }
        }
    }

    return count;
}

void MathLogicKernel::ensure_scratch(usize registers, usize chunk_elements) {
    const usize values = registers * chunk_elements;
    if (scratch_x_.size() >= values)
        return;
    scratch_x_.resize(values);
    scratch_y_.resize(values);
    scratch_z_.resize(values);
}

usize MathLogicKernel::resolve_count(std::span<MutableVec3SoaView> vec3_streams) const noexcept {
    usize count = std::numeric_limits<usize>::max();
    bool any = false;

    for (usize i = 0; i < vec3_stream_used_.size(); ++i) {
        if (vec3_stream_used_[i] == 0)
            continue;
        if (i >= vec3_streams.size())
            return 0;

        const MutableVec3SoaView stream = vec3_streams[i];
        if (stream.x == nullptr || stream.y == nullptr || stream.z == nullptr)
            return 0;

        count = std::min(count, stream.count);
        any = true;
    }

    return any ? count : 0;
}

void MathLogicKernelBuilder::begin_record() {
    nodes_.clear();
    stores_.clear();
    f32_uniform_defaults_.clear();
    vec3_uniform_defaults_.clear();
}

MathLogicKernel MathLogicKernelBuilder::end_record() const {
    return compile();
}

LogicVec3Ref MathLogicKernelBuilder::vec3_stream(u16 stream) {
    Node node{};
    node.kind = NodeKind::Vec3Stream;
    node.slot = stream;
    return LogicVec3Ref{this, stream, add_node(node)};
}

LogicVec3 MathLogicKernelBuilder::vec3_uniform(u16 slot, Vec3 default_value) {
    if (slot >= vec3_uniform_defaults_.size())
        vec3_uniform_defaults_.resize(static_cast<usize>(slot) + 1);
    vec3_uniform_defaults_[slot] = default_value;

    Node node{};
    node.kind = NodeKind::Vec3Uniform;
    node.slot = slot;
    node.vec3 = default_value;
    return LogicVec3{this, add_node(node)};
}

LogicScalar MathLogicKernelBuilder::f32_uniform(u16 slot, f32 default_value) {
    if (slot >= f32_uniform_defaults_.size())
        f32_uniform_defaults_.resize(static_cast<usize>(slot) + 1);
    f32_uniform_defaults_[slot] = default_value;

    Node node{};
    node.kind = NodeKind::F32Uniform;
    node.slot = slot;
    node.scalar = default_value;
    return LogicScalar{this, add_node(node)};
}

LogicScalar MathLogicKernelBuilder::f32_constant(f32 value) {
    Node node{};
    node.kind = NodeKind::F32Constant;
    node.scalar = value;
    return LogicScalar{this, add_node(node)};
}

MathLogicKernel MathLogicKernelBuilder::compile() const {
    MathLogicKernel kernel;
    kernel.f32_uniforms_ = f32_uniform_defaults_;
    kernel.vec3_uniforms_ = vec3_uniform_defaults_;
    kernel.stats_.chunk_elements = kKernelChunkElements;
    kernel.stats_.stores = static_cast<u32>(stores_.size());
    kernel.stats_.f32_uniforms = static_cast<u32>(kernel.f32_uniforms_.size());
    kernel.stats_.vec3_uniforms = static_cast<u32>(kernel.vec3_uniforms_.size());

    detail::VectorStackVector<u16> reg_for_node;
    reg_for_node.resize(nodes_.size(), kInvalidNode);
    u16 next_reg = 0;

    auto mark_stream = [&](u16 stream) {
        if (stream >= kernel.vec3_stream_used_.size())
            kernel.vec3_stream_used_.resize(static_cast<usize>(stream) + 1);
        kernel.vec3_stream_used_[stream] = 1;
    };

    auto alloc_reg = [&]() -> u16 { return next_reg++; };

    auto scalar_source = [&](u16 node_index, auto&& scalar_source_ref) -> MathLogicKernel::ScalarSource {
        const Node& node = nodes_[node_index];
        switch (node.kind) {
            case NodeKind::F32Constant:
                return MathLogicKernel::ScalarSource{
                    MathLogicKernel::ScalarKind::Constant,
                    0,
                    node.scalar,
                    1.0f,
                };
            case NodeKind::F32Uniform:
                return MathLogicKernel::ScalarSource{
                    MathLogicKernel::ScalarKind::Uniform,
                    node.slot,
                    node.scalar,
                    1.0f,
                };
            case NodeKind::F32Neg: {
                MathLogicKernel::ScalarSource src = scalar_source_ref(node.a, scalar_source_ref);
                src.scale = -src.scale;
                return src;
            }
            default:
                return MathLogicKernel::ScalarSource{};
        }
    };

    struct ScaledStream {
        u16 stream = 0;
        MathLogicKernel::ScalarSource scalars[2]{};
        u8 scalar_count = 0;
    };

    auto peel_scaled_stream = [&](u16 node_index, auto&& peel_ref) -> ScaledStream {
        if (node_index == kInvalidNode || node_index >= nodes_.size())
            return {};

        const Node& node = nodes_[node_index];
        if (node.kind == NodeKind::Vec3Stream)
            return ScaledStream{node.slot, {}, 0};

        if (node.kind != NodeKind::Vec3MulScalar)
            return {};

        ScaledStream scaled = peel_ref(node.a, peel_ref);
        if (scaled.scalar_count >= 2)
            return {};
        scaled.scalars[scaled.scalar_count++] = scalar_source(node.b, scalar_source);
        return scaled;
    };

    auto emit_stream_madd_store = [&](const Store& store) -> bool {
        if (store.node == kInvalidNode || store.node >= nodes_.size())
            return false;

        const Node& node = nodes_[store.node];
        if (node.kind != NodeKind::Vec3Add)
            return false;

        const Node& a = nodes_[node.a];
        const Node& b = nodes_[node.b];
        u16 base_stream = 0xFFFFu;
        ScaledStream scaled{};

        if (a.kind == NodeKind::Vec3Stream && a.slot == store.stream) {
            base_stream = a.slot;
            scaled = peel_scaled_stream(node.b, peel_scaled_stream);
        } else if (b.kind == NodeKind::Vec3Stream && b.slot == store.stream) {
            base_stream = b.slot;
            scaled = peel_scaled_stream(node.a, peel_scaled_stream);
        } else {
            return false;
        }

        if (scaled.scalar_count == 0 || scaled.stream == 0xFFFFu)
            return false;

        mark_stream(store.stream);
        mark_stream(base_stream);
        mark_stream(scaled.stream);
        kernel.program_.push_back(MathLogicKernel::Instruction{
            MathLogicKernel::InstrKind::Vec3StreamMaddStore,
            0,
            base_stream,
            scaled.stream,
            store.stream,
            0,
            scaled.scalars[0],
            scaled.scalars[1],
            {},
            scaled.scalar_count,
        });
        ++kernel.stats_.fused_madd;
        return true;
    };

    auto match_stream_madd_store = [&](const Store& store) {
        struct Match {
            bool valid = false;
            u16 dst_stream = 0;
            u16 base_stream = 0;
            u16 mul_stream = 0;
            MathLogicKernel::ScalarSource scalars[2]{};
            u8 scalar_count = 0;
        };

        if (store.node == kInvalidNode || store.node >= nodes_.size())
            return Match{};

        const Node& node = nodes_[store.node];
        if (node.kind != NodeKind::Vec3Add)
            return Match{};

        const Node& a = nodes_[node.a];
        const Node& b = nodes_[node.b];
        u16 base_stream = 0xFFFFu;
        ScaledStream scaled{};

        if (a.kind == NodeKind::Vec3Stream && a.slot == store.stream) {
            base_stream = a.slot;
            scaled = peel_scaled_stream(node.b, peel_scaled_stream);
        } else if (b.kind == NodeKind::Vec3Stream && b.slot == store.stream) {
            base_stream = b.slot;
            scaled = peel_scaled_stream(node.a, peel_scaled_stream);
        } else {
            return Match{};
        }

        if (scaled.scalar_count == 0 || scaled.stream == 0xFFFFu)
            return Match{};

        return Match{
            true,
            store.stream,
            base_stream,
            scaled.stream,
            {scaled.scalars[0], scaled.scalars[1]},
            scaled.scalar_count,
        };
    };

    auto emit_chained_madd_store = [&](const Store& first, const Store& second) -> bool {
        const auto a = match_stream_madd_store(first);
        const auto b = match_stream_madd_store(second);
        if (!a.valid || !b.valid)
            return false;
        if (b.mul_stream != a.dst_stream || b.scalar_count != 1)
            return false;

        mark_stream(a.dst_stream);
        mark_stream(a.mul_stream);
        mark_stream(b.dst_stream);
        kernel.program_.push_back(MathLogicKernel::Instruction{
            MathLogicKernel::InstrKind::Vec3ChainedMaddStore,
            0,
            a.mul_stream,
            b.dst_stream,
            a.dst_stream,
            0,
            a.scalars[0],
            a.scalars[1],
            b.scalars[0],
            a.scalar_count,
        });
        kernel.stats_.fused_madd += 2;
        return true;
    };

    auto emit_vec = [&](u16 node_index, auto&& emit_vec_ref) -> u16 {
        if (node_index == kInvalidNode || node_index >= nodes_.size())
            return 0;
        if (reg_for_node[node_index] != kInvalidNode)
            return reg_for_node[node_index];

        const Node& node = nodes_[node_index];
        const u16 dst = alloc_reg();
        reg_for_node[node_index] = dst;

        switch (node.kind) {
            case NodeKind::Vec3Stream: {
                mark_stream(node.slot);
                kernel.program_.push_back(MathLogicKernel::Instruction{
                    MathLogicKernel::InstrKind::LoadVec3Stream,
                    dst,
                    0,
                    0,
                    node.slot,
                });
                break;
            }
            case NodeKind::Vec3Uniform: {
                kernel.program_.push_back(MathLogicKernel::Instruction{
                    MathLogicKernel::InstrKind::LoadVec3Uniform,
                    dst,
                    0,
                    0,
                    0,
                    node.slot,
                });
                break;
            }
            case NodeKind::Vec3Add: {
                const Node& a = nodes_[node.a];
                const Node& b = nodes_[node.b];
                if (a.kind == NodeKind::Vec3MulScalar) {
                    kernel.program_.push_back(MathLogicKernel::Instruction{
                        MathLogicKernel::InstrKind::Vec3Madd,
                        dst,
                        emit_vec_ref(node.b, emit_vec_ref),
                        emit_vec_ref(a.a, emit_vec_ref),
                        0,
                        0,
                        scalar_source(a.b, scalar_source),
                    });
                    ++kernel.stats_.fused_madd;
                } else if (b.kind == NodeKind::Vec3MulScalar) {
                    kernel.program_.push_back(MathLogicKernel::Instruction{
                        MathLogicKernel::InstrKind::Vec3Madd,
                        dst,
                        emit_vec_ref(node.a, emit_vec_ref),
                        emit_vec_ref(b.a, emit_vec_ref),
                        0,
                        0,
                        scalar_source(b.b, scalar_source),
                    });
                    ++kernel.stats_.fused_madd;
                } else {
                    kernel.program_.push_back(MathLogicKernel::Instruction{
                        MathLogicKernel::InstrKind::Vec3Add,
                        dst,
                        emit_vec_ref(node.a, emit_vec_ref),
                        emit_vec_ref(node.b, emit_vec_ref),
                    });
                }
                break;
            }
            case NodeKind::Vec3Sub: {
                kernel.program_.push_back(MathLogicKernel::Instruction{
                    MathLogicKernel::InstrKind::Vec3Sub,
                    dst,
                    emit_vec_ref(node.a, emit_vec_ref),
                    emit_vec_ref(node.b, emit_vec_ref),
                });
                break;
            }
            case NodeKind::Vec3MulScalar: {
                kernel.program_.push_back(MathLogicKernel::Instruction{
                    MathLogicKernel::InstrKind::Vec3MulScalar,
                    dst,
                    emit_vec_ref(node.a, emit_vec_ref),
                    0,
                    0,
                    0,
                    scalar_source(node.b, scalar_source),
                });
                break;
            }
            case NodeKind::F32Constant:
            case NodeKind::F32Uniform:
            case NodeKind::F32Neg:
                break;
        }

        return dst;
    };

    for (usize store_index = 0; store_index < stores_.size(); ++store_index) {
        const Store& store = stores_[store_index];
        if (store_index + 1 < stores_.size() &&
            emit_chained_madd_store(store, stores_[store_index + 1])) {
            ++store_index;
            continue;
        }

        if (emit_stream_madd_store(store))
            continue;

        const u16 src = emit_vec(store.node, emit_vec);
        mark_stream(store.stream);
        kernel.program_.push_back(MathLogicKernel::Instruction{
            MathLogicKernel::InstrKind::StoreVec3Stream,
            0,
            src,
            0,
            store.stream,
        });
    }

    kernel.stats_.instructions = static_cast<u32>(kernel.program_.size());
    kernel.stats_.vec3_registers = next_reg;
    kernel.stats_.vec3_streams = static_cast<u32>(kernel.vec3_stream_used_.size());
    return kernel;
}

u16 MathLogicKernelBuilder::add_node(Node node) {
    const usize index = nodes_.size();
    if (index >= kInvalidNode)
        return kInvalidNode;
    nodes_.push_back(node);
    return static_cast<u16>(index);
}

u16 MathLogicKernelBuilder::add_vec3_stream_node(u16 stream) {
    Node node{};
    node.kind = NodeKind::Vec3Stream;
    node.slot = stream;
    return add_node(node);
}

void MathLogicKernelBuilder::store_vec3(u16 stream, LogicVec3 expr) {
    if (!same_builder(this, expr.builder_) || expr.node_ == kInvalidNode)
        return;
    stores_.push_back(Store{stream, expr.node_});
}

LogicVec3 MathLogicKernelBuilder::make_vec3(NodeKind kind, LogicVec3 a, LogicVec3 b) {
    if (!same_builder(a.builder_, b.builder_))
        return {};

    Node node{};
    node.kind = kind;
    node.a = a.node_;
    node.b = b.node_;
    return LogicVec3{a.builder_, a.builder_->add_node(node)};
}

LogicVec3 MathLogicKernelBuilder::make_vec3_scalar(NodeKind kind, LogicVec3 v, LogicScalar s) {
    if (!same_builder(v.builder_, s.builder_))
        return {};

    Node node{};
    node.kind = kind;
    node.a = v.node_;
    node.b = s.node_;
    return LogicVec3{v.builder_, v.builder_->add_node(node)};
}

LogicScalar MathLogicKernelBuilder::make_scalar(NodeKind kind, LogicScalar v) {
    if (v.builder_ == nullptr)
        return {};

    Node node{};
    node.kind = kind;
    node.a = v.node_;
    return LogicScalar{v.builder_, v.builder_->add_node(node)};
}

LogicScalar operator-(LogicScalar v) {
    if (v.builder_ == nullptr)
        return {};
    return v.builder_->make_scalar(MathLogicKernelBuilder::NodeKind::F32Neg, v);
}

LogicVec3 operator+(LogicVec3 a, LogicVec3 b) {
    if (!same_builder(a.builder_, b.builder_))
        return {};
    return a.builder_->make_vec3(MathLogicKernelBuilder::NodeKind::Vec3Add, a, b);
}

LogicVec3 operator-(LogicVec3 a, LogicVec3 b) {
    if (!same_builder(a.builder_, b.builder_))
        return {};
    return a.builder_->make_vec3(MathLogicKernelBuilder::NodeKind::Vec3Sub, a, b);
}

LogicVec3 operator*(LogicVec3 v, LogicScalar s) {
    if (!same_builder(v.builder_, s.builder_))
        return {};
    return v.builder_->make_vec3_scalar(MathLogicKernelBuilder::NodeKind::Vec3MulScalar, v, s);
}

LogicVec3 operator*(LogicScalar s, LogicVec3 v) {
    return v * s;
}

LogicVec3 operator*(LogicVec3 v, f32 s) {
    if (v.builder_ == nullptr)
        return {};
    return v * v.builder_->f32_constant(s);
}

LogicVec3 operator*(f32 s, LogicVec3 v) {
    return v * s;
}

}  // namespace psynder::math

#undef PSY_MATHLOGIC_RESTRICT
