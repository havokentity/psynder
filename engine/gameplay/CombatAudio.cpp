// SPDX-License-Identifier: MIT
// Psynder -- M-COMBAT combat-audio side-channel implementation. See
// CombatAudio.h. Every function is a pure side-channel: it appends AudioEvents
// and (at most) READS the registry for positions. No combat numeric is touched.

#include "gameplay/CombatAudio.h"

#include "scene/EcsRegistry.h"

namespace psynder::gameplay {

f32 default_combat_volume(CombatSoundId sound) noexcept {
    switch (sound) {
        case CombatSoundId::Gunshot:
            return 0.9f;  // the discharge is the loudest cue.
        case CombatSoundId::Impact:
            return 0.55f;  // the strike is quieter than the shot.
        case CombatSoundId::Death:
            return 0.75f;  // mid -- audible but not overpowering.
    }
    return 0.75f;
}

void emit_fire(CombatAudioQueue& queue, math::Vec3 shooter_origin, bool fired) {
    if (!fired)
        return;  // refused shot (cooldown / no ammo): no cue, no double-emit.
    queue.push(CombatSoundId::Gunshot, shooter_origin,
               default_combat_volume(CombatSoundId::Gunshot));
}

void emit_hit(CombatAudioQueue& queue,
              math::Vec3 fire_origin,
              math::Vec3 fire_dir,
              const HitResult& hit) {
    if (!hit.hit)
        return;  // a miss makes no impact sound.
    // Reconstruct the hit point from the ray. Normalize dir so distance is a
    // true world length (fire_weapon's HitResult.distance is along the unit ray).
    const f32 len = math::length(fire_dir);
    const math::Vec3 ndir = (len > 0.0f) ? math::mul(fire_dir, 1.0f / len) : fire_dir;
    const math::Vec3 point = math::add(fire_origin, math::mul(ndir, hit.distance));
    queue.push(CombatSoundId::Impact, point, default_combat_volume(CombatSoundId::Impact));
}

void emit_shot(CombatAudioQueue& queue,
               math::Vec3 fire_origin,
               math::Vec3 fire_dir,
               bool fired,
               const HitResult& hit) {
    emit_fire(queue, fire_origin, fired);
    // Only sound the impact if the shot actually went out this call. A hit can
    // only exist for a fired shot, but gate on `fired` defensively so a stale
    // HitResult passed with fired=false never sneaks an impact through.
    if (fired)
        emit_hit(queue, fire_origin, fire_dir, hit);
}

void emit_death(CombatAudioQueue& queue,
                scene::EcsRegistry& registry,
                Entity corpse,
                math::Vec3 fallback_pos) {
    math::Vec3 pos = fallback_pos;
    (void)entity_position(registry, corpse, pos);  // overwrites pos on success
    queue.push(CombatSoundId::Death, pos, default_combat_volume(CombatSoundId::Death));
}

void emit_deaths(CombatAudioQueue& queue,
                 scene::EcsRegistry& registry,
                 const CombatContext& ctx) {
    for (const Entity corpse : ctx.deaths)
        emit_death(queue, registry, corpse);
}

}  // namespace psynder::gameplay
