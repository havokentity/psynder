// SPDX-License-Identifier: MIT
// Psynder — Wave B: heterogeneous-core (P/E) detection + per-worker hints.
// Lane 04 owned.
//
// DESIGN.md §2.4: "Heterogeneous cores: on Apple Silicon and Intel Core
// Ultra we group P-cores and E-cores into separate worker pools;
// latency-sensitive jobs (raster, raytrace) prefer P-cores, throughput
// jobs (asset decompress, audio) are happy on E-cores."
//
// We detect P-core / E-core counts at startup and apply an OS-specific
// affinity *hint* per worker thread:
//
//   - macOS (Apple Silicon w/ AMP): use `pthread_set_qos_class_self_np`
//     on each worker. QOS_CLASS_USER_INTERACTIVE prefers P-cores;
//     QOS_CLASS_BACKGROUND prefers E-cores. The kernel's CLPC scheduler
//     honors QoS as the primary cue for AMP scheduling; we do not need
//     raw affinity masks (macOS doesn't expose them anyway). Core count
//     comes from sysctl `hw.perflevel0.physicalcpu` (P cluster) and
//     `hw.perflevel1.physicalcpu` (E cluster).
//   - Win32 (Intel Core Ultra, Snapdragon X): use `SetThreadInformation`
//     with `ThreadPowerThrottling` to opt-*out* of E-core throttling for
//     latency workers and opt-*in* for throughput workers. Core counts
//     via `GetLogicalProcessorInformationEx(RelationProcessorCore)` and
//     the EfficiencyClass field (0 = E, ≥ 1 = P on hybrid SKUs).
//   - Linux (Intel Alder Lake+, ARM big.LITTLE): bucket cores by
//     /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq.
//
// If the host is homogeneous, `p_cores == e_cores == 0` is returned and
// the caller falls back to the single unified pool. This preserves
// Wave A's behavior on those boxes.

#pragma once

#include "core/Types.h"

namespace psynder::jobs::detail {

struct HeteroTopology {
    u32 p_cores = 0;   // performance / "big" cores, 0 if homogeneous
    u32 e_cores = 0;   // efficiency / "LITTLE" cores, 0 if homogeneous
    u32 total   = 0;   // total physical cores reported by detection

    bool is_hetero() const noexcept {
        return p_cores > 0 && e_cores > 0;
    }
};

// Detect once per process. Idempotent; cheap to call.
HeteroTopology detect_hetero_topology() noexcept;

// Tag a worker thread for the latency (P-core) or throughput (E-core)
// pool. Call from inside the worker thread. Failure to apply is non-fatal.
enum class WorkerClass : u8 {
    Latency,    // prefers P-cores
    Throughput, // prefers E-cores
};

void apply_worker_class_hint(WorkerClass cls) noexcept;

}  // namespace psynder::jobs::detail
