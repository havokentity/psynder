// SPDX-License-Identifier: MIT
// Psynder — per-map terrain backend configuration (DESIGN.md §9.2).
//
// The backend is fixed at map load time and cannot change mid-map (the
// LOD pipelines and lightmap layouts differ; see §9.2 "Per-map runtime
// config"). This header exposes a tiny parser for the `terrain_backend`
// key — the map's `.psylevel` header carries it as a string, parsed once
// at scene boot.

#pragma once

#include "world/outdoor/Terrain.h"

#include "core/Types.h"

#include <string_view>

namespace psynder::world::outdoor::detail {

// Parse the value of the per-map `terrain_backend = …` key. Accepts the
// two values defined in DESIGN.md §9.2 + ADR-008. Returns false on an
// unknown / empty value (caller keeps the previous selection or the
// engine default).
inline bool parse_backend(std::string_view value, TerrainBackend& out) noexcept {
    // Trim leading whitespace — the .psylevel parser hands us the raw token
    // and may include indentation. We never see embedded NULs.
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                              value.back() == '\n')) {
        value.remove_suffix(1);
    }

    if (value == "mesh") {
        out = TerrainBackend::PolygonCDLOD;
        return true;
    }
    if (value == "raymarch") {
        out = TerrainBackend::HeightmapRaymarch;
        return true;
    }
    return false;
}

// Stringify back the way `.psylevel` serializes it. Useful for the editor's
// inspector panel and for diagnostic logs.
inline const char* backend_name(TerrainBackend b) noexcept {
    switch (b) {
        case TerrainBackend::PolygonCDLOD:
            return "mesh";
        case TerrainBackend::HeightmapRaymarch:
            return "raymarch";
    }
    return "unknown";
}

}  // namespace psynder::world::outdoor::detail
