// SPDX-License-Identifier: MIT
// Psynder — job system synchronous fallback. Lane 04 implements the real
// Chase-Lev work-stealing deque + job graph; this Phase-0 stub runs every
// submitted job synchronously on the caller's thread so engine code that
// submits jobs at least executes correctly.

#include "JobSystem.h"

#include <atomic>
#include <thread>

namespace psynder::jobs {

namespace {
std::atomic<u32> g_next_id{1};
u32 g_workers = 1;
}  // namespace

JobSystem& JobSystem::Get() {
    static JobSystem s;
    return s;
}

void JobSystem::start(u32 worker_count) {
    if (worker_count == 0) {
        worker_count = std::max(1u, std::thread::hardware_concurrency());
    }
    g_workers = worker_count;
}

void JobSystem::stop() {
    g_workers = 1;
}

JobHandle JobSystem::submit(const JobDesc& desc, JobHandle /*dep*/) {
    JobHandle h{ g_next_id.fetch_add(1, std::memory_order_relaxed), 0 };
    if (desc.fn) desc.fn(desc.user);
    return h;
}

void JobSystem::wait(JobHandle /*h*/) {
    // Phase-0 stub: synchronous submit means nothing to wait on.
}

void JobSystem::parallel_for(usize begin, usize end, usize /*grain*/,
                             const std::function<void(usize, usize)>& body) {
    if (begin < end && body) body(begin, end);
}

u32 JobSystem::worker_count() const noexcept   { return g_workers; }
u32 JobSystem::current_worker() const noexcept { return 0; }

}  // namespace psynder::jobs
