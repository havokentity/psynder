// SPDX-License-Identifier: MIT
// Psynder — spatial-index dispatcher. Wave A only declares the routing
// surface; the SAP / BVH8 / hashed-grid backends slip to Wave B.

#include "Spatial.h"

namespace psynder::scene::detail {

// Wave-A returns nullptr — query / insert / update / remove become no-ops
// inside the World until Wave B fills them in. The interfaces are declared
// in Spatial.h so dependent lanes can compile against the symbol set.
ISpatialIndex* sap_backend() noexcept   { return nullptr; }
ISpatialIndex* bvh_backend() noexcept   { return nullptr; }
ISpatialIndex* grid_backend() noexcept  { return nullptr; }

ISpatialIndex* resolve(SpatialBackend backend) noexcept {
    switch (backend) {
        case SpatialBackend::SweepAndPrune: return sap_backend();
        case SpatialBackend::Bvh:           return bvh_backend();
        case SpatialBackend::HashedGrid:    return grid_backend();
        case SpatialBackend::None:
        default:                            return nullptr;
    }
}

}  // namespace psynder::scene::detail
