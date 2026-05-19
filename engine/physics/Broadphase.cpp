// SPDX-License-Identifier: MIT
// Psynder physics — sweep-and-prune broadphase TU.
//
// Orchestration: dispatch 3 axis-pass jobs in parallel via the job system,
// then 3-way intersect the sorted unique pair lists. The per-axis kernel
// lives header-only in `internal/Kernels.h`.

#include "Broadphase.h"
#include "internal/Kernels.h"

#include "jobs/JobSystem.h"

#include <algorithm>
#include <vector>

namespace psynder::physics::detail {

namespace {

struct AxisJobCtx {
    const AabbEntry*   aabbs;
    usize              count;
    u32                axis;
    std::vector<CandidatePair>* out_pairs;
};

}  // anonymous namespace

void broadphase_sap(std::vector<AabbEntry>& aabbs,
                    std::vector<CandidatePair>& out_pairs) {
    out_pairs.clear();
    if (aabbs.size() < 2) return;

    std::vector<CandidatePair> per_axis[3];
    AxisJobCtx ctxs[3] = {
        {aabbs.data(), aabbs.size(), 0, &per_axis[0]},
        {aabbs.data(), aabbs.size(), 1, &per_axis[1]},
        {aabbs.data(), aabbs.size(), 2, &per_axis[2]},
    };

    auto job_fn = [](void* user) noexcept {
        auto* ctx = static_cast<AxisJobCtx*>(user);
        kernels::kernel_sap_axis(ctx->aabbs, ctx->count, ctx->axis, *ctx->out_pairs);
    };

    auto& js = jobs::JobSystem::Get();
    jobs::JobHandle hs[3];
    for (u32 i = 0; i < 3; ++i) {
        hs[i] = js.submit(jobs::JobDesc{job_fn, &ctxs[i], "phys-sap-axis", 1});
    }
    for (u32 i = 0; i < 3; ++i) js.wait(hs[i]);

    // 3-way intersection of sorted, unique pair lists. Use the
    // max-then-advance idiom: at each step compute the max of the three
    // heads, then advance every head that's strictly less than max. When
    // all three heads equal the max we have a triple match — emit and step.
    const auto& a = per_axis[0];
    const auto& b = per_axis[1];
    const auto& c = per_axis[2];
    usize ia = 0, ib = 0, ic = 0;
    auto less = [](CandidatePair x, CandidatePair y) {
        if (x.a != y.a) return x.a < y.a;
        return x.b < y.b;
    };
    auto pair_max = [&](CandidatePair x, CandidatePair y, CandidatePair z) {
        CandidatePair m = less(x, y) ? y : x;
        return less(m, z) ? z : m;
    };
    while (ia < a.size() && ib < b.size() && ic < c.size()) {
        CandidatePair m = pair_max(a[ia], b[ib], c[ic]);
        bool ea = !less(a[ia], m) && !less(m, a[ia]);
        bool eb = !less(b[ib], m) && !less(m, b[ib]);
        bool ec = !less(c[ic], m) && !less(m, c[ic]);
        if (ea && eb && ec) {
            out_pairs.push_back(m);
            ++ia; ++ib; ++ic;
        } else {
            if (!ea) ++ia;
            if (!eb) ++ib;
            if (!ec) ++ic;
        }
    }
}

}  // namespace psynder::physics::detail
