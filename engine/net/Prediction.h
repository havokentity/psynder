// SPDX-License-Identifier: MIT
// Psynder - client-side prediction + server reconciliation. Lane 14 (Wave B).
//
// The replication layer (Replication.{h,cpp}) streams authoritative state from
// server to client and interpolates REMOTE entities between snapshots. That is
// the right model for entities the local player does not control. For the
// player's OWN controlled entity, interpolation feels laggy: the client would
// only ever see its avatar move one RTT after pressing a key.
//
// This module adds the standard predict-and-reconcile model (Quake3 /
// Source / Overwatch GDC):
//
//   PREDICTION  - the client applies local input to its controlled entity
//                 IMMEDIATELY (same tick), so the avatar responds with zero
//                 perceived latency. Each input is stamped with a monotonically
//                 increasing sequence and buffered in an input RING.
//
//   RECONCILE   - the authoritative snapshot for the controlled entity carries
//                 the input seq the server last processed (`acked_input`). On
//                 each snapshot the client RESETS its predicted state to the
//                 authoritative state, drops inputs the server has already
//                 consumed, then REPLAYS the remaining (still-unacked) inputs
//                 from that authoritative base. If the client predicted
//                 correctly, the replayed result equals what it already showed
//                 (no visible correction); if the server diverged (e.g. a
//                 collision the client missed), the replay converges to the
//                 server's truth WITHOUT rubber-banding the unacked motion.
//
// Determinism: the per-input step (`step_state`) is a pure fixed-step function
// of (state, cmd) - no wall-clock, no RNG - so the client's replay reproduces
// the server's integration exactly given the same inputs. The fixed step keeps
// server and client in lockstep on the controlled entity.
//
// Alloc-free: the input ring and the predicted-state are fixed-capacity POD
// buffers owned by the client; nothing heap-allocates per tick. Inputs are
// ECS-component-driven - the controlled entity carries a PredictedComponent
// the predictor reads/writes, so the model plugs into the DOTS world rather
// than living in a side table.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "scene/EcsRegistry.h"
#include "scene/SceneEcs.h"

#include <array>
#include <span>

namespace psynder::net {

// --- Wire-stable input command ----------------------------------------------
// One frame of player intent for the controlled entity. POD + trivially
// copyable so its raw little-endian bytes ARE the wire payload (mirrors the
// replication wire discipline). `move` is a per-tick displacement intent
// (units/tick); a real game would carry a button bitmask + look angles, but a
// displacement vector is enough to exercise predict/replay deterministically.
struct InputCmd {
    u32 seq = 0;     // monotonically increasing; 0 == "none".
    u32 tick = 0;    // client fixed-step tick the input was sampled on.
    f32 move_x = 0.f;
    f32 move_y = 0.f;
    f32 move_z = 0.f;
    f32 _pad = 0.f;  // keep 24-byte size stable / 4-byte aligned.
};
static_assert(sizeof(InputCmd) == 24, "InputCmd must stay 24 bytes for the wire");

inline constexpr usize kInputCmdBytes = 24;

// Encode / decode one InputCmd in little-endian. Returns false if `out`/`in`
// is too small. Endian-stable + padding-independent (hand-serialized).
bool encode_input(const InputCmd& c, std::span<u8> out) noexcept;
bool decode_input(std::span<const u8> in, InputCmd& c) noexcept;

// --- Predicted ECS component -------------------------------------------------
// The controlled entity carries this. `pos` is the predicted position the
// predictor integrates; the engine's TransformComponent is updated from it so
// the rest of the world sees the predicted avatar. `last_applied_input` is the
// highest input seq folded into `pos` (predicted) or processed (server).
PSYNDER_COMPONENT(PredictedComponent) {
    math::Vec3 pos{0.f, 0.f, 0.f};
    u32 last_applied_input = 0;
    u32 _pad = 0;  // keep trivially copyable / 8-byte aligned.
};

// The pure fixed-step integrator shared by client prediction and server
// authority. Given the current position and one input command, returns the
// next position. Deterministic: depends ONLY on its arguments. This is the
// single source of truth both ends run so a replay reproduces the server.
math::Vec3 step_state(const math::Vec3& pos, const InputCmd& cmd) noexcept;

// --- Input ring --------------------------------------------------------------
// Fixed-capacity ring of unacked inputs. The client appends on each sampled
// frame and drops the prefix the server has acknowledged. Sized for the worst
// case in-flight window at 60Hz over a high-RTT link; pooled, never grows.
inline constexpr usize kInputRingCap = 256;

class InputRing {
   public:
    // Append `cmd` (its seq must be > the last appended seq). Returns false if
    // the ring is full (caller must let the server catch up); the alloc-free
    // contract means we never grow past kInputRingCap.
    bool push(const InputCmd& cmd) noexcept;

    // Drop every buffered input with seq <= `acked_seq` (the server has
    // processed them; they no longer need replay). Returns the count dropped.
    u32 drop_acked(u32 acked_seq) noexcept;

    // Inputs still awaiting ack, in seq order. Read-only view into the ring's
    // contiguous live span (the ring keeps live entries packed at the front
    // after drop_acked, so this is a simple span).
    std::span<const InputCmd> unacked() const noexcept {
        return std::span<const InputCmd>(buf_.data(), count_);
    }

    u32 count() const noexcept { return count_; }
    void clear() noexcept { count_ = 0; }
    u32 last_seq() const noexcept { return count_ ? buf_[count_ - 1].seq : 0u; }

   private:
    std::array<InputCmd, kInputRingCap> buf_{};
    u32 count_ = 0;  // live entries packed at [0, count_).
};

// --- Predictor (client-side) -------------------------------------------------
// Owns the controlled entity's predicted state + the input ring. The flow per
// client fixed-step tick:
//   1. predict(cmd)            - integrate the new input, write the predicted
//                                pos to the ECS PredictedComponent + Transform,
//                                buffer the input in the ring.
//   2. (network) send the cmd to the server.
//   3. on each snapshot: reconcile(authoritative_pos, server_acked_input)
//                                - reset to authoritative, drop acked inputs,
//                                  replay the remainder, re-write the ECS.
class Predictor {
   public:
    // Bind the controlled entity. It must already carry a PredictedComponent +
    // TransformComponent. `initial_pos` seeds the predicted state.
    void bind(Entity controlled, math::Vec3 initial_pos) noexcept;

    Entity controlled() const noexcept { return controlled_; }
    math::Vec3 predicted_pos() const noexcept { return predicted_pos_; }
    u32 next_input_seq() const noexcept { return next_seq_; }
    u32 unacked_count() const noexcept { return ring_.count(); }

    // Allocate the next input seq, integrate it onto the predicted state
    // (same tick - zero perceived latency), write it through to the ECS, and
    // buffer it. Returns the issued command (so the caller can ship it). On a
    // full ring the input is still integrated but NOT buffered (it cannot be
    // replayed; in practice the server keeps up well within kInputRingCap).
    InputCmd predict(scene::EcsRegistry& registry, u32 client_tick, math::Vec3 move) noexcept;

    // Reconcile against an authoritative update for the controlled entity:
    // `auth_pos` is the server's position, `server_acked_input` is the highest
    // input seq the server had processed when it produced that state. Resets
    // the predicted state to `auth_pos`, drops acked inputs, replays the rest,
    // and writes the converged state back to the ECS. Idempotent if nothing is
    // unacked (predicted == authoritative).
    void reconcile(scene::EcsRegistry& registry,
                   math::Vec3 auth_pos,
                   u32 server_acked_input) noexcept;

   private:
    void write_ecs_(scene::EcsRegistry& registry) noexcept;

    Entity controlled_ = kInvalidEntity;
    math::Vec3 predicted_pos_{0.f, 0.f, 0.f};
    u32 next_seq_ = 1;  // 0 reserved as "none".
    InputRing ring_{};
};

// --- Server-side input processor --------------------------------------------
// Applies client inputs in seq order to the controlled entity's AUTHORITATIVE
// state and tracks the highest seq processed (echoed back to the client as the
// reconcile ack). Deterministic: runs the same step_state the client predicts
// with, so a correctly-predicting client never sees a correction.
class ServerInputProcessor {
   public:
    void bind(Entity controlled, math::Vec3 initial_pos) noexcept;

    Entity controlled() const noexcept { return controlled_; }
    math::Vec3 authoritative_pos() const noexcept { return auth_pos_; }
    u32 acked_input() const noexcept { return acked_input_; }

    // Process one input command. Out-of-order / already-processed (seq <=
    // acked) commands are ignored (the server is authoritative on ordering).
    // Advances the authoritative state and writes it to the ECS.
    void process(scene::EcsRegistry& registry, const InputCmd& cmd) noexcept;

   private:
    Entity controlled_ = kInvalidEntity;
    math::Vec3 auth_pos_{0.f, 0.f, 0.f};
    u32 acked_input_ = 0;
};

}  // namespace psynder::net
