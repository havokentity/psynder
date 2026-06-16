// SPDX-License-Identifier: MIT
// Psynder -- M-COMBAT combat-audio side-channel (ADR-024). Additive layer that
// turns combat EVENTS into spatial AudioEvents WITHOUT coupling the gameplay
// module to the audio mixer. Gameplay stays "scene/physics/math only": this
// header names sounds with a tiny enum and emits POD AudioEvents into a
// pre-reserved queue; a host/demo drains the queue into audio::Engine::play.
//
// CRITICAL INVARIANT: audio is a PURE SIDE-CHANNEL. Nothing here mutates the
// ECS, the CombatContext damage/death lists, weapon cooldowns, ammo, or any
// combat numeric. Emission reads combat results and appends events -- combat
// outcomes are byte-identical whether or not these helpers are ever called.

#pragma once

#include "core/Types.h"
#include "gameplay/CombatSystems.h"
#include "math/Math.h"
#include "scene/SceneEcs.h"

#include <vector>

namespace psynder::gameplay {

using ::psynder::f32;
using ::psynder::u8;
using ::psynder::u32;
using ::psynder::usize;

// Which combat cue a queued event names. Mirrors audio::CombatSound by ordinal
// (Gunshot=0, Impact=1, Death=2) so a host maps it to a ClipId with a single
// table lookup; declared here (not pulled from the audio header) so the
// gameplay lib never links audio.
enum class CombatSoundId : u8 {
    Gunshot = 0,
    Impact = 1,
    Death = 2,
};

// A queued, spatialized combat cue. Plain POD: a host plays it via
//   engine.play(table.at(static_cast<audio::CombatSound>(ev.sound)),
//               ev.position, ev.volume);
struct AudioEvent {
    CombatSoundId sound = CombatSoundId::Gunshot;
    u8 _pad[3] = {};
    math::Vec3 position{0.0f, 0.0f, 0.0f};
    f32 volume = 1.0f;
};

// Pre-reserved, alloc-free event queue. Reuse one per combat world: reserve()
// at load, begin_tick() at the top of a frame (retains capacity), then let the
// combat-audio emit helpers append. Mirrors CombatContext's lifecycle so the
// host can clear damage + audio in lockstep.
struct CombatAudioQueue {
    std::vector<AudioEvent> events;

    void reserve(usize max_events) { events.reserve(max_events); }
    void begin_tick() noexcept { events.clear(); }  // retains capacity
    [[nodiscard]] usize size() const noexcept { return events.size(); }
    [[nodiscard]] bool empty() const noexcept { return events.empty(); }

    void push(CombatSoundId sound, math::Vec3 position, f32 volume) {
        AudioEvent ev{};
        ev.sound = sound;
        ev.position = position;
        ev.volume = (volume > 0.0f) ? volume : 0.0f;
        events.push_back(ev);
    }
};

// Default per-cue volumes (sane mix: the shot is loudest, the impact quieter,
// the death cue mid). Pure constant lookup.
[[nodiscard]] f32 default_combat_volume(CombatSoundId sound) noexcept;

// --- Emission helpers -------------------------------------------------------
// Each appends ONE event derived from a combat result. They read the registry
// for positions only (const) and never mutate combat state.

// On a successful weapon FIRE, emit the gunshot at the shooter's muzzle origin.
// `fired` is the gate (pass fire_weapon()'s success, e.g. ammo consumed): a
// refused shot (cooldown / no ammo) emits nothing, so there is no double-emit.
void emit_fire(CombatAudioQueue& queue, math::Vec3 shooter_origin, bool fired);

// On a hitscan HIT, emit the impact at the struck point. Derives the hit point
// as origin + dir*distance from the HitResult; emits nothing on a miss.
void emit_hit(CombatAudioQueue& queue,
              math::Vec3 fire_origin,
              math::Vec3 fire_dir,
              const HitResult& hit);

// Convenience: emit BOTH the gunshot and (on a hit) the impact for one shot.
// `fired` must be the fire_weapon success gate; `hit` is its returned HitResult.
void emit_shot(CombatAudioQueue& queue,
               math::Vec3 fire_origin,
               math::Vec3 fire_dir,
               bool fired,
               const HitResult& hit);

// On a DEATH, emit the death cue at the corpse position. Reads the entity's
// TransformComponent translation; if the entity has already been despawned and
// has no transform, falls back to `fallback_pos`. Call once per ctx.deaths
// entry (drained BEFORE resolve_deaths despawns them, or pass a captured pos).
void emit_death(CombatAudioQueue& queue,
                scene::EcsRegistry& registry,
                Entity corpse,
                math::Vec3 fallback_pos = math::Vec3{0.0f, 0.0f, 0.0f});

// Drain every entry in ctx.deaths into death cues. Reads transforms only; the
// caller must invoke this BEFORE resolve_deaths(despawn=true) destroys the
// corpses (so positions are still live). Pure side-channel.
void emit_deaths(CombatAudioQueue& queue,
                 scene::EcsRegistry& registry,
                 const CombatContext& ctx);

}  // namespace psynder::gameplay
