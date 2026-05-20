// SPDX-License-Identifier: MIT
// Psynder — lane 12 unit test: voice pool acquire / release invariants.
//
// We pull the algorithm directly from `audio/internal/MixerCore.h` (header-
// only) so the test compiles without linking `psynder_audio` — the shared
// `tests/unit/CMakeLists.txt` is owned by lane 25 and currently does not
// include the audio static lib in its link list.

#include "audio/internal/MixerCore.h"

#include <catch2/catch_test_macros.hpp>

using psynder::audio::detail::kMaxVoices;
using psynder::audio::detail::VoicePool;
using psynder::math::Vec3;

TEST_CASE("audio: voice pool acquires up to kMaxVoices then refuses more", "[audio][voices]") {
    VoicePool pool;
    REQUIRE(pool.active_count() == 0u);

    psynder::u32 ids[kMaxVoices];
    for (psynder::u32 i = 0; i < kMaxVoices; ++i) {
        ids[i] = pool.acquire(/*clip*/ 1u, Vec3{0, 0, 1.0f}, /*vol*/ 0.5f);
        REQUIRE(ids[i] != 0u);
    }
    REQUIRE(pool.active_count() == kMaxVoices);

    // 33rd acquire fails (returns 0 = invalid).
    const psynder::u32 overflow = pool.acquire(1u, Vec3{0, 0, 1.0f}, 0.5f);
    REQUIRE(overflow == 0u);
    REQUIRE(pool.active_count() == kMaxVoices);

    // Release one then re-acquire — generation must change so the old id
    // can no longer be released a second time.
    const psynder::u32 reused_idx = VoicePool::unpack_index(ids[5]);
    REQUIRE(pool.release(ids[5]));
    REQUIRE(pool.active_count() == kMaxVoices - 1u);

    const psynder::u32 new_id = pool.acquire(2u, Vec3{1, 0, 1.0f}, 0.7f);
    REQUIRE(new_id != 0u);
    REQUIRE(VoicePool::unpack_index(new_id) == reused_idx);
    REQUIRE(VoicePool::unpack_gen(new_id) != VoicePool::unpack_gen(ids[5]));

    // Stale id should no longer release.
    REQUIRE_FALSE(pool.release(ids[5]));
    REQUIRE(pool.active_count() == kMaxVoices);

    pool.clear();
    REQUIRE(pool.active_count() == 0u);
}

TEST_CASE("audio: voice pool release of invalid id is a no-op", "[audio][voices]") {
    VoicePool pool;
    REQUIRE_FALSE(pool.release(0u));
    REQUIRE_FALSE(pool.release(0xDEADBEEFu));
    REQUIRE(pool.active_count() == 0u);
}
