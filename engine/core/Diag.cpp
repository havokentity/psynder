// SPDX-License-Identifier: MIT
// Psynder — diag impl. The level cvar is registered by the engine startup
// once the console exists; we initialize at tier 1 so state-transition events
// emit out of the box.

#include "Diag.h"

namespace psynder::diag {

std::atomic<int> g_diag_level{1};

}  // namespace psynder::diag
