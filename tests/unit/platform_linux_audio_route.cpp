// SPDX-License-Identifier: MIT
// Psynder — Lane 22 audio-backend routing test.
//
// Host-portable: the LinuxAudioRoute.h header is dependency-free (no
// Wayland / PipeWire / ALSA references) so it compiles on the Mac
// orchestrator that drives this lane's Wave-B PR. The runtime backends
// themselves live in LinuxAudio.cpp and are PSYNDER_PLATFORM_LINUX-guarded;
// this test verifies only the ranking/dispatch policy.
//
// Covers (one TEST_CASE per Wave-B test budget):
//   - PipeWire is preferred over ALSA (DESIGN.md §11.2).
//   - The kAudioRouteSentinel inline-constexpr symbol resolves on the
//     host where psynder_unit links — that's the cheap way of proving
//     the platform-linux lane's header was actually pulled into the test
//     translation-unit set on Mac (the inline-constexpr address is
//     well-defined under C++23 odr-rules for header-defined constants).

#include "platform/linux/LinuxAudioRoute.h"

#include <catch2/catch_test_macros.hpp>

namespace pli = psynder::platform::linux_impl;

TEST_CASE(
    "platform_linux: audio backend ranking prefers PipeWire then ALSA, "
    "and the route-sentinel symbol resolves on every host",
    "[platform-linux][audio]") {
    // ─── Ranking ────────────────────────────────────────────────────────
    // DESIGN.md §11.2 — PipeWire first, ALSA fallback, None last.
    STATIC_REQUIRE(pli::backend_preferred_over(pli::AudioBackend::PipeWire, pli::AudioBackend::ALSA));
    STATIC_REQUIRE(pli::backend_preferred_over(pli::AudioBackend::ALSA, pli::AudioBackend::None));
    STATIC_REQUIRE_FALSE(
        pli::backend_preferred_over(pli::AudioBackend::ALSA, pli::AudioBackend::PipeWire));

    // prefer() folds the ordering — pairwise checks for each combination.
    STATIC_REQUIRE(pli::prefer(pli::AudioBackend::PipeWire, pli::AudioBackend::ALSA) ==
                   pli::AudioBackend::PipeWire);
    STATIC_REQUIRE(pli::prefer(pli::AudioBackend::ALSA, pli::AudioBackend::PipeWire) ==
                   pli::AudioBackend::PipeWire);
    STATIC_REQUIRE(pli::prefer(pli::AudioBackend::None, pli::AudioBackend::ALSA) ==
                   pli::AudioBackend::ALSA);

    // Runtime checks too, so the assertion message shows up in test output
    // when somebody flips the enum order without re-reading DESIGN.md.
    REQUIRE(pli::prefer(pli::AudioBackend::PipeWire, pli::AudioBackend::ALSA) ==
            pli::AudioBackend::PipeWire);

    // ─── Sentinel ───────────────────────────────────────────────────────
    // The inline-constexpr `kAudioRouteSentinel` is visible in every TU
    // that includes the header — that's the host-portability contract for
    // this lane's test. Take its address (forces ODR-use) and verify the
    // value the header documents (`P` for PipeWire-first). On Mac the
    // platform-linux library's TUs are all #ifdef'd out, but the header
    // alone gives us a symbol we can peer at.
    const char* p = &pli::kAudioRouteSentinel;
    REQUIRE(p != nullptr);
    REQUIRE(*p == 'P');
}
