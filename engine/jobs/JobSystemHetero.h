// SPDX-License-Identifier: MIT
// Psynder — Wave B sibling header for the JobSystem. Lane 04 owned.
//
// `JobSystem.h` is the FROZEN public surface from Wave A and we keep it
// byte-identical. The Wave-B heterogeneous-pool API (P-core preferring
// latency pool, E-core preferring throughput pool) ships here as free
// functions in `psynder::jobs` — same namespace, additive surface.
//
// Lifecycle:
//   - `JobSystem::start()` (or the lazy start triggered on first submit)
//     stands up the unified worker pool used by `submit/parallel_for`.
//     The first call to ANY of the hetero APIs below additionally stands
//     up two side pools sized from the detected P/E core counts.
//   - On homogeneous boxes (no P/E split detected) BOTH `submit_latency`
//     and `submit_throughput` route to the unified pool so callers do not
//     need to special-case anything.
//   - `JobSystem::stop()` tears down the hetero pools too.
//
// Semantics:
//   - `submit_latency(desc, dep)`: prefers P-cores. Use for raster, RT,
//     vertex transform, denoise — anything in the frame-time critical
//     path.
//   - `submit_throughput(desc, dep)`: prefers E-cores. Use for asset
//     decompression, audio mixing, ambient AI ticks — work that wants to
//     keep going but doesn't hold up the next frame.
//   - Dependencies are honored across pools: a `submit_latency` job can
//     depend on a `submit_throughput` job and vice versa. The completion
//     path routes the child onto its registered class's pool.
//
// Diagnostics (homogeneous-fallback friendly):
//   - `hetero_latency_workers()` / `hetero_throughput_workers()` return
//     the per-pool worker counts. On homogeneous boxes both return 0
//     because the unified pool serves both classes.
//   - `hetero_is_active()` is true when the box really did report a P/E
//     split and we stood up the side pools.

#pragma once

#include "JobSystem.h"

namespace psynder::jobs {

// Submit a job to the latency-preferring (P-core) pool. Behaves exactly
// like `JobSystem::submit` otherwise — same JobDesc, same dependency
// semantics, returns a JobHandle waitable via `JobSystem::wait`.
JobHandle submit_latency(const JobDesc& desc, JobHandle dep = {}) noexcept;

// Submit a job to the throughput-preferring (E-core) pool.
JobHandle submit_throughput(const JobDesc& desc, JobHandle dep = {}) noexcept;

// Diagnostics — see header banner.
bool hetero_is_active() noexcept;
u32 hetero_latency_workers() noexcept;
u32 hetero_throughput_workers() noexcept;

// Hetero P-core / E-core counts as detected by the platform's topology
// helper (sysctl perflevels on macOS, GetLogicalProcessorInformationEx on
// Win, /sys cpufreq on Linux). Returns 0/0 on homogeneous hosts. Exposed
// for tests and bench harnesses.
struct HeteroCounts {
    u32 p_cores;
    u32 e_cores;
    u32 total;
};
HeteroCounts hetero_detected_counts() noexcept;

}  // namespace psynder::jobs
