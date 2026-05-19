// SPDX-License-Identifier: MIT
// Psynder — job system public API. Lane 04 owns the Chase-Lev deque + job
// graph + fiber impl. Phase-0 scaffold provides a header surface other
// lanes code against.

#pragma once

#include "core/Types.h"

#include <atomic>
#include <functional>

namespace psynder::jobs {

using JobFn = void (*)(void* user) noexcept;

struct JobHandle {
    u32 id  = 0;
    u32 gen = 0;
    constexpr bool valid() const noexcept { return id != 0; }
};

struct JobDesc {
    JobFn       fn       = nullptr;
    void*       user     = nullptr;
    const char* name     = "job";
    u32         priority = 0;   // 0 = normal, higher = sooner
};

class JobSystem {
public:
    static JobSystem& Get();

    // Lifecycle
    void start(u32 worker_count = 0);  // 0 = autodetect physical cores
    void stop();

    // Submit one job; returns a handle for join.
    JobHandle submit(const JobDesc& desc, JobHandle dep = {});

    // Block until a specific handle (and its transitive deps) completes.
    void wait(JobHandle h);

    // Parallel-for: split [begin, end) into chunks and run on the pool.
    // Synchronous: returns after all chunks complete.
    void parallel_for(usize begin, usize end, usize grain,
                      const std::function<void(usize, usize)>& body);

    u32  worker_count() const noexcept;
    u32  current_worker() const noexcept;  // 0..worker_count()-1; main = ~0u

private:
    JobSystem() = default;
};

}  // namespace psynder::jobs
