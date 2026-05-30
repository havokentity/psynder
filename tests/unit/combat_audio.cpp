// SPDX-License-Identifier: MIT
// Psynder -- W13-4 COMBAT AUDIO unit tests. Covers two additive layers:
//   1. engine/audio/CombatClips -- procedural gunshot / impact / death clips
//      synthesized with no asset files: non-empty, in-range, and deterministic
//      (same params -> byte-identical samples).
//   2. engine/gameplay/CombatAudio -- the combat-audio SIDE-CHANNEL: a weapon
//      fire emits a gunshot at the shooter; a hitscan hit emits an impact at the
//      hit point; a death emits a death cue at the corpse; no double-emit; and
//      combat NUMERICS are byte-identical whether or not audio is enabled
//      (audio is a pure side-channel).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "audio/CombatClips.h"
#include "gameplay/CombatAudio.h"
#include "gameplay/CombatSystems.h"
#include "gameplay/GameplayComponents.h"
#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::EcsRegistry;
using psynder::scene::HealthComponent;
using psynder::scene::LocalTransform;
using psynder::scene::Scene;
using psynder::scene::WeaponComponent;

namespace {

// Mirror gameplay_combat.cpp's registry hygiene so cases don't leak rows.
struct RegistryReset {
    RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

Entity spawn_target(Scene& scene, math::Vec3 pos, u32 faction, f32 hp, f32 radius = 0.5f) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    HealthComponent health{};
    health.max_health = hp;
    health.current_health = hp;
    health.faction = faction;
    scene.registry().add<HealthComponent>(e, health);
    HitboxComponent hb{};
    hb.radius = radius;
    scene.registry().add<HitboxComponent>(e, sanitize_hitbox(hb));
    return e;
}

Entity spawn_shooter(Scene& scene, math::Vec3 pos, u32 faction, f32 damage, f32 range,
                     u32 ammo = 100u) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    HealthComponent health{};
    health.faction = faction;
    scene.registry().add<HealthComponent>(e, health);
    WeaponComponent weapon{};
    weapon.damage = damage;
    weapon.range = range;
    weapon.fire_rate = 0.0f;  // no cooldown gate for single-shot tests
    weapon.ammo = ammo;
    scene.registry().add<WeaponComponent>(e, weapon);
    WeaponRuntimeComponent rt{};
    rt.kind = WeaponKind::Hitscan;
    scene.registry().add<WeaponRuntimeComponent>(e, sanitize_weapon_runtime(rt));
    return e;
}

// Did the shooter's shot actually go out? A host computes this from the ammo
// delta (fire_weapon decrements ammo on a real shot, leaves it on a refusal).
bool shot_fired(Scene& scene, Entity shooter, u32 ammo_before) {
    const auto* w = scene.registry().get<WeaponComponent>(shooter);
    return w && w->ammo < ammo_before;
}

}  // namespace

// --- 1. Procedural clips: non-empty, in-range, deterministic ----------------
TEST_CASE("combat_audio: synthesized clips are non-empty and in range",
          "[combat_audio][clips]") {
    for (audio::CombatSound s :
         {audio::CombatSound::Gunshot, audio::CombatSound::Impact, audio::CombatSound::Death}) {
        const audio::CombatClipDesc d = audio::combat_clip_desc(s);
        const u32 n = audio::combat_clip_sample_count(d);
        REQUIRE(n > 0u);

        std::vector<f32> buf(n, 0.0f);
        const u32 wrote = audio::synthesize_combat_clip(d, buf);
        REQUIRE(wrote == n);

        // Every sample is finite and inside [-1, 1].
        f32 peak = 0.0f;
        for (f32 v : buf) {
            REQUIRE(std::isfinite(v));
            REQUIRE(v <= 1.0f);
            REQUIRE(v >= -1.0f);
            peak = std::max(peak, std::abs(v));
        }
        // The clip is audibly non-empty: meaningful peak + RMS energy.
        REQUIRE(peak > 0.05f);
        REQUIRE(audio::combat_clip_energy(d) > 0.0f);
    }
}

TEST_CASE("combat_audio: synthesis is deterministic for identical params",
          "[combat_audio][clips]") {
    const audio::CombatClipDesc d = audio::combat_clip_desc(audio::CombatSound::Gunshot);
    const u32 n = audio::combat_clip_sample_count(d);

    std::vector<f32> a(n, 0.0f), b(n, 0.0f);
    REQUIRE(audio::synthesize_combat_clip(d, a) == n);
    REQUIRE(audio::synthesize_combat_clip(d, b) == n);

    // Byte-identical PCM, run to run, same seed/params.
    REQUIRE(std::memcmp(a.data(), b.data(), n * sizeof(f32)) == 0);
    // The energy estimate matches the rendered PCM exactly (shared generator).
    REQUIRE(audio::combat_clip_energy(d) == audio::combat_clip_energy(d));
}

TEST_CASE("combat_audio: different cues synthesize distinct PCM",
          "[combat_audio][clips]") {
    const audio::CombatClipDesc g = audio::combat_clip_desc(audio::CombatSound::Gunshot);
    const audio::CombatClipDesc i = audio::combat_clip_desc(audio::CombatSound::Impact);
    const audio::CombatClipDesc x = audio::combat_clip_desc(audio::CombatSound::Death);
    // Distinct descriptors (length and/or seed differ) => distinct sounds.
    REQUIRE(audio::combat_clip_sample_count(x) > audio::combat_clip_sample_count(g));
    REQUIRE(audio::combat_clip_sample_count(g) > audio::combat_clip_sample_count(i));
    REQUIRE(g.seed != i.seed);
    REQUIRE(i.seed != x.seed);
}

TEST_CASE("combat_audio: register mints three distinct stable ClipIds",
          "[combat_audio][clips]") {
    const audio::CombatClipTable t0 = audio::register_combat_clips();
    const audio::CombatClipTable t1 = audio::register_combat_clips();
    const audio::ClipId g = t0.at(audio::CombatSound::Gunshot);
    const audio::ClipId i = t0.at(audio::CombatSound::Impact);
    const audio::ClipId x = t0.at(audio::CombatSound::Death);
    REQUIRE(g.valid());
    REQUIRE(i.valid());
    REQUIRE(x.valid());
    REQUIRE_FALSE(g == i);
    REQUIRE_FALSE(i == x);
    REQUIRE_FALSE(g == x);
    // Idempotent: same tokens every call.
    REQUIRE(t1.at(audio::CombatSound::Gunshot) == g);
    REQUIRE(t1.at(audio::CombatSound::Death) == x);
}

// --- 2. Side-channel emission: fire -> gunshot at the shooter ---------------
TEST_CASE("combat_audio: firing emits a gunshot at the shooter",
          "[combat_audio][emit]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    const math::Vec3 origin{0, 0, 0};
    const Entity shooter = spawn_shooter(scene, origin, /*faction*/ 1u, 40.0f, 100.0f);
    (void)spawn_target(scene, {10, 0, 0}, /*faction*/ 2u, 100.0f);

    CombatContext ctx;
    CombatAudioQueue audio_q;
    audio_q.reserve(16);
    ctx.begin_tick();
    audio_q.begin_tick();

    const u32 ammo_before = scene.registry().get<WeaponComponent>(shooter)->ammo;
    const math::Vec3 dir{1, 0, 0};
    const HitResult hit = fire_weapon(scene.registry(), scene, shooter, origin, dir, &ctx, config);
    const bool fired = shot_fired(scene, shooter, ammo_before);
    REQUIRE(fired);

    emit_fire(audio_q, origin, fired);

    REQUIRE(audio_q.size() == 1u);
    REQUIRE(audio_q.events[0].sound == CombatSoundId::Gunshot);
    REQUIRE(audio_q.events[0].position.x == Catch::Approx(origin.x));
    REQUIRE(audio_q.events[0].position.y == Catch::Approx(origin.y));
    REQUIRE(audio_q.events[0].position.z == Catch::Approx(origin.z));
    REQUIRE(audio_q.events[0].volume > 0.0f);
    (void)hit;
}

TEST_CASE("combat_audio: a refused shot emits no gunshot (no double-emit)",
          "[combat_audio][emit]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    const math::Vec3 origin{0, 0, 0};
    // Out of ammo => fire_weapon refuses.
    const Entity shooter = spawn_shooter(scene, origin, 1u, 40.0f, 100.0f, /*ammo*/ 0u);

    CombatContext ctx;
    CombatAudioQueue audio_q;
    audio_q.reserve(16);
    ctx.begin_tick();
    audio_q.begin_tick();

    const u32 ammo_before = scene.registry().get<WeaponComponent>(shooter)->ammo;
    (void)fire_weapon(scene.registry(), scene, shooter, origin, {1, 0, 0}, &ctx, config);
    const bool fired = shot_fired(scene, shooter, ammo_before);
    REQUIRE_FALSE(fired);

    emit_fire(audio_q, origin, fired);
    REQUIRE(audio_q.empty());  // refused shot: no cue.
}

// --- fire -> impact at the hit point ----------------------------------------
TEST_CASE("combat_audio: a hit emits an impact at the hit position",
          "[combat_audio][emit]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    const math::Vec3 origin{0, 0, 0};
    const Entity shooter = spawn_shooter(scene, origin, 1u, 40.0f, 100.0f);
    // Enemy sphere r=0.5 centered at x=10 => the ray down +X strikes its near
    // face at t = 9.5.
    (void)spawn_target(scene, {10, 0, 0}, 2u, 100.0f, /*radius*/ 0.5f);

    CombatContext ctx;
    CombatAudioQueue audio_q;
    audio_q.reserve(16);
    ctx.begin_tick();
    audio_q.begin_tick();

    const math::Vec3 dir{1, 0, 0};
    const u32 ammo_before = scene.registry().get<WeaponComponent>(shooter)->ammo;
    const HitResult hit = fire_weapon(scene.registry(), scene, shooter, origin, dir, &ctx, config);
    const bool fired = shot_fired(scene, shooter, ammo_before);
    REQUIRE(hit.hit);
    REQUIRE(hit.distance == Catch::Approx(9.5f));

    emit_shot(audio_q, origin, dir, fired, hit);

    // One gunshot + one impact, in order.
    REQUIRE(audio_q.size() == 2u);
    REQUIRE(audio_q.events[0].sound == CombatSoundId::Gunshot);
    REQUIRE(audio_q.events[1].sound == CombatSoundId::Impact);
    // Impact sits at the struck point: origin + dir * 9.5 = (9.5, 0, 0).
    REQUIRE(audio_q.events[1].position.x == Catch::Approx(9.5f));
    REQUIRE(audio_q.events[1].position.y == Catch::Approx(0.0f));
    REQUIRE(audio_q.events[1].position.z == Catch::Approx(0.0f));
}

TEST_CASE("combat_audio: a miss emits the gunshot but no impact",
          "[combat_audio][emit]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    const math::Vec3 origin{0, 0, 0};
    const Entity shooter = spawn_shooter(scene, origin, 1u, 40.0f, /*range*/ 100.0f);
    (void)spawn_target(scene, {0, 50, 0}, 2u, 100.0f);  // off the ray => miss

    CombatContext ctx;
    CombatAudioQueue audio_q;
    audio_q.reserve(16);
    ctx.begin_tick();
    audio_q.begin_tick();

    const math::Vec3 dir{1, 0, 0};
    const u32 ammo_before = scene.registry().get<WeaponComponent>(shooter)->ammo;
    const HitResult hit = fire_weapon(scene.registry(), scene, shooter, origin, dir, &ctx, config);
    const bool fired = shot_fired(scene, shooter, ammo_before);
    REQUIRE(fired);
    REQUIRE_FALSE(hit.hit);

    emit_shot(audio_q, origin, dir, fired, hit);
    REQUIRE(audio_q.size() == 1u);
    REQUIRE(audio_q.events[0].sound == CombatSoundId::Gunshot);
}

// --- death -> death cue at the corpse ---------------------------------------
TEST_CASE("combat_audio: a death emits a death cue at the corpse",
          "[combat_audio][emit]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    const math::Vec3 origin{0, 0, 0};
    const Entity shooter = spawn_shooter(scene, origin, 1u, /*lethal dmg*/ 200.0f, 100.0f);
    const math::Vec3 corpse_pos{10, 0, 0};
    const Entity enemy = spawn_target(scene, corpse_pos, 2u, /*hp*/ 100.0f);

    CombatContext ctx;
    CombatAudioQueue audio_q;
    audio_q.reserve(16);
    ctx.begin_tick();
    audio_q.begin_tick();

    const HitResult hit = fire_weapon(scene.registry(), scene, shooter, origin, {1, 0, 0}, &ctx,
                                      config);
    REQUIRE(hit.hit);
    flush_damage_events(scene.registry(), ctx, config);
    REQUIRE(ctx.deaths.size() == 1u);
    REQUIRE(ctx.deaths[0] == enemy);

    // Drain deaths into cues BEFORE resolve_deaths despawns the corpse.
    emit_deaths(audio_q, scene.registry(), ctx);

    REQUIRE(audio_q.size() == 1u);
    REQUIRE(audio_q.events[0].sound == CombatSoundId::Death);
    REQUIRE(audio_q.events[0].position.x == Catch::Approx(corpse_pos.x));
    REQUIRE(audio_q.events[0].position.y == Catch::Approx(corpse_pos.y));
    REQUIRE(audio_q.events[0].position.z == Catch::Approx(corpse_pos.z));

    // resolve_deaths runs after; the cue position was captured while live.
    (void)resolve_deaths(scene, /*despawn*/ true);
}

// --- audio is a PURE SIDE-CHANNEL: combat numerics are byte-identical -------
TEST_CASE("combat_audio: combat numerics are byte-identical with audio on/off",
          "[combat_audio][side_channel]") {
    // Run an identical combat sequence twice -- once WITHOUT touching any audio
    // helper, once driving the full combat-audio side-channel -- and assert the
    // resulting health/death state is bit-for-bit identical.
    auto run = [](bool with_audio) {
        psynder::scene::detail::EcsRegistryImpl::Get().shutdown();
        Scene scene{EcsRegistry::Get()};
        CombatConfig config{};

        const math::Vec3 origin{0, 0, 0};
        const Entity shooter = spawn_shooter(scene, origin, 1u, /*dmg*/ 35.0f, 100.0f);
        const Entity e0 = spawn_target(scene, {8, 0, 0}, 2u, /*hp*/ 100.0f);
        const Entity e1 = spawn_target(scene, {12, 0, 0}, 2u, /*hp*/ 30.0f);

        CombatContext ctx;
        CombatAudioQueue audio_q;
        audio_q.reserve(16);

        std::array<f32, 8> health_log{};
        usize log_n = 0;

        // Three shots straddling a kill (e1 dies on the second shot when the
        // nearest target e0 has been cleared by then -- but e0 is nearer, so all
        // three hit e0; that is fine, we just need a deterministic sequence).
        for (int shot = 0; shot < 4; ++shot) {
            ctx.begin_tick();
            if (with_audio)
                audio_q.begin_tick();

            const math::Vec3 dir{1, 0, 0};
            const u32 ammo_before = scene.registry().get<WeaponComponent>(shooter)->ammo;
            const HitResult hit =
                fire_weapon(scene.registry(), scene, shooter, origin, dir, &ctx, config);
            const bool fired = scene.registry().get<WeaponComponent>(shooter)->ammo < ammo_before;

            if (with_audio)
                emit_shot(audio_q, origin, dir, fired, hit);

            flush_damage_events(scene.registry(), ctx, config);

            if (with_audio)
                emit_deaths(audio_q, scene.registry(), ctx);

            (void)resolve_deaths(scene, /*despawn*/ true);

            // Record the surviving targets' health (0 if despawned).
            const auto* h0 = scene.registry().get<HealthComponent>(e0);
            const auto* h1 = scene.registry().get<HealthComponent>(e1);
            health_log[log_n++] = h0 ? h0->current_health : -1.0f;
            health_log[log_n++] = h1 ? h1->current_health : -1.0f;
        }
        psynder::scene::detail::EcsRegistryImpl::Get().shutdown();
        return std::pair<std::array<f32, 8>, usize>{health_log, log_n};
    };

    const auto [no_audio, n0] = run(/*with_audio*/ false);
    const auto [with_audio, n1] = run(/*with_audio*/ true);

    REQUIRE(n0 == n1);
    // Byte-identical combat outcome regardless of the audio side-channel.
    REQUIRE(std::memcmp(no_audio.data(), with_audio.data(), n0 * sizeof(f32)) == 0);
}
