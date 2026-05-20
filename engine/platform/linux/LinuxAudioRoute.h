// SPDX-License-Identifier: MIT
// Psynder — Lane 22: audio-backend selection table.
//
// Host-portable header (no Wayland / ALSA / PipeWire deps) so the unit test
// at tests/unit/platform_linux_audio_route.cpp can verify the ranking on a
// Mac orchestrator host. The runtime PipeWire / ALSA wiring lives in
// LinuxAudio.cpp behind PSYNDER_PLATFORM_LINUX.
//
// DESIGN.md §11.2: PipeWire first, ALSA fallback. The order encoded here
// is the *preference* order — the platform code probes each entry top to
// bottom, taking the first that successfully opens the device.

#pragma once

#include <cstdint>

namespace psynder::platform::linux_impl {

// Audio-backend rank. Lower numeric value = preferred. The ranking is
// referenced in two places:
//   - LinuxPlatform.cpp's device-opener loop (Wave B)
//   - psynder_audio::backend_init when desc.backend == Auto (lane 12, via
//     the strong overrides this lane links into the audio target).
enum class AudioBackend : std::uint8_t {
    PipeWire = 0,
    ALSA = 1,
    None = 2,  // sentinel — no usable backend
};

// Strict-weak ordering used by tests + the device-opener loop.
constexpr bool backend_preferred_over(AudioBackend a, AudioBackend b) noexcept {
    return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
}

// Pick the better of two backends. Used by the runtime probe loop and the
// host-only test. constexpr so the compiler can fold it into the dispatch
// table when both arguments are constant.
constexpr AudioBackend prefer(AudioBackend a, AudioBackend b) noexcept {
    return backend_preferred_over(a, b) ? a : b;
}

// Sentinel byte exposed for the host-portable unit test. Defined inline so
// every TU that includes this header has the symbol — works on both Linux
// (where the audio TUs link this lane in) and Mac (where the lane is
// header-only-visible because most TUs are guarded out by
// PSYNDER_PLATFORM_LINUX).
inline constexpr char kAudioRouteSentinel = 'P';  // P for PipeWire-first

}  // namespace psynder::platform::linux_impl
