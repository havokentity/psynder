// SPDX-License-Identifier: MIT
// Psynder physics -- articulated ragdoll primitive (DESIGN.md ADR-019).
//
// Composes World bodies + the stock joint solver (Joints.h) into an articulated
// rag. See Ragdoll.h for the joint/limit scheme. This TU only uses the PUBLIC
// World + joint surface (create_body / set_body_* / solve_joints / destroy_body)
// -- it does NOT reach into WorldImpl, the colored solver, the integrator or the
// narrowphase, so the ragdoll is purely additive over the existing engine.
//
// Determinism: inherits the lane-wide -fno-fast-math gate from
// engine/physics/CMakeLists.txt; no RNG, no time, pure field math, so a scripted
// build + solve sequence reproduces bit-for-bit (same contract as solve_joints).

#include "physics/Ragdoll.h"

#include "physics/Shape.h"  // detail::quat_rotate

#include <cmath>

namespace psynder::physics {

namespace {

// Rotate a body-local offset by a quaternion (qualified to dodge the
// math/physics quat_rotate ADL ambiguity, same as Shape.h's helpers).
math::Vec3 rotate(const math::Quat& q, math::Vec3 v) noexcept {
    return detail::quat_rotate(q, v);
}

// Compose two rotations: result applies `child` first, then `parent`.
math::Quat compose(const math::Quat& parent, const math::Quat& child) noexcept {
    return math::quat_mul(parent, child);
}

}  // namespace

// ---------------------------------------------------------------------------
// Default humanoid template. Upright ~1.89 m figure with its pelvis hub centred
// at y = 1.0. +Y is up; capsules run along their local +Y. The skeleton is now
// authored anatomically TIGHT: adjacent segments TOUCH at every joint (no
// artificial surface gaps). This is safe because build_ragdoll stamps every
// segment with the shared kRagdollSelfCollideGroup filter (ADR-020), so the
// broadphase pair-acceptance rejects all limb-vs-limb pairs within the rag --
// the contact solver never sees a self-pair, so there is no contact-vs-joint
// fight that the old inter-segment gaps existed to avoid. Each joint's two
// anchors still name the SAME world point (now ON the touching surface), so the
// ball-socket pin starts exactly coincident. Cone limits let the figure fold
// naturally while preventing a limb from inverting.
//
// Geometry book-keeping (world Y/X at identity; capsule reach = half_h + r):
//   pelvis  box   he{0.14,0.10,0.10}     centre 1.00  -> top 1.10, bottom 0.90
//   torso   caps  r0.11 hh0.14 reach0.25 centre 1.35  -> bottom tip 1.10 (touch)
//   head    caps  r0.085 hh0.06 reach.145 centre 1.745-> bottom tip 1.60 (touch)
//   arm     caps  r0.05 hh0.18 reach0.23  centre x0.16 -> inner surf 0.11 (touch)
//   leg     caps  r0.07 hh0.24 reach0.31  centre 0.59  -> top tip 0.90 (touch)
// ---------------------------------------------------------------------------
RagdollDesc default_humanoid() {
    RagdollDesc d;
    d.segments.reserve(7);

    const math::Quat kIdent{0, 0, 0, 1};

    // 0: pelvis (root) box hub.
    {
        RagdollSegment s{};
        s.shape = Shape::Box;
        s.mass = 12.0f;
        s.local_position = {0.0f, 1.0f, 0.0f};
        s.local_rotation = kIdent;
        s.half_extent = {0.14f, 0.10f, 0.10f};
        s.parent = kRagdollRoot;
        d.segments.push_back(s);
    }
    // 1: torso. Bottom tip TOUCHES the pelvis top at world y = 1.10; the spine
    // joint sits exactly on that shared surface. Anchors coincide there.
    {
        RagdollSegment s{};
        s.shape = Shape::Capsule;
        s.mass = 18.0f;
        s.local_position = {0.0f, 1.35f, 0.0f};
        s.local_rotation = kIdent;
        s.half_extent = {0.11f, 0.14f, 0.11f};  // radius, half-height
        s.parent = 0;
        s.joint_local = {0.0f, 0.10f, 0.0f};         // pelvis top   (world 1.10)
        s.child_joint_local = {0.0f, -0.25f, 0.0f};  // torso btm tip(world 1.10)
        s.cone_limit = 1.3f;                          // ~74 deg spine bend
        d.segments.push_back(s);
    }
    // 2: head. Bottom tip TOUCHES the torso top at world y = 1.60; the neck joint
    // sits on that shared surface.
    {
        RagdollSegment s{};
        s.shape = Shape::Capsule;
        s.mass = 5.0f;
        s.local_position = {0.0f, 1.745f, 0.0f};
        s.local_rotation = kIdent;
        s.half_extent = {0.085f, 0.06f, 0.085f};
        s.parent = 1;
        s.joint_local = {0.0f, 0.25f, 0.0f};         // torso top   (world 1.60)
        s.child_joint_local = {0.0f, -0.145f, 0.0f}; // head btm tip(world 1.60)
        s.cone_limit = 1.3f;                          // ~74 deg neck
        d.segments.push_back(s);
    }
    // 3: left arm. Inner surface TOUCHES the torso side: arm centre x = -0.16 so
    // its inner surface (-0.11) meets the torso surface (-0.11). The shoulder
    // joint is at world (-0.11, 1.45, 0), on that shared surface.
    {
        RagdollSegment s{};
        s.shape = Shape::Capsule;
        s.mass = 4.0f;
        s.local_position = {-0.16f, 1.30f, 0.0f};
        s.local_rotation = kIdent;
        s.half_extent = {0.05f, 0.18f, 0.05f};
        s.parent = 1;
        s.joint_local = {-0.11f, 0.10f, 0.0f};       // torso surf (world -0.11, 1.45)
        s.child_joint_local = {0.05f, 0.15f, 0.0f};  // arm inner  (world -0.11, 1.45)
        s.cone_limit = 1.6f;                          // ~92 deg shoulder
        d.segments.push_back(s);
    }
    // 4: right arm. Mirror of the left.
    {
        RagdollSegment s{};
        s.shape = Shape::Capsule;
        s.mass = 4.0f;
        s.local_position = {0.16f, 1.30f, 0.0f};
        s.local_rotation = kIdent;
        s.half_extent = {0.05f, 0.18f, 0.05f};
        s.parent = 1;
        s.joint_local = {0.11f, 0.10f, 0.0f};        // torso surf (world 0.11, 1.45)
        s.child_joint_local = {-0.05f, 0.15f, 0.0f}; // arm inner  (world 0.11, 1.45)
        s.cone_limit = 1.6f;
        d.segments.push_back(s);
    }
    // 5: left leg. Top tip TOUCHES the pelvis bottom at world y = 0.90; the hip
    // joint is at world (-0.08, 0.90, 0), on that shared surface.
    {
        RagdollSegment s{};
        s.shape = Shape::Capsule;
        s.mass = 9.0f;
        s.local_position = {-0.08f, 0.59f, 0.0f};
        s.local_rotation = kIdent;
        s.half_extent = {0.07f, 0.24f, 0.07f};
        s.parent = 0;
        s.joint_local = {-0.08f, -0.10f, 0.0f};      // pelvis btm (world -0.08, 0.90)
        s.child_joint_local = {0.0f, 0.31f, 0.0f};   // leg top tip(world -0.08, 0.90)
        s.cone_limit = 1.4f;                          // ~80 deg hip
        d.segments.push_back(s);
    }
    // 6: right leg. Mirror of the left.
    {
        RagdollSegment s{};
        s.shape = Shape::Capsule;
        s.mass = 9.0f;
        s.local_position = {0.08f, 0.59f, 0.0f};
        s.local_rotation = kIdent;
        s.half_extent = {0.07f, 0.24f, 0.07f};
        s.parent = 0;
        s.joint_local = {0.08f, -0.10f, 0.0f};       // right hip  (world 0.08, 0.90)
        s.child_joint_local = {0.0f, 0.31f, 0.0f};   // leg top tip(world 0.08, 0.90)
        s.cone_limit = 1.4f;
        d.segments.push_back(s);
    }
    return d;
}

// ---------------------------------------------------------------------------
// Build / spawn.
// ---------------------------------------------------------------------------
RagdollId build_ragdoll(World& world,
                        const RagdollDesc& desc,
                        const math::Quat& world_rotation,
                        const math::Vec3& world_position,
                        const math::Vec3& initial_velocity,
                        Ragdoll& out) {
    out.clear();
    if (desc.segments.empty())
        return {};

    const usize n = desc.segments.size();
    // Pre-size the live arrays ONCE (no per-segment reallocation churn).
    out.bodies.reserve(n);
    out.joints.joints.reserve(n);  // at most one pin + one cone per non-root

    // First pass: create every body at its world transform and seed velocity.
    for (usize i = 0; i < n; ++i) {
        const RagdollSegment& s = desc.segments[i];
        BodyDesc bd{};
        bd.shape = s.shape;
        bd.mass = s.mass;
        // World transform: rotate the relative position by the ragdoll's
        // rotation, then translate by its position.
        bd.position = math::add(world_position, rotate(world_rotation, s.local_position));
        bd.rotation = compose(world_rotation, s.local_rotation);
        bd.half_extent = s.half_extent;
        bd.friction = s.friction;
        bd.restitution = s.restitution;
        // Self-collision filter (ADR-020): every segment of a rag shares the
        // kRagdollSelfCollideGroup bit and clears it from its mask, so the
        // broadphase rejects all limb-vs-limb pairs (anatomically tight segments
        // no longer fight the joint pins) while limb-vs-world pairs still collide
        // -- the world's default all-ones group contains the bit and its default
        // all-ones mask accepts it, so the rag still settles on the ground. NOTE:
        // because all rags share this one bit, two SEPARATE rags also pass
        // through each other (acceptable for death rags; a game that wants rag-
        // vs-rag collision would give each rag its own group bit instead).
        bd.collision_group = kRagdollSelfCollideGroup;
        bd.collision_mask = ~kRagdollSelfCollideGroup;

        BodyId id = world.create_body(bd);
        out.bodies.push_back(id);
        // Inherit the dying character's momentum: every segment starts with the
        // same linear velocity (set_body_velocity is a no-op on a static body).
        if (id.valid())
            world.set_body_velocity(id, initial_velocity);
    }

    // Second pass: connect each non-root segment to its parent with a
    // ball-socket pin (+ optional rope cone limit). Anchors are body-LOCAL so
    // they track the bodies as they rotate; the solver pins the two anchors
    // coincident (rest_length baked to the spawn separation, which is ~0 here
    // because both anchors name the same world joint point).
    for (usize i = 0; i < n; ++i) {
        const RagdollSegment& s = desc.segments[i];
        if (s.parent == kRagdollRoot)
            continue;
        if (s.parent >= n)
            continue;  // malformed desc: skip rather than index OOB
        const BodyId parent_id = out.bodies[s.parent];
        const BodyId child_id = out.bodies[i];
        if (!parent_id.valid() || !child_id.valid())
            continue;

        // Ball-socket pin: anchor on the parent at joint_local, anchor on the
        // child at child_joint_local. Both name the same world point at spawn,
        // so the pin distance (rest_length) is ~0.
        Joint pin = joint_with_handles(JointKind::BallSocket, parent_id, child_id);
        pin.anchor_a = s.joint_local;
        pin.anchor_b = s.child_joint_local;
        // rest_length is the spawn separation of the two anchors (the projection
        // pins to it). Compute it from the placed bodies so a desc whose anchors
        // are not perfectly coincident still pins to a stable distance.
        const math::Vec3 pa = math::add(world.get_position(parent_id),
                                        rotate(world.get_rotation(parent_id), s.joint_local));
        const math::Vec3 pb = math::add(world.get_position(child_id),
                                        rotate(world.get_rotation(child_id), s.child_joint_local));
        pin.rest_length = math::length(math::sub(pb, pa));
        out.joints.add(pin);

        // Soft cone limit: a Rope between an OFF-AXIS anchor on each body. The
        // anchor sits one bone-radius out along the child's local +X from the
        // joint; when the limb splays past the cone half-angle the two off-axis
        // anchors separate beyond the rope's rest length and the rope pulls them
        // back, catching the limb before it inverts. Slack inside the cone.
        if (s.cone_limit > 0.0f) {
            // Lever arm: distance from the joint to the off-axis anchor on each
            // side. Use a fixed fraction of the child half-height so the cone
            // scales with limb length.
            const f32 lever = std::max(0.05f, s.half_extent.y * 0.5f);
            const math::Vec3 off{lever, 0.0f, 0.0f};  // local +X offset
            Joint cone = joint_with_handles(JointKind::Rope, parent_id, child_id);
            cone.anchor_a = math::add(s.joint_local, off);
            cone.anchor_b = math::add(s.child_joint_local, off);
            // Rope max length: the chord across the cone at the lever radius.
            // For two equal lever arms meeting at the joint, splaying to the
            // half-angle theta separates the off-axis anchors by ~2*lever*sin(theta/2)
            // PLUS their spawn separation. Bake the spawn separation in so the
            // rope is slack at rest and only engages past the limit.
            const math::Vec3 ca = math::add(world.get_position(parent_id),
                                            rotate(world.get_rotation(parent_id), cone.anchor_a));
            const math::Vec3 cb = math::add(world.get_position(child_id),
                                            rotate(world.get_rotation(child_id), cone.anchor_b));
            const f32 rest = math::length(math::sub(cb, ca));
            cone.rest_length = rest + 2.0f * lever * std::sin(s.cone_limit * 0.5f);
            out.joints.add(cone);
        }
    }

    // Stamp a generation-safe id. We derive it from the root body's handle so a
    // torn-down-and-rebuilt ragdoll gets a distinct id (the root body's slot/gen
    // advance on reuse); fall back to a fixed live sentinel if the root is
    // static/invalid so alive() still reports true for a built rag.
    const BodyId root = out.bodies.empty() ? BodyId{} : out.bodies.front();
    out.id = root.valid() ? RagdollId{root.raw} : RagdollId{0x01000000u};
    return out.id;
}

// ---------------------------------------------------------------------------
// Per-frame joint solve. Run after world.step(dt).
// ---------------------------------------------------------------------------
void solve_ragdoll(World& world, const Ragdoll& rag, f32 dt, u32 iterations) {
    if (!rag.alive() || rag.joints.joints.empty() || dt <= 0.0f)
        return;
    // Ragdoll-tuned params: extra position passes so the limb pins settle
    // visibly connected, and the stock split-impulse velocity damping so the rag
    // comes to rest rather than jittering. The TIGHT skeleton (ADR-020) transmits
    // ground-impact load straight through the pins (the old gaps used to absorb a
    // little of it), so we run a couple more position passes to keep the worst
    // transient joint stretch small at the moment of impact. The solver
    // re-resolves the gen-safe BodyId handles each call and skips any joint whose
    // body was destroyed.
    JointSolverParams params{};
    params.position_iterations = 12;
    params.velocity_damping = 0.05f;
    // Velocity-sweep floor. The TIGHT skeleton (ADR-020, gaps removed) rests with
    // several segments AND the world plane all loading the distal pins at once
    // (e.g. a head whose own ground contact fights the neck pin while the rag
    // lies on its side), so we floor the Gauss-Seidel velocity sweeps at 16 to
    // keep the worst transient pin stretch small at impact. Honour a caller
    // asking for MORE, never fewer; pure integer clamp, deterministic.
    const u32 vel_iters = iterations < 16u ? 16u : iterations;
    solve_joints(world, rag.joints, params, dt, vel_iters);
}

// ---------------------------------------------------------------------------
// Teardown. Generation-safe + idempotent.
// ---------------------------------------------------------------------------
void destroy_ragdoll(World& world, Ragdoll& rag) {
    for (BodyId id : rag.bodies) {
        if (id.valid())
            world.destroy_body(id);  // gen-checked: a stale/freed id is a no-op
    }
    rag.clear();
}

}  // namespace psynder::physics
