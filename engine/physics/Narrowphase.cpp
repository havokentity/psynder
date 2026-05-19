// SPDX-License-Identifier: MIT
// Psynder physics — narrowphase TU. Thin forwarder: every algorithm lives in
// `internal/Kernels.h` so unit tests can exercise it without linking the
// physics static lib (mirrors the audio lane's MixerCore.h pattern).

#include "Narrowphase.h"
#include "internal/Kernels.h"

namespace psynder::physics::detail {

bool collide_sphere_sphere(math::Vec3 ca, f32 ra,
                           math::Vec3 cb, f32 rb,
                           Contact& out) noexcept {
    return kernels::kernel_sphere_sphere(ca, ra, cb, rb, out);
}

bool collide_sphere_capsule(math::Vec3 cs, f32 rs,
                            math::Vec3 cc, math::Quat qc,
                            f32 rc, f32 hc,
                            Contact& out) noexcept {
    return kernels::kernel_sphere_capsule(cs, rs, cc, qc, rc, hc, out);
}

bool collide_capsule_capsule(math::Vec3 ca, math::Quat qa, f32 ra, f32 ha,
                             math::Vec3 cb, math::Quat qb, f32 rb, f32 hb,
                             Contact& out) noexcept {
    return kernels::kernel_capsule_capsule(ca, qa, ra, ha, cb, qb, rb, hb, out);
}

bool collide_aabb_aabb(math::Aabb a, math::Aabb b, Contact& out) noexcept {
    return kernels::kernel_aabb_aabb(a, b, out);
}

bool collide_gjk_epa(const GjkSupport& a, const GjkSupport& b,
                     Contact& out) noexcept {
    return kernels::kernel_gjk_epa(a, b, out);
}

bool collide_pair(const Body& a, const Body& b, Contact& out) noexcept {
    return kernels::kernel_collide_pair(a, b, out);
}

}  // namespace psynder::physics::detail
