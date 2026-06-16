// SPDX-License-Identifier: MIT
// Psynder shooter_demo -- W12-4 death-ragdoll pool implementation (games-only).
//
// This TU is the ONLY place physics/Ragdoll.h (and its transitive
// physics/Joints.h -> physics/Shape.h) is compiled for the shooter demo. It does
// NOT include scene/SceneEcs.h / math/MathExt.h, so Shape.h's unqualified
// quat_rotate calls never collide with math::quat_rotate (see RagdollFx.h header
// comment for the ADL-ambiguity rationale). All scene/render work stays in
// main.cpp, which talks to this module through the Shape.h-free RagdollFx.h
// interface.
//
// Determinism: the only inputs are the spawn pose/impulse + dt; build_ragdoll /
// solve_ragdoll inherit the engine's -fno-fast-math bit-for-bit contract, and the
// settle book-keeping is plain float comparison -- no RNG, no time.

#include "RagdollFx.h"

#include "physics/Ragdoll.h"

#include <algorithm>
#include <cmath>

namespace shooter {

using psynder::math::Vec3;
using psynder::math::Quat;

namespace {

// Settle thresholds. A corpse is "settled" once every body moved less than
// kStillStep metres for kHoldFrames consecutive frames, while grounded + finite.
// kStillStep (m/frame at 60 Hz) ~= 0.42 m/s -- the slow quasi-static ground drift
// the engine ragdoll's own settle test treats as "at rest" (avg speed^2 < 0.2).
constexpr f32 kStillStep = 0.007f;   // per-frame motion threshold (m) at ~60 Hz
constexpr u32 kHoldFrames = 6u;      // debounce frames before declaring settled
constexpr f32 kGroundSlack = 0.2f;   // a body may dip this far below floor and still count

// World-space render box scale for a humanoid limb body. Capsules become their
// bounding box (diameter x full-length); boxes use the half-extent directly.
Vec3 limb_box_scale(const psynder::physics::RagdollSegment& seg) noexcept {
    if (seg.shape == psynder::physics::Shape::Capsule) {
        const f32 d = 2.0f * seg.half_extent.x;
        const f32 h = 2.0f * (seg.half_extent.y + seg.half_extent.x);
        return {d, h, d};
    }
    return {2.0f * seg.half_extent.x, 2.0f * seg.half_extent.y, 2.0f * seg.half_extent.z};
}

}  // namespace

struct RagdollPool::Impl {
    psynder::physics::RagdollDesc desc{};                 // shared humanoid template
    psynder::physics::BodyId floor{};                     // static settle floor
    f32 floor_y = 0.0f;

    struct Slot {
        psynder::physics::Ragdoll rag{};
        std::array<Vec3, kCorpseBodies> prev{};  // last-frame body positions (motion proxy)
        bool active = false;
        u32 settle_frames = 0u;
        CorpseState state{};
    };
    std::array<Slot, kMaxCorpses> slots{};
};

RagdollPool::RagdollPool() : impl_(std::make_unique<Impl>()) {}
RagdollPool::~RagdollPool() = default;

void RagdollPool::init(psynder::physics::World& world,
                       f32 floor_y,
                       f32 cx, f32 cz, f32 sx, f32 sz) {
    impl_->desc = psynder::physics::default_humanoid();
    impl_->floor_y = floor_y;

    // Static floor slab the corpses land + settle on. The demo's room has wall +
    // pillar colliders but NO floor collider, so add a thin static box at the
    // floor spanning the room. It sits well below chest-height horizontal rays so
    // it does not perturb the existing AI-LOS / player-hitscan behaviour.
    psynder::physics::BodyDesc fd{};
    fd.shape = psynder::physics::Shape::Box;
    fd.mass = 0.0f;  // static
    fd.position = {cx, floor_y - 0.05f, cz};
    fd.half_extent = {0.5f * sx, 0.05f, 0.5f * sz};
    fd.friction = 0.85f;
    impl_->floor = world.create_body(fd);
}

u32 RagdollPool::spawn(psynder::physics::World& world,
                       const Vec3& root_position,
                       const Quat& root_rotation,
                       const Vec3& impulse) {
    // Grab the first idle slot. Pool sized to kMaxCorpses (== max enemies), so a
    // full pool only happens if every enemy is a live corpse already.
    Impl::Slot* slot = nullptr;
    u32 index = kNoSlot;
    for (u32 i = 0; i < kMaxCorpses; ++i) {
        if (!impl_->slots[i].active) {
            slot = &impl_->slots[i];
            index = i;
            break;
        }
    }
    if (!slot)
        return kNoSlot;

    const psynder::physics::RagdollId id = psynder::physics::build_ragdoll(
        world, impl_->desc, root_rotation, root_position, impulse, slot->rag);
    if (!id.valid())
        return kNoSlot;

    slot->active = true;
    slot->settle_frames = 0u;
    slot->state = CorpseState{};
    slot->state.active = true;
    slot->state.body_count = static_cast<u32>(slot->rag.body_count());
    // Seed the motion proxy so the first frame's delta is ~0 (no false settle).
    const usize n = std::min<usize>(slot->rag.body_count(), kCorpseBodies);
    for (usize i = 0; i < n; ++i) {
        const psynder::physics::BodyId b = slot->rag.bodies[i];
        slot->prev[i] = b.valid() ? world.get_position(b) : Vec3{0.0f, 0.0f, 0.0f};
    }
    ++spawned_total_;
    return index;
}

void RagdollPool::step(psynder::physics::World& world, f32 dt) {
    for (auto& slot : impl_->slots) {
        if (!slot.active)
            continue;

        // Joint pass (the World already integrated + resolved contacts in step()).
        psynder::physics::solve_ragdoll(world, slot.rag, dt);

        f32 max_step = 0.0f;
        f32 min_y = 1e30f;
        bool finite = true;
        const usize n = std::min<usize>(slot.rag.body_count(), kCorpseBodies);
        for (usize i = 0; i < n; ++i) {
            const psynder::physics::BodyId b = slot.rag.bodies[i];
            if (!b.valid())
                continue;
            const Vec3 p = world.get_position(b);
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                finite = false;
            min_y = std::min(min_y, p.y);
            const Vec3 d = psynder::math::sub(p, slot.prev[i]);
            max_step = std::max(max_step, psynder::math::length(d));
            slot.prev[i] = p;
        }

        const bool grounded = min_y > impl_->floor_y - kGroundSlack;
        if (finite && grounded && max_step < kStillStep) {
            if (slot.settle_frames < 0xFFFFFFFFu)
                ++slot.settle_frames;
        } else {
            slot.settle_frames = 0u;
        }

        slot.state.active = true;
        slot.state.finite = finite;
        slot.state.grounded = grounded;
        slot.state.min_y = (min_y < 1e29f) ? min_y : impl_->floor_y;
        slot.state.settled = slot.settle_frames >= kHoldFrames;
        slot.state.body_count = static_cast<u32>(n);
    }
}

void RagdollPool::limb_transforms(psynder::physics::World& world,
                                  u32 slot,
                                  std::span<LimbTransform> out) const {
    if (slot >= kMaxCorpses)
        return;
    const Impl::Slot& s = impl_->slots[slot];
    if (!s.active)
        return;
    const usize n = std::min<usize>({s.rag.body_count(), kCorpseBodies, out.size()});
    for (usize i = 0; i < n; ++i) {
        const psynder::physics::BodyId b = s.rag.bodies[i];
        if (!b.valid()) {
            out[i].valid = false;
            continue;
        }
        out[i].position = world.get_position(b);
        out[i].rotation = world.get_rotation(b);
        out[i].box_scale = limb_box_scale(impl_->desc.segments[i]);
        out[i].valid = true;
    }
}

CorpseState RagdollPool::state(u32 slot) const {
    if (slot >= kMaxCorpses)
        return CorpseState{};
    return impl_->slots[slot].state;
}

void RagdollPool::teardown(psynder::physics::World& world) {
    for (auto& slot : impl_->slots) {
        if (slot.active || slot.rag.alive())
            psynder::physics::destroy_ragdoll(world, slot.rag);
        slot.active = false;
        slot.state = CorpseState{};
    }
}

}  // namespace shooter
