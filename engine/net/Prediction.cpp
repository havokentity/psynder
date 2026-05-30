// SPDX-License-Identifier: MIT
// Psynder - client prediction + server reconciliation impl. Lane 14 (Wave B).

#include "Prediction.h"

#include "scene/SceneGraph.h"  // LocalTransform / TransformComponent fields.

namespace psynder::net {

namespace {

// --- little-endian primitives (match Frame.cpp / Replication.cpp) -----------
PSY_FORCEINLINE void write_u32_le(u8* p, u32 v) noexcept {
    p[0] = u8(v & 0xFFu);
    p[1] = u8((v >> 8) & 0xFFu);
    p[2] = u8((v >> 16) & 0xFFu);
    p[3] = u8((v >> 24) & 0xFFu);
}
PSY_FORCEINLINE u32 read_u32_le(const u8* p) noexcept {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}
// f32 is moved through its IEEE-754 bit pattern so the wire is byte-exact and
// endian-stable (no implementation-defined float endianness assumptions beyond
// the little-endian byte order the whole protocol already mandates).
PSY_FORCEINLINE void write_f32_le(u8* p, f32 v) noexcept {
    u32 bits;
    __builtin_memcpy(&bits, &v, sizeof(bits));
    write_u32_le(p, bits);
}
PSY_FORCEINLINE f32 read_f32_le(const u8* p) noexcept {
    u32 bits = read_u32_le(p);
    f32 v;
    __builtin_memcpy(&v, &bits, sizeof(v));
    return v;
}

}  // namespace

bool encode_input(const InputCmd& c, std::span<u8> out) noexcept {
    if (out.size() < kInputCmdBytes)
        return false;
    u8* p = out.data();
    write_u32_le(p + 0, c.seq);
    write_u32_le(p + 4, c.tick);
    write_f32_le(p + 8, c.move_x);
    write_f32_le(p + 12, c.move_y);
    write_f32_le(p + 16, c.move_z);
    write_f32_le(p + 20, c._pad);
    return true;
}

bool decode_input(std::span<const u8> in, InputCmd& c) noexcept {
    if (in.size() < kInputCmdBytes)
        return false;
    const u8* p = in.data();
    c.seq = read_u32_le(p + 0);
    c.tick = read_u32_le(p + 4);
    c.move_x = read_f32_le(p + 8);
    c.move_y = read_f32_le(p + 12);
    c.move_z = read_f32_le(p + 16);
    c._pad = read_f32_le(p + 20);
    return true;
}

// The single deterministic integrator. Fixed-step: one input == one position
// delta. No wall-clock, no RNG, no global state -> identical on both ends.
math::Vec3 step_state(const math::Vec3& pos, const InputCmd& cmd) noexcept {
    return math::Vec3{pos.x + cmd.move_x, pos.y + cmd.move_y, pos.z + cmd.move_z};
}

// --- InputRing ---------------------------------------------------------------
bool InputRing::push(const InputCmd& cmd) noexcept {
    if (count_ >= kInputRingCap)
        return false;  // full: alloc-free contract forbids growth.
    buf_[count_++] = cmd;
    return true;
}

u32 InputRing::drop_acked(u32 acked_seq) noexcept {
    if (count_ == 0 || acked_seq == 0)
        return 0;
    // Live entries are seq-ordered + packed at the front. Count the acked
    // prefix, then shift the survivors down. Bounded by count_ <= cap.
    u32 dropped = 0;
    while (dropped < count_ && buf_[dropped].seq <= acked_seq)
        ++dropped;
    if (dropped == 0)
        return 0;
    const u32 remain = count_ - dropped;
    for (u32 i = 0; i < remain; ++i)
        buf_[i] = buf_[i + dropped];
    count_ = remain;
    return dropped;
}

// --- Predictor ---------------------------------------------------------------
void Predictor::bind(Entity controlled, math::Vec3 initial_pos) noexcept {
    controlled_ = controlled;
    predicted_pos_ = initial_pos;
    next_seq_ = 1;
    ring_.clear();
}

void Predictor::write_ecs_(scene::EcsRegistry& registry) noexcept {
    if (!controlled_.valid())
        return;
    if (auto* pc = registry.get<PredictedComponent>(controlled_)) {
        pc->pos = predicted_pos_;
        pc->last_applied_input = ring_.count() ? ring_.last_seq() : pc->last_applied_input;
    }
    // Mirror the predicted position into the Transform so the rest of the
    // world (render, physics queries) sees the predicted avatar.
    if (auto* tc = registry.get<scene::TransformComponent>(controlled_))
        tc->local.translation = predicted_pos_;
}

InputCmd Predictor::predict(scene::EcsRegistry& registry,
                            u32 client_tick,
                            math::Vec3 move) noexcept {
    InputCmd cmd{};
    cmd.seq = next_seq_++;
    cmd.tick = client_tick;
    cmd.move_x = move.x;
    cmd.move_y = move.y;
    cmd.move_z = move.z;

    // Apply immediately (zero perceived latency) and buffer for replay.
    predicted_pos_ = step_state(predicted_pos_, cmd);
    ring_.push(cmd);  // full-ring => not buffered; still predicted locally.
    write_ecs_(registry);
    return cmd;
}

void Predictor::reconcile(scene::EcsRegistry& registry,
                          math::Vec3 auth_pos,
                          u32 server_acked_input) noexcept {
    // 1. Drop inputs the server has already folded into auth_pos.
    ring_.drop_acked(server_acked_input);
    // 2. Reset to the authoritative truth, then REPLAY the still-unacked
    //    inputs from that base. If prediction was correct the result equals
    //    what we already showed (no visible snap); if the server diverged we
    //    converge here without rubber-banding the unacked motion.
    predicted_pos_ = auth_pos;
    for (const InputCmd& c : ring_.unacked())
        predicted_pos_ = step_state(predicted_pos_, c);
    write_ecs_(registry);
}

// --- ServerInputProcessor ----------------------------------------------------
void ServerInputProcessor::bind(Entity controlled, math::Vec3 initial_pos) noexcept {
    controlled_ = controlled;
    auth_pos_ = initial_pos;
    acked_input_ = 0;
}

void ServerInputProcessor::process(scene::EcsRegistry& registry, const InputCmd& cmd) noexcept {
    if (cmd.seq == 0 || cmd.seq <= acked_input_)
        return;  // duplicate / out-of-order: authoritative ordering wins.
    auth_pos_ = step_state(auth_pos_, cmd);
    acked_input_ = cmd.seq;
    if (!controlled_.valid())
        return;
    if (auto* pc = registry.get<PredictedComponent>(controlled_)) {
        pc->pos = auth_pos_;
        pc->last_applied_input = acked_input_;
    }
    if (auto* tc = registry.get<scene::TransformComponent>(controlled_))
        tc->local.translation = auth_pos_;
}

}  // namespace psynder::net
