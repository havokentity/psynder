// SPDX-License-Identifier: MIT
// Psynder physics -- articulated RAGDOLL primitive (DESIGN.md ADR-019).
//
// A ragdoll is a small articulated assembly of rigid capsule/box limb segments
// (pelvis / torso / head + 2 upper + 2 lower limbs, ~6-9 bodies) connected by
// the EXISTING joint solver (engine/physics/Joints.h). It is the foundation for
// death ragdolls: a dying character's skeleton is spawned at a world transform
// with the character's velocity and then collapses into a physically simulated
// rag that drops, settles on the ground, and stays connected.
//
// -- How it composes the engine ---------------------------------------------
// Nothing here changes the solver, integrator or narrowphase. A ragdoll is just
// a set of World bodies + a JointSet, driven by the documented public path:
//
//     physics::World world;
//     physics::Ragdoll rag;
//     physics::RagdollDesc desc = physics::default_humanoid();
//     physics::build_ragdoll(world, desc, world_transform, initial_velocity, rag);
//     ...
//     for each frame:
//         world.step(dt);
//         physics::solve_ragdoll(world, rag, dt);   // == solve_joints over rag.joints
//     ...
//     physics::destroy_ragdoll(world, rag);         // generation-safe teardown
//
// step() integrates gravity + resolves contacts (the capsules rest on the
// ground via the W9-3 capsule manifold + the W8 colored solver). solve_ragdoll
// then runs the joint pass so the limbs stay pinned at their joints. Both are
// deterministic and allocate nothing on the hot path beyond solve_joints' own
// pooled scratch.
//
// -- Joint / limit scheme ----------------------------------------------------
// Each parent->child link is a BALL-SOCKET pin (3-DoF rotation about the joint
// point) carrying full BodyIds (joint_with_handles) so a destroyed-and-recycled
// body cannot be pulled by a dangling joint. The ball-socket alone leaves all
// three rotational DoF free, which would let a limb fold straight back through
// its parent. To stop unnatural inversion WITHOUT touching the solver, each link
// also carries an additive soft CONE LIMIT: a Rope joint between an off-axis
// anchor on the parent and the matching anchor on the child. The rope is slack
// inside the allowed cone and only pulls once the limb splays past the limit
// half-angle, so the limb swings freely within a natural range and is gently
// caught at the extreme rather than inverting. Rope + ball-socket are both
// stock joint kinds -- the ragdoll is purely a CONFIGURATION of the existing
// solver, no new constraint math.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "physics/Joints.h"
#include "physics/Physics.h"

#include <vector>

namespace psynder::physics {

// One limb segment of a ragdoll skeleton. Transforms are RELATIVE to the
// ragdoll root (the world_transform passed to build_ragdoll), so a desc is a
// reusable template that can be spawned anywhere. `parent` is the index of the
// segment this one hangs off (kRagdollRoot for the root segment, which has no
// inbound joint). The joint connecting this segment to its parent pivots about
// `joint_local` expressed in the PARENT's local frame; the same world point is
// `child_joint_local` in this segment's local frame.
struct RagdollSegment {
    Shape shape = Shape::Capsule;          // Capsule or Box limb
    f32 mass = 1.0f;                       // kg
    math::Vec3 local_position{0, 0, 0};    // segment centre, relative to root
    math::Quat local_rotation{0, 0, 0, 1}; // segment orientation, relative to root
    math::Vec3 half_extent{0.1f, 0.2f, 0.1f};

    u32 parent = 0xFFFFFFFFu;              // index of parent segment (root = sentinel)
    math::Vec3 joint_local{0, 0, 0};       // joint point in PARENT local space
    math::Vec3 child_joint_local{0, 0, 0}; // SAME point in THIS segment local space

    // Cone limit half-angle (radians) about the parent->child bone direction.
    // 0 disables the cone (pure ball-socket). A positive value adds the Rope
    // soft cone described in the file header so the limb cannot invert.
    f32 cone_limit = 0.0f;

    f32 friction = 0.6f;
    f32 restitution = 0.0f;
};

inline constexpr u32 kRagdollRoot = 0xFFFFFFFFu;

// A skeleton template: an ordered list of segments. Index 0 should be the root
// (parent == kRagdollRoot). Children must appear AFTER their parent so a single
// forward pass can place each segment relative to an already-placed parent.
// Pre-sized: build_ragdoll reserves the live id arrays from segment count, so
// the spawn does no incremental per-segment reallocation churn beyond the
// reserve. POD-ish; copy/template freely.
struct RagdollDesc {
    std::vector<RagdollSegment> segments;

    usize size() const noexcept { return segments.size(); }
    void clear() noexcept { segments.clear(); }
};

struct RagdollTag {};
using RagdollId = Handle<RagdollTag>;

// A live ragdoll instance: the World bodies it created + the JointSet that pins
// them, plus a generation-stamped id so a teardown is idempotent and a stale
// Ragdoll cannot double-free. `bodies[i]` is the BodyId for desc.segments[i]
// (same order), so a caller can read each limb's transform back from the World.
// The vectors are sized ONCE at build time and only cleared (capacity kept) on
// teardown -- the sim hot path (solve_ragdoll) never touches them.
struct Ragdoll {
    RagdollId id{};
    std::vector<BodyId> bodies;  // one per desc segment, in desc order
    JointSet joints;             // ball-socket pins + rope cone limits

    bool alive() const noexcept { return id.valid() && !bodies.empty(); }
    usize body_count() const noexcept { return bodies.size(); }
    void clear() noexcept {
        id = {};
        bodies.clear();
        joints.clear();
    }
};

// -- Default humanoid template ----------------------------------------------
// A 7-body upright humanoid (pelvis, torso, head, two arms, two legs) sized for
// a ~1.8 m character standing at the origin. Capsule limbs, box-ish torso. Each
// joint carries a natural cone limit so the figure collapses without limbs
// passing through the body. Returned by value -- build a custom RagdollDesc by
// hand for other skeletons.
RagdollDesc default_humanoid();

// -- Build / spawn -----------------------------------------------------------
// Create the ragdoll's bodies in `world` and connect them with ball-socket pins
// (+ rope cone limits) per the desc, placed at `world_transform` (a rotation +
// translation applied to every segment's relative transform) and seeded with
// `initial_velocity` on EVERY segment (so a dying character's momentum carries
// into the rag). Fills `out` with the new ids + joint set and returns its
// RagdollId (also stored in out.id). The joints use generation-safe BodyId
// handles. On any failure (empty desc) returns an invalid id and leaves `out`
// cleared. Deterministic: no RNG, no time; same inputs reproduce bit-for-bit.
RagdollId build_ragdoll(World& world,
                        const RagdollDesc& desc,
                        const math::Quat& world_rotation,
                        const math::Vec3& world_position,
                        const math::Vec3& initial_velocity,
                        Ragdoll& out);

// -- Per-frame joint solve ---------------------------------------------------
// Run AFTER world.step(dt): resolves the ragdoll's JointSet against the live
// bodies (a thin wrapper over solve_joints with ragdoll-tuned iteration counts).
// No-op for a torn-down / empty ragdoll. Allocates nothing on the hot path
// beyond solve_joints' own pooled resolve scratch.
void solve_ragdoll(World& world, const Ragdoll& rag, f32 dt, u32 iterations = 10);

// -- Teardown ----------------------------------------------------------------
// Destroy every body the ragdoll created (generation-safe: a stale id is a
// no-op, a body already gone is skipped) and clear the joint set + ids so the
// Ragdoll returns to an empty baseline. Idempotent -- calling twice is safe.
void destroy_ragdoll(World& world, Ragdoll& rag);

}  // namespace psynder::physics
