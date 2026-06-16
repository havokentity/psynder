// SPDX-License-Identifier: MIT

#include <catch2/catch_test_macros.hpp>

#include "audio/Chiptune.h"

#include <algorithm>

using namespace psynder;

TEST_CASE("audio: boot chime exposes a playable chip song", "[audio][chiptune]") {
    const audio::ChipSong& song = audio::boot_chime_song();

    REQUIRE(song.bpm > 0u);
    REQUIRE(song.ticks_per_beat > 0u);
    REQUIRE(song.master_volume > 0.0f);
    REQUIRE(!song.notes.empty());

    u32 end_tick = 0;
    for (const audio::ChipNote& note : song.notes) {
        REQUIRE(note.duration_ticks > 0u);
        end_tick = std::max(end_tick, note.start_tick + note.duration_ticks);
    }
    REQUIRE(end_tick > 0u);

    audio::stop_chiptune();
    REQUIRE_FALSE(audio::chiptune_active());
    audio::play_chiptune(song);
    REQUIRE(audio::chiptune_active());
    audio::stop_chiptune();
    REQUIRE_FALSE(audio::chiptune_active());
}

TEST_CASE("audio: empty chip song does not start playback", "[audio][chiptune]") {
    audio::ChipSong song{};
    audio::stop_chiptune();
    audio::play_chiptune(song);
    REQUIRE_FALSE(audio::chiptune_active());
}
