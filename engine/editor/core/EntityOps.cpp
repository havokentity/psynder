// SPDX-License-Identifier: MIT
// Psynder — non-inline anchors for editor::ops (referenced by the IPC
// dispatcher in lane 19 and by samples). The functions themselves are
// inline in EntityOps.h; this TU keeps a stable PDB / dSYM record.

#include "EntityOps.h"

namespace psynder::editor::ops {

// Force at least one out-of-line symbol so the linker keeps this TU
// in the static archive for IPC's runtime symbol lookup.
void anchor() {}

}  // namespace psynder::editor::ops
