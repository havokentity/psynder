// SPDX-License-Identifier: MIT
// Psynder — diag tiers. Ported from dmonte (pt::diag → psynder::diag).
//
// PSY_DIAG_TIER{1,2,3} gates expensive diagnostic lines on the runtime cvar
// `r_diagnostic_level` (registered by Engine::Init). Hot path when off is a
// single relaxed atomic load + compare; format-args evaluation is short-
// circuited.

#pragma once

#include "Log.h"

#include <atomic>
#include <fmt/format.h>

namespace psynder::diag {

extern std::atomic<int> g_diag_level;

inline bool tier_enabled(int tier) noexcept {
    return g_diag_level.load(std::memory_order_relaxed) >= tier;
}

}  // namespace psynder::diag

#define PSY_DIAG_TIER1(category, ...)                                                \
    do {                                                                             \
        if (::psynder::diag::tier_enabled(1)) {                                      \
            ::psynder::log::info("[{}] {}", (category), ::fmt::format(__VA_ARGS__)); \
        }                                                                            \
    } while (0)

#define PSY_DIAG_TIER2(category, ...)                                                \
    do {                                                                             \
        if (::psynder::diag::tier_enabled(2)) {                                      \
            ::psynder::log::info("[{}] {}", (category), ::fmt::format(__VA_ARGS__)); \
        }                                                                            \
    } while (0)

#define PSY_DIAG_TIER3(category, ...)                                                \
    do {                                                                             \
        if (::psynder::diag::tier_enabled(3)) {                                      \
            ::psynder::log::info("[{}] {}", (category), ::fmt::format(__VA_ARGS__)); \
        }                                                                            \
    } while (0)
