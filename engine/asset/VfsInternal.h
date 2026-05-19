// SPDX-License-Identifier: MIT
// Psynder — internal VFS test / diagnostic surface.
//
// Public API in Vfs.h is frozen. The hooks here let in-lane tests reset
// the singleton between cases and inspect mount internals. NOT part of
// any cross-lane contract; safe to evolve.

#pragma once

#include "Vfs.h"

namespace psynder::asset::internal {

// Tear down every mount, drop the decompress cache, stop the watcher
// thread. Used by `tests/unit/asset_*.cpp` to ensure case isolation.
void reset_for_tests();

// True if the watcher poll thread is running (lazy-started on first
// `Vfs::watch()` call in dev builds).
bool watcher_thread_running();

// Force one watcher poll cycle synchronously. Tests call this so they
// don't have to wait for the background poll interval.
void poll_watchers_now();

}  // namespace psynder::asset::internal
