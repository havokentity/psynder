// SPDX-License-Identifier: MIT
// Psynder - first DEMO GAME: psynder_shooter_demo.
//
// A small Duke3D/Quake-style indoor first-person shooter that composes the
// whole engine stack to prove the pieces work together:
//   * LEVEL      - an indoor box room built as scene mesh entities (engine/scene
//                  + the builtin UnitCube), with matching STATIC physics box
//                  bodies (engine/physics) so the room blocks line-of-sight and
//                  bullets.
//   * RENDER     - scene point lights + RenderSettings ambient, drawn through the
//                  HYBRID path (raster primary visibility + per-pixel traced
//                  shadow rays; engine/render/hybrid). Falls back to plain
//                  raster+lights automatically if nothing traceable is found.
//   * PLAYER     - a first-person samples::CharacterController (WASD + mouse-look)
//                  driving a scene camera; an ECS combat entity carrying a Weapon
//                  + Health(faction=player) + Hitbox so enemies can target it and
//                  left-click fires a hitscan that kills enemies.
//   * ENEMIES    - a few ECS entities, each Health(faction=enemy) + Hitbox +
//                  AiAgent + Perception + Patrol + Weapon. They perceive -> chase
//                  -> shoot the player via the AI host hooks.
//   * AI HOOKS   - LOS = physics World::raycast (occluded when geometry is hit
//                  before the target); FIRE = gameplay::fire_weapon (hitscan at
//                  the player); MOVE = a kinematic transform step clamped to the
//                  room interior + pushed into the scene graph for rendering.
//   * COMBAT     - gameplay hitscan / damage / death (engine/gameplay). Player
//                  hitscan and enemy fire both route through fire_weapon; deaths
//                  are resolved (enemies despawn) once per tick.
//
// PER-FRAME SYSTEM ORDER (DOTS, alloc-free in the steady state):
//   1. read input + advance the player controller (or the scripted smoke path)
//   2. physics.step(dt)                          (world sim; bodies are static)
//   3. sync the player camera + combat entity transforms to the eye
//   4-6. combat tick, ordered begin -> queue -> apply so the shots fired THIS
//        frame land this frame:
//        a. CombatContext::begin_tick(); tick_weapon_cooldowns; tick_projectiles
//        b. ai.update(): perceive -> think -> act (Attack -> fire hook ->
//           gameplay::fire_weapon, queueing damage)
//        c. player left-click -> gameplay::fire_weapon (hitscan vs enemy hitboxes)
//        d. flush_damage_events; cleanup_projectiles; resolve_deaths (despawn)
//   7. render (hybrid shadow occluder build + raster, or RT fallback) + HUD
//
// ALLOC-FREE ARGUMENT: all entities + physics bodies + scene capacity are
// created once at load. The per-frame loop only reads input, steps fixed-size
// systems, and mutates pooled ECS columns in place. AiContext / CombatContext
// own pre-reserved scratch (reserve() at load) that is clear()ed, never freed.
// The HUD uses ui::imm's frame-scoped immediate buffers. No std::vector or other
// heap allocation happens inside the loop.
//
// HYBRID-vs-RASTER CHOICE: we render through engine_frame_render() in Hybrid
// mode after building the scene shadow TLAS (render::hybrid::build_shadow_scene),
// exactly as player/main.cpp's prepare_hybrid_shadows does. If the shadow build
// finds nothing traceable (or shadows are disabled) the occluder stays inactive
// and the same call renders plain raster + scene lights - so the demo always
// shows a lit room.
//
// CLI flags (shared app args):
//   --smoke-frames=N         Headless CI run: scripted player walk for N frames.
//   --smoke-frames N         Space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the final framebuffer to PATH as PNG.

#include "RagdollFx.h"  // W12-4 pooled death ragdolls (Shape.h-isolated; see header)

#include "common/CharacterController.h"
#include "ai/AiComponents.h"
#include "ai/AiSystems.h"
#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "gameplay/CombatSystems.h"
#include "gameplay/DoorTriggerPickup.h"
#include "gameplay/GameplayComponents.h"
#include "math/Math.h"
#include "physics/Physics.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/RenderingSystem.h"
#include "render/hybrid/ShadowScene.h"
#include "scene/RenderSettings.h"
#include "scene/SceneEcs.h"
#include "ui/imm/Imm.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

using namespace psynder;

namespace {

// --- Factions --------------------------------------------------------------
constexpr u32 kFactionPlayer = 1u;
constexpr u32 kFactionEnemy = 2u;

// --- Render config -------------------------------------------------------
constexpr u32 kFbW = 640;
constexpr u32 kFbH = 360;

// --- Room dimensions (axis-aligned box; metres) ---------------------------
constexpr f32 kFloorY = 0.0f;
constexpr f32 kCeilY = 3.0f;
constexpr f32 kRoomX0 = -7.0f;
constexpr f32 kRoomX1 = 7.0f;
constexpr f32 kRoomZ0 = -9.0f;
constexpr f32 kRoomZ1 = 9.0f;
constexpr f32 kWallThick = 0.4f;   // half-thickness used for the physics slabs
constexpr f32 kEyeHeight = 1.6f;

constexpr u32 kMaxEnemies = 3u;

// --- Death-ragdoll config -------------------------------------------------
// One ragdoll per enemy is the worst case (every enemy can be a corpse at once);
// the RagdollFx pool sizes itself to shooter::kMaxCorpses with kCorpseBodies limb
// boxes each. Mirror those here for the scene-side render-mesh pool + reserve.
constexpr u32 kMaxRagdolls = shooter::kMaxCorpses;
constexpr u32 kRagdollBodies = shooter::kCorpseBodies;  // default_humanoid() segment count
static_assert(kMaxRagdolls >= kMaxEnemies, "corpse pool must cover every enemy");
// Shove the corpse this fast (m/s) away from the killer along the kill ray so it
// tumbles in the kill direction instead of crumpling straight down. Kept gentle
// so the rag bleeds its energy + settles within the headless frame budget.
// Deterministic constant.
constexpr f32 kCorpsePush = 2.0f;

// Pack RGBA8 in the engine's 0xAABBGGRR layout (R in the low byte).
constexpr u32 rgba8(u32 r, u32 g, u32 b, u32 a = 0xFFu) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}

// A single static collision slab (box) registered with the physics world so
// raycasts (AI LOS + player hitscan) are occluded by the room geometry.
struct Slab {
    math::Vec3 center{};
    math::Vec3 half_extent{};
};

// --- Game state ----------------------------------------------------------
// Everything is constructed once; the per-frame loop only mutates it in place.
struct ShooterGame {
    scene::Scene* scene = nullptr;
    render::RenderingSystem* renderer = nullptr;

    physics::World world{};  // owns the static room collision slabs

    samples::CharacterController controller{};
    std::array<math::Aabb, 1> walk_volume{};  // room interior, for slide collision

    // Player combat entity (so enemies can target it + the player can fire).
    Entity player{};
    Entity camera{};

    std::array<Entity, kMaxEnemies> enemies{};
    u32 enemy_count = 0u;

    gameplay::CombatSystems combat{};
    ai::AiSystems ai{};

    bool prev_fire = false;  // edge-trigger for left-click
    u32 player_kills = 0u;

    // --- W10-3 door / trigger / pickup showcase ----------------------------
    Entity health_pickup{};   // a +HP item on the player's path.
    Entity door{};            // a sliding slab the trigger opens.
    Entity door_trigger{};    // volume that opens `door` on player entry.
    physics::BodyId door_body{};  // the door's static collision slab (LOS block).
    math::Vec3 door_closed_pos{};
    bool door_opened_logged = false;  // one-shot "door opened" log latch.

    // Actor view fed to tick_triggers / tick_pickups (player + enemies). Sized
    // once; rebuilt in place each frame (no per-frame heap).
    std::array<gameplay::Actor, 1u + kMaxEnemies> actors{};
    u32 actor_count = 0u;

    // Pre-reserved scratch for the door/trigger/pickup tick (alloc-free output
    // buffers; the systems append into these capacity-bounded spans).
    std::array<gameplay::TriggerEvent, 8u> trigger_events{};
    std::array<gameplay::PickupEvent, 8u> pickup_events{};
    std::array<Entity, 8u> pickup_despawn{};

    // --- W12-4 death ragdolls ----------------------------------------------
    // A fixed pool of corpse slots (one per possible enemy). When an enemy dies
    // we grab a free slot, build a humanoid ragdoll in the SAME physics World at
    // the dead enemy's pose, and render its limb bodies as scene boxes. The
    // ragdoll keeps simulating (steps + joint-solves with the world) until it
    // settles on the floor. The physics ragdoll bookkeeping lives in RagdollFx
    // (its own TU, isolated from physics/Shape.h -- see RagdollFx.h). Here we keep
    // only the SCENE side: the per-limb render boxes, spawned ONCE at load and
    // re-pointed each death (hidden while the slot is free, shown + driven while
    // it holds a corpse). Pooled => the steady state spawns no heap.
    shooter::RagdollPool ragdolls{};
    struct CorpseRender {
        std::array<Entity, kRagdollBodies> meshes{};  // one render box per body
        bool active = false;                          // slot currently rendering a corpse
        bool logged_settled = false;                  // one-shot settle log latch
    };
    std::array<CorpseRender, kMaxRagdolls> corpse_render{};
    render::MeshId corpse_mesh{};       // unit cube reused for every limb box
    render::MaterialId corpse_mat{};    // dim corpse material

    // Scratch for reading back a corpse's limb transforms each frame (no heap).
    std::array<shooter::LimbTransform, kRagdollBodies> limb_scratch{};
};

// The AI LOS / FIRE / MOVE hooks all need the live game; stash a pointer so the
// plain function-pointer hooks (no std::function heap alloc) can reach it.
ShooterGame* g_game = nullptr;

// --- Room geometry as scene mesh entities ----------------------------------
// Each wall / floor / ceiling is a scaled builtin UnitCube (centred at origin,
// extent +/-0.5) so it participates in BOTH the raster pass and the hybrid
// shadow TLAS gather. Static mobility - level geometry never moves.
void spawn_box(ShooterGame& game,
               render::MeshId cube,
               render::MaterialId mat,
               math::Vec3 center,
               math::Vec3 size) {
    scene::LocalTransform local{};
    local.translation = center;
    local.scale = size;  // UnitCube spans 1.0 on each axis, so scale == size.
    (void)game.scene->spawn_mesh_instance(cube,
                                          mat,
                                          local,
                                          scene::kInvalidSceneNode,
                                          scene::RenderableFlags::Visible,
                                          scene::ObjectMobility::Static);
}

// Create a flat-shaded, DOUBLE-SIDED material. The room is built from solid
// boxes the camera stands INSIDE, so it sees their inner (back) faces; double-
// sided drawing (CullMode::None, via MaterialWinding::DoubleSided) makes those
// interior faces render. Enemies are double-sided too so the cheap box mesh
// reads from every angle.
render::MaterialId make_material(scene::Scene& scene, u32 albedo) {
    render::MaterialDesc desc{};
    desc.albedo_rgba8 = albedo;
    desc.winding = render::MaterialWinding::DoubleSided;
    return scene.materials().create(desc);
}

void build_room(ShooterGame& game) {
    render::MeshId cube = game.renderer->builtin_mesh(render::BuiltInMesh::UnitCube);

    const render::MaterialId floor_mat = make_material(*game.scene, rgba8(120, 130, 150));
    const render::MaterialId wall_mat = make_material(*game.scene, rgba8(170, 160, 140));
    const render::MaterialId ceil_mat = make_material(*game.scene, rgba8(90, 95, 110));

    const f32 cx = 0.5f * (kRoomX0 + kRoomX1);
    const f32 cz = 0.5f * (kRoomZ0 + kRoomZ1);
    const f32 sx = (kRoomX1 - kRoomX0);
    const f32 sz = (kRoomZ1 - kRoomZ0);
    const f32 cy = 0.5f * (kFloorY + kCeilY);
    const f32 sy = (kCeilY - kFloorY);
    const f32 t = kWallThick;  // slab thickness

    // Floor + ceiling (thin slabs just below the floor / above the ceiling).
    spawn_box(game, cube, floor_mat, {cx, kFloorY - t, cz}, {sx, 2.0f * t, sz});
    spawn_box(game, cube, ceil_mat, {cx, kCeilY + t, cz}, {sx, 2.0f * t, sz});
    // Four walls (extend a touch past the corners so there is no seam).
    spawn_box(game, cube, wall_mat, {kRoomX0 - t, cy, cz}, {2.0f * t, sy, sz + 4.0f * t});  // -X
    spawn_box(game, cube, wall_mat, {kRoomX1 + t, cy, cz}, {2.0f * t, sy, sz + 4.0f * t});  // +X
    spawn_box(game, cube, wall_mat, {cx, cy, kRoomZ0 - t}, {sx + 4.0f * t, sy, 2.0f * t});  // -Z
    spawn_box(game, cube, wall_mat, {cx, cy, kRoomZ1 + t}, {sx + 4.0f * t, sy, 2.0f * t});  // +Z

    // A central pillar: real cover that casts a shadow AND blocks line-of-sight,
    // so the AI LOS-occlusion path is genuinely exercised.
    const render::MaterialId pillar_mat = make_material(*game.scene, rgba8(200, 120, 90));
    const math::Vec3 pillar_center{0.0f, cy, 0.0f};
    const math::Vec3 pillar_size{1.4f, sy, 1.4f};
    spawn_box(game, cube, pillar_mat, pillar_center, pillar_size);

    // -- Static physics collision slabs (for raycast LOS + hitscan) ----------
    const std::array<Slab, 5> slabs{{
        {{kRoomX0 - t, cy, cz}, {t, 0.5f * sy, 0.5f * sz + 2.0f * t}},  // -X wall
        {{kRoomX1 + t, cy, cz}, {t, 0.5f * sy, 0.5f * sz + 2.0f * t}},  // +X wall
        {{cx, cy, kRoomZ0 - t}, {0.5f * sx + 2.0f * t, 0.5f * sy, t}},  // -Z wall
        {{cx, cy, kRoomZ1 + t}, {0.5f * sx + 2.0f * t, 0.5f * sy, t}},  // +Z wall
        {{0.0f, cy, 0.0f}, {0.7f, 0.5f * sy, 0.7f}},                    // pillar
    }};
    for (const Slab& s : slabs) {
        physics::BodyDesc desc{};
        desc.shape = physics::Shape::Box;
        desc.mass = 0.0f;  // static
        desc.position = s.center;
        desc.half_extent = s.half_extent;
        (void)game.world.create_body(desc);
    }

    // World bounds for the character controller (shrunk so the eye never clips
    // the walls; the controller's own bounds_skin handles the rest).
    game.walk_volume[0] = math::Aabb{{kRoomX0, kFloorY, kRoomZ0}, {kRoomX1, kCeilY, kRoomZ1}};
}

// --- Lights + render settings ----------------------------------------------
void make_lights(ShooterGame& game) {
    // Two warm ceiling point lights, one each side of the pillar, so the pillar
    // casts a visible hybrid shadow.
    const auto add_light = [&](math::Vec3 pos, u32 color, f32 intensity, f32 range) {
        scene::LocalTransform local{};
        local.translation = pos;
        const Entity e = game.scene->create_entity(local);
        scene::LightComponent light{};
        light.kind = scene::LightKind::Point;
        light.color_rgba8 = color;
        light.intensity = intensity;
        light.range = range;
        light.casts_shadow = 1u;
        (void)game.scene->attach_light(e, light);
    };
    add_light({0.0f, kCeilY - 0.4f, -4.5f}, rgba8(255, 240, 220), 2.4f, 16.0f);
    add_light({0.0f, kCeilY - 0.4f, 4.5f}, rgba8(220, 230, 255), 2.4f, 16.0f);

    scene::RenderSettings rs = game.scene->render_settings();
    rs.render_mode = scene::RenderMode::Hybrid;
    rs.shadows_enabled = 1u;
    rs.shadow_opacity = 0.75f;
    rs.shadow_softness = 0.4f;
    rs.ambient_color_rgba8 = rgba8(40, 44, 52);
    rs.ambient_intensity = 1.0f;
    game.scene->set_render_settings(rs);
}

// --- Combat actor authoring -------------------------------------------------
void make_weapon(scene::Scene& scene, Entity e, f32 damage, f32 range, f32 fire_rate, u32 ammo) {
    scene::WeaponComponent weapon{};
    weapon.damage = damage;
    weapon.range = range;
    weapon.fire_rate = fire_rate;
    weapon.ammo = ammo;
    weapon.automatic = 1u;
    scene.registry().add<scene::WeaponComponent>(e, scene::sanitize_weapon_component(weapon));

    gameplay::WeaponRuntimeComponent rt{};
    rt.kind = gameplay::WeaponKind::Hitscan;
    scene.registry().add<gameplay::WeaponRuntimeComponent>(e, gameplay::sanitize_weapon_runtime(rt));
}

void make_hitbox(scene::Scene& scene, Entity e, f32 radius) {
    gameplay::HitboxComponent hb{};
    hb.radius = radius;
    scene.registry().add<gameplay::HitboxComponent>(e, gameplay::sanitize_hitbox(hb));
}

void make_health(scene::Scene& scene, Entity e, f32 hp, u32 faction) {
    scene::HealthComponent health{};
    health.max_health = hp;
    health.current_health = hp;
    health.faction = faction;
    scene.registry().add<scene::HealthComponent>(e, scene::sanitize_health_component(health));
}

void spawn_player(ShooterGame& game) {
    const math::Vec3 start{0.0f, kFloorY + kEyeHeight, kRoomZ1 - 2.0f};

    scene::LocalTransform local{};
    local.translation = start;
    game.player = game.scene->create_entity(local);
    make_health(*game.scene, game.player, 100.0f, kFactionPlayer);
    make_hitbox(*game.scene, game.player, 0.5f);
    make_weapon(*game.scene, game.player, /*damage*/ 50.0f, /*range*/ 60.0f, /*fire_rate*/ 6.0f,
                /*ammo*/ 999u);

    // First-person controller + a scene camera entity it drives each frame.
    samples::CharacterControllerConfig cfg{};
    cfg.floor_y = kFloorY;
    cfg.eye_height = kEyeHeight;
    cfg.bounds_skin = 0.4f;
    game.controller.set_config(cfg);
    game.controller.set_volumes(game.walk_volume.data(),
                                static_cast<u32>(game.walk_volume.size()));
    game.controller.set_mode(samples::ControllerMode::Fps);
    game.controller.set_position(start);
    game.controller.set_look(0.0f, 0.0f);

    scene::CameraDesc cam{};
    cam.position = start;
    cam.look_at = {0.0f, start.y, 0.0f};
    cam.fov_y_rad = 75.0f * math::kDegToRad;
    cam.near_z = 0.05f;
    cam.far_z = 80.0f;
    game.camera = game.scene->spawn_camera(cam);
}

void spawn_enemy(ShooterGame& game,
                 render::MeshId cube,
                 render::MaterialId mat,
                 math::Vec3 pos,
                 const std::array<math::Vec3, 2>& patrol) {
    if (game.enemy_count >= kMaxEnemies)
        return;

    scene::LocalTransform local{};
    local.translation = pos;
    local.scale = {0.7f, 1.7f, 0.7f};  // a humanoid-ish box
    const Entity e =
        game.scene->spawn_mesh_instance(cube, mat, local, scene::kInvalidSceneNode,
                                        scene::RenderableFlags::Visible,
                                        scene::ObjectMobility::Dynamic);

    make_health(*game.scene, e, 100.0f, kFactionEnemy);
    make_hitbox(*game.scene, e, 0.7f);
    make_weapon(*game.scene, e, /*damage*/ 8.0f, /*range*/ 40.0f, /*fire_rate*/ 1.5f, /*ammo*/ 999u);

    ai::AiAgentComponent agent{};
    agent.state = ai::AiState::Patrol;
    agent.sight_range = 30.0f;
    agent.fov_cos = -1.0f;  // omnidirectional senses (no head-turn model yet)
    agent.attack_range = 22.0f;
    agent.think_interval = 0.12f;
    agent.move_speed = 2.6f;
    game.scene->registry().add<ai::AiAgentComponent>(e, ai::sanitize_ai_agent(agent));
    game.scene->registry().add<ai::PerceptionComponent>(e, ai::PerceptionComponent{});

    ai::PatrolComponent route{};
    route.count = 2u;
    route.waypoints[0] = patrol[0];
    route.waypoints[1] = patrol[1];
    route.wait_time = 0.8f;
    route.arrive_radius = 0.6f;
    game.scene->registry().add<ai::PatrolComponent>(e, ai::sanitize_patrol(route));

    game.enemies[game.enemy_count++] = e;
}

void spawn_enemies(ShooterGame& game) {
    render::MeshId cube = game.renderer->builtin_mesh(render::BuiltInMesh::UnitCube);
    const render::MaterialId enemy_mat = make_material(*game.scene, rgba8(210, 70, 60));

    const f32 ey = kFloorY + 0.85f;  // box half-height above the floor
    spawn_enemy(game, cube, enemy_mat, {-4.5f, ey, -6.0f},
                {{{-4.5f, ey, -6.0f}, {-4.5f, ey, -1.0f}}});
    spawn_enemy(game, cube, enemy_mat, {4.5f, ey, -6.0f},
                {{{4.5f, ey, -6.0f}, {4.5f, ey, -1.0f}}});
    spawn_enemy(game, cube, enemy_mat, {0.0f, ey, -8.0f},
                {{{-3.0f, ey, -8.0f}, {3.0f, ey, -8.0f}}});
}

// --- W10-3 mechanics showcase: a health pickup + a trigger-opened door --------
// Built once at load. The smoke player walks the room centre from z=+7 toward
// z=-1; it crosses the trigger volume (opening the door) and overlaps the health
// pickup, so the headless run logs both the "trigger fired -> door opened" and
// "picked up health A->B" events the smoke gate looks for.
void build_mechanics(ShooterGame& game) {
    render::MeshId cube = game.renderer->builtin_mesh(render::BuiltInMesh::UnitCube);

    // -- Health pickup: a small green box at chest height on the player path. --
    const render::MaterialId pickup_mat = make_material(*game.scene, rgba8(80, 220, 120));
    const math::Vec3 pickup_pos{0.0f, kFloorY + 1.0f, 3.0f};
    {
        scene::LocalTransform local{};
        local.translation = pickup_pos;
        local.scale = {0.4f, 0.4f, 0.4f};
        game.health_pickup =
            game.scene->spawn_mesh_instance(cube, pickup_mat, local, scene::kInvalidSceneNode,
                                            scene::RenderableFlags::Visible,
                                            scene::ObjectMobility::Dynamic);
        gameplay::PickupComponent pk{};
        pk.kind = gameplay::PickupKind::Health;
        pk.amount = 25.0f;          // heals 25 HP
        pk.radius = 1.6f;           // generous so the smoke path overlaps it
        pk.pickup_faction = kFactionPlayer;  // only the player grabs it
        game.scene->registry().add<gameplay::PickupComponent>(
            game.health_pickup, gameplay::sanitize_pickup(pk));
    }

    // -- Door: a sliding slab in front of the rear alcove. Closed it blocks the
    //    centre lane (LOS + bullets via its physics slab + combat hitbox); the
    //    trigger slides it up out of the way so the path / sight-line opens. --
    const render::MaterialId door_mat = make_material(*game.scene, rgba8(150, 110, 60));
    const f32 door_h = 2.0f;
    const math::Vec3 door_closed{0.0f, kFloorY + 0.5f * door_h, -3.0f};
    const math::Vec3 door_open = math::add(door_closed, math::Vec3{0.0f, door_h + 0.2f, 0.0f});
    const math::Vec3 door_half{1.6f, 0.5f * door_h, 0.3f};
    {
        scene::LocalTransform local{};
        local.translation = door_closed;
        local.scale = {2.0f * door_half.x, 2.0f * door_half.y, 2.0f * door_half.z};
        game.door =
            game.scene->spawn_mesh_instance(cube, door_mat, local, scene::kInvalidSceneNode,
                                            scene::RenderableFlags::Visible,
                                            scene::ObjectMobility::Dynamic);
        game.door_closed_pos = door_closed;

        // Combat hitbox so a gameplay raycast (player hitscan / probe) is blocked
        // when closed; tick_doors disables it when fully open.
        gameplay::HitboxComponent hb{};
        hb.radius = 0.0f;  // AABB
        hb.half_extent = door_half;
        hb.enabled = 1u;
        game.scene->registry().add<gameplay::HitboxComponent>(
            game.door, gameplay::sanitize_hitbox(hb));

        gameplay::DoorComponent dc{};
        dc.closed_pos = door_closed;
        dc.open_pos = door_open;
        dc.move_time = 0.6f;          // slides open in ~0.6s
        dc.hitbox_entity = game.door;  // gate its own hitbox
        game.scene->registry().add<gameplay::DoorComponent>(
            game.door, gameplay::sanitize_door(dc));

        // A static physics slab co-located with the door so the AI LOS raycast
        // (engine/physics) is also blocked when closed. We move it with the door.
        physics::BodyDesc desc{};
        desc.shape = physics::Shape::Box;
        desc.mass = 0.0f;  // static
        desc.position = door_closed;
        desc.half_extent = door_half;
        game.door_body = game.world.create_body(desc);
    }

    // -- Trigger: a volume on the player's path (z=+4) that opens the door. --
    {
        scene::LocalTransform local{};
        local.translation = {0.0f, kFloorY + 1.0f, 4.0f};
        game.door_trigger = game.scene->create_entity(local);
        gameplay::TriggerVolumeComponent tv{};
        tv.radius = 2.2f;
        tv.fire_faction = kFactionPlayer;  // the player opens it
        tv.target = game.door;
        tv.open_target_door = 1u;
        game.scene->registry().add<gameplay::TriggerVolumeComponent>(
            game.door_trigger, gameplay::sanitize_trigger_volume(tv));
    }
}

// --- W12-4 death ragdolls ----------------------------------------------------
// The physics side (the pooled humanoid ragdolls + the static settle floor + the
// joint solve + settle detection) lives in RagdollFx (its own TU, isolated from
// physics/Shape.h). main.cpp owns only the SCENE side: it pre-spawns a hidden
// render box per limb of every pool slot, reveals them on a kill, and each frame
// reads the pool's limb transforms back to drive those boxes so the flopping
// corpse is visible.
//
// Build the corpse pool ONCE at load: the physics pool (template + floor) and a
// hidden render box per limb of every slot. Spawning is then alloc-free.
void build_ragdoll_pool(ShooterGame& game) {
    // Physics pool: shared humanoid template + a static floor slab spanning the
    // room so corpses land + settle. The room's static slabs are only the four
    // walls + pillar (+ the door); there is NO floor collider, so the pool adds
    // one at kFloorY -- below the chest-height horizontal LOS/hitscan rays, so it
    // does not perturb the existing AI-sight / player-hitscan behaviour.
    const f32 cx = 0.5f * (kRoomX0 + kRoomX1);
    const f32 cz = 0.5f * (kRoomZ0 + kRoomZ1);
    const f32 sx = (kRoomX1 - kRoomX0);
    const f32 sz = (kRoomZ1 - kRoomZ0);
    game.ragdolls.init(game.world, kFloorY, cx, cz, sx, sz);

    // One reusable cube mesh + a dim material for every limb of every corpse.
    game.corpse_mesh = game.renderer->builtin_mesh(render::BuiltInMesh::UnitCube);
    game.corpse_mat = make_material(*game.scene, rgba8(150, 55, 50));

    // Pre-spawn every limb render box, hidden. Each holds a corpse limb's
    // transform while its slot is active; while idle it is parked invisible.
    for (auto& cr : game.corpse_render) {
        for (Entity& mesh : cr.meshes) {
            scene::LocalTransform local{};
            local.scale = {0.1f, 0.1f, 0.1f};
            mesh = game.scene->spawn_mesh_instance(game.corpse_mesh, game.corpse_mat, local,
                                                   scene::kInvalidSceneNode,
                                                   scene::RenderableFlags::None,  // hidden
                                                   scene::ObjectMobility::Dynamic);
        }
    }
}

// Spawn a death ragdoll for a dead enemy. Reads the enemy's pose BEFORE it is
// despawned this frame, asks the pool for a free slot (building the humanoid in
// the demo's physics World at that pose with an impulse AWAY from the killer), and
// reveals that slot's pre-spawned limb meshes. Deterministic: the corpse is pushed
// along the kill direction so it flops. No-op (graceful) if the pool is full.
void spawn_corpse(ShooterGame& game, Entity dead_enemy, math::Vec3 killer_pos) {
    scene::EcsRegistry& registry = game.scene->registry();

    // Pose of the dying enemy (still alive at this point in the tick).
    math::Vec3 epos{};
    if (!gameplay::entity_position(registry, dead_enemy, epos))
        return;
    math::Quat erot{0, 0, 0, 1};
    if (const auto* t = registry.get<scene::TransformComponent>(dead_enemy))
        erot = t->local.rotation;

    // Horizontal kill direction (away from the killer). Deterministic function of
    // the two positions -- no RNG / no time.
    math::Vec3 away = math::sub(epos, killer_pos);
    away.y = 0.0f;
    const f32 d = math::length(away);
    const math::Vec3 push_dir =
        (d > 1e-4f) ? math::mul(away, 1.0f / d) : math::Vec3{0.0f, 0.0f, 1.0f};

    // Spawn the corpse ALREADY LEANING in the push direction and LOW to the floor
    // so it topples onto its side and settles fast (within the headless frame
    // budget) -- the canonical death-ragdoll collapse, same trick the engine
    // ragdoll unit test uses. The lean is a fixed ~70 deg tilt about the
    // horizontal axis perpendicular to the push (up x push_dir), composed onto
    // the enemy's own yaw. Root low enough that the leaning figure's torso starts
    // close to the ground (the pelvis hub sits ~1 m above the root in the desc).
    const math::Vec3 up{0.0f, 1.0f, 0.0f};
    math::Vec3 tilt_axis = math::cross(up, push_dir);
    const f32 axis_len = math::length(tilt_axis);
    tilt_axis = (axis_len > 1e-4f) ? math::mul(tilt_axis, 1.0f / axis_len)
                                   : math::Vec3{1.0f, 0.0f, 0.0f};
    constexpr f32 kLean = 1.2f;  // ~69 deg lean, like the ragdoll settle test
    const math::Quat tilt = math::quat_from_axis_angle(tilt_axis, kLean);
    const math::Quat root_rot = math::quat_normalize(math::quat_mul(tilt, erot));
    // Drop the root so the leaning torso starts ~0.5 m up: it lands almost
    // immediately and bleeds its energy on the floor rather than free-falling.
    const math::Vec3 root_pos{epos.x, kFloorY + 0.2f, epos.z};

    // Gentle horizontal shove so the corpse slides/tumbles in the kill direction
    // as it collapses (no upward kick -- that just prolongs airtime + settling).
    const math::Vec3 impulse = math::mul(push_dir, kCorpsePush);

    const u32 slot = game.ragdolls.spawn(game.world, root_pos, root_rot, impulse);
    if (slot == shooter::RagdollPool::kNoSlot || slot >= game.corpse_render.size())
        return;

    ShooterGame::CorpseRender& cr = game.corpse_render[slot];
    cr.active = true;
    cr.logged_settled = false;
    // Reveal this slot's limb meshes (driven each frame by tick_ragdolls).
    for (Entity mesh : cr.meshes) {
        if (mesh.valid())
            (void)scene::set_renderable_flags(registry, mesh, scene::RenderableFlags::Visible);
    }

    PSY_LOG_INFO("shooter_demo: enemy died -> ragdoll {} spawned ({} bodies)",
                 game.ragdolls.spawned_total(), shooter::RagdollPool::body_count());
}

// Step every active corpse: advance the pool's joint solve (the bodies already
// moved in game.world.step), push each limb body transform into its render mesh so
// the flopping is visible, and report settling. Returns the count of corpses that
// are currently settled + grounded + finite (for the smoke gate). Alloc-free:
// writes pre-spawned meshes in place via a fixed scratch span, no heap.
u32 tick_ragdolls(ShooterGame& game, f32 dt) {
    // Advance the physics ragdolls (joint solve + settle bookkeeping).
    game.ragdolls.step(game.world, dt);

    u32 settled = 0u;
    for (u32 slot = 0; slot < game.corpse_render.size(); ++slot) {
        ShooterGame::CorpseRender& cr = game.corpse_render[slot];
        if (!cr.active)
            continue;

        // Pull the limb transforms back and drive the render boxes.
        const std::span<shooter::LimbTransform> limbs{game.limb_scratch.data(),
                                                      game.limb_scratch.size()};
        game.ragdolls.limb_transforms(game.world, slot, limbs);
        for (u32 i = 0; i < cr.meshes.size(); ++i) {
            if (!cr.meshes[i].valid() || !limbs[i].valid)
                continue;
            scene::LocalTransform local{};
            local.translation = limbs[i].position;
            local.rotation = limbs[i].rotation;
            local.scale = limbs[i].box_scale;
            (void)game.scene->set_transform(cr.meshes[i], local);
        }

        const shooter::CorpseState st = game.ragdolls.state(slot);
        if (st.settled && st.grounded && st.finite) {
            ++settled;
            if (!cr.logged_settled) {
                cr.logged_settled = true;
                PSY_LOG_INFO("shooter_demo: ragdoll settled (min_y={:.2f}m, grounded + finite)",
                             static_cast<double>(st.min_y));
            }
        }
    }
    return settled;
}

// Tear down every corpse (free the World bodies). Called at shutdown so the
// pooled ragdolls release their physics bodies cleanly.
void teardown_ragdolls(ShooterGame& game) {
    game.ragdolls.teardown(game.world);
    for (auto& cr : game.corpse_render)
        cr.active = false;
}

// Rebuild the actor view (player + live enemies) fed to the trigger / pickup
// systems. In-place into the pre-sized array => no per-frame heap.
void refresh_actors(ShooterGame& game) {
    game.actor_count = 0u;
    game.actors[game.actor_count++] = gameplay::Actor{game.player, kFactionPlayer};
    for (u32 i = 0; i < game.enemy_count; ++i) {
        if (game.scene->registry().alive(game.enemies[i]))
            game.actors[game.actor_count++] = gameplay::Actor{game.enemies[i], kFactionEnemy};
    }
}

// One tick of the W10-3 mechanics: triggers -> doors -> pickups, plus driving
// the door's physics slab + mesh from the DoorComponent and logging the events
// the smoke gate looks for. Alloc-free (fixed-capacity output spans).
void tick_mechanics(ShooterGame& game, f32 dt) {
    scene::EcsRegistry& registry = game.scene->registry();
    refresh_actors(game);
    const std::span<const gameplay::Actor> actors{game.actors.data(), game.actor_count};

    // Triggers: a player entry opens the linked door (sets open_request).
    u32 tev = 0u;
    (void)gameplay::tick_triggers(registry, actors, game.trigger_events, &tev);
    for (u32 i = 0; i < tev; ++i) {
        const gameplay::TriggerEvent& e = game.trigger_events[i];
        PSY_LOG_INFO("shooter_demo: trigger fired (entry #{}) -> door opening",
                     e.fire_count);
    }

    // Doors: advance the slab animation + gate the combat hitbox.
    gameplay::tick_doors(registry, dt);

    // Push the door's animated position into BOTH the rendered mesh and the
    // physics slab so LOS + bullets track the moving door.
    if (const auto* d = registry.get<gameplay::DoorComponent>(game.door)) {
        if (auto* t = registry.get<scene::TransformComponent>(game.door)) {
            scene::LocalTransform local = t->local;
            local.translation = d->position;
            (void)game.scene->set_transform(game.door, local);
        }
        if (game.door_body.valid())
            game.world.set_body_position(game.door_body, d->position);
        if (d->state == gameplay::DoorState::Open && !game.door_opened_logged) {
            game.door_opened_logged = true;
            PSY_LOG_INFO("shooter_demo: trigger fired -> door opened (LOS + path clear)");
        }
    }

    // Pickups: grant on overlap, then despawn the spent items.
    u32 pev = 0u;
    u32 desp = 0u;
    (void)gameplay::tick_pickups(registry, actors, game.pickup_despawn, &desp,
                                 game.pickup_events, &pev);
    for (u32 i = 0; i < pev; ++i) {
        const gameplay::PickupEvent& e = game.pickup_events[i];
        const char* kind = (e.kind == gameplay::PickupKind::Health) ? "health"
                           : (e.kind == gameplay::PickupKind::Ammo)  ? "ammo"
                                                                     : "weapon";
        PSY_LOG_INFO("shooter_demo: picked up {} {:.0f}->{:.0f}", kind,
                     static_cast<double>(e.before), static_cast<double>(e.after));
    }
    for (u32 i = 0; i < desp; ++i)
        (void)game.scene->despawn_entity(game.pickup_despawn[i]);
}

// --- AI host hooks ----------------------------------------------------------

// LOS: clear when the physics raycast from `origin` to `target` is NOT blocked
// by room geometry before reaching the target. The room walls/pillar are the
// only physics bodies, so any hit closer than the target distance means the line
// of sight is occluded.
bool los_hook(void*, math::Vec3 origin, math::Vec3 target) {
    if (!g_game)
        return true;
    const math::Vec3 to = math::sub(target, origin);
    const f32 dist = math::length(to);
    if (dist <= 1e-4f)
        return true;
    const math::Vec3 dir = math::mul(to, 1.0f / dist);
    const physics::World::RaycastHit hit = g_game->world.raycast(origin, dir, dist);
    // A hit strictly closer than the target (minus a small skin) blocks sight.
    return !(hit.hit && hit.t < dist - 0.1f);
}

// FIRE: the agent attacks the player via the combat hitscan. fire_weapon sweeps
// the agent's weapon ray against enemy-of-its-faction hitboxes (the player) and
// queues damage into the shared CombatContext (mutex-guarded), so this is safe
// from the parallel act() body.
bool fire_hook(void*, Entity agent, Entity target) {
    if (!g_game)
        return false;
    scene::Scene& scene = *g_game->scene;
    math::Vec3 origin{};
    math::Vec3 tpos{};
    if (!gameplay::entity_position(scene.registry(), agent, origin) ||
        !gameplay::entity_position(scene.registry(), target, tpos))
        return false;
    origin.y += 1.0f;  // muzzle around chest height
    const math::Vec3 to = math::sub(tpos, origin);
    const f32 dist = math::length(to);
    if (dist <= 1e-4f)
        return false;
    const math::Vec3 dir = math::mul(to, 1.0f / dist);
    const gameplay::HitResult r = g_game->combat.fire(scene, agent, origin, dir);
    return r.hit;
}

// MOVE: clamp the desired step to the room interior, push it into the scene
// graph node (so the enemy mesh renders at the new spot), and return the clamped
// position (which act() also stores into the agent's TransformComponent, keeping
// the two in sync). Pure per-row write through the host - no shared state.
math::Vec3 apply_move_hook(void*, Entity agent, math::Vec3 desired) {
    if (!g_game)
        return desired;
    constexpr f32 kSkin = 0.8f;
    desired.x = std::clamp(desired.x, kRoomX0 + kSkin, kRoomX1 - kSkin);
    desired.z = std::clamp(desired.z, kRoomZ0 + kSkin, kRoomZ1 - kSkin);
    desired.y = kFloorY + 0.85f;  // stay grounded
    scene::Scene& scene = *g_game->scene;
    if (auto* t = scene.registry().get<scene::TransformComponent>(agent)) {
        scene::LocalTransform local = t->local;
        local.translation = desired;
        (void)scene.set_transform(agent, local);
    }
    return desired;
}

// --- Player --------------------------------------------------------------

// Push the player's eye into the camera entity + the combat entity transform so
// enemies target the live position and the camera follows first-person.
void sync_player_transform(ShooterGame& game) {
    const math::Vec3 eye = game.controller.eye();
    const math::Vec3 fwd = game.controller.forward();

    // Camera: position at the eye, oriented down `forward`.
    scene::LocalTransform cam_local{};
    cam_local.translation = eye;
    cam_local.rotation =
        scene::camera_rotation_towards(eye, math::add(eye, fwd), math::Vec3{0.0f, 1.0f, 0.0f});
    (void)game.scene->set_transform(game.camera, cam_local);

    // Combat entity: a slightly lowered "body" position is what enemies aim at.
    scene::LocalTransform body_local{};
    body_local.translation = math::Vec3{eye.x, eye.y - 0.5f, eye.z};
    (void)game.scene->set_transform(game.player, body_local);
}

// Player hitscan: fire from the eye along `dir` against enemy hitboxes.
// fire_weapon picks the nearest different-faction hitbox in range and queues the
// damage; combat.update() resolves it (and any death) this same frame.
void player_fire(ShooterGame& game, math::Vec3 dir) {
    const math::Vec3 eye = game.controller.eye();
    const gameplay::HitResult r = game.combat.fire(*game.scene, game.player, eye, dir);
    if (r.hit)
        PSY_LOG_INFO("shooter_demo: player hitscan struck an enemy at {:.1f}m", r.distance);
}

// Direction from the eye to the nearest still-alive enemy (for the headless
// smoke path, which has no mouse to aim with). Returns false when every enemy is
// dead. Lets the smoke run demonstrate the full player -> hitscan -> damage ->
// death -> despawn -> kill-count chain without a window.
bool aim_at_nearest_enemy(ShooterGame& game, math::Vec3& out_dir) {
    const math::Vec3 eye = game.controller.eye();
    f32 best = 1e30f;
    bool found = false;
    for (u32 i = 0; i < game.enemy_count; ++i) {
        const Entity e = game.enemies[i];
        if (!game.scene->registry().alive(e))
            continue;
        math::Vec3 pos{};
        if (!gameplay::entity_position(game.scene->registry(), e, pos))
            continue;
        const math::Vec3 to = math::sub(pos, eye);
        const f32 d = math::length(to);
        if (d > 1e-3f && d < best) {
            best = d;
            out_dir = math::mul(to, 1.0f / d);
            found = true;
        }
    }
    return found;
}

// --- HUD -------------------------------------------------------------------
void draw_hud(ShooterGame& game, render::Framebuffer& fb) {
    const auto* hp = game.scene->registry().get<scene::HealthComponent>(game.player);
    const f32 health = hp ? hp->current_health : 0.0f;

    u32 alive = 0u;
    for (u32 i = 0; i < game.enemy_count; ++i) {
        if (game.scene->registry().alive(game.enemies[i]))
            ++alive;
    }

    char line[96];
    std::snprintf(line, sizeof(line), "HP %3.0f   ENEMIES %u   KILLS %u",
                  static_cast<double>(health), alive, game.player_kills);

    ui::imm::begin_frame(fb);
    ui::imm::filled_rect(math::Vec2{8.0f, 8.0f}, math::Vec2{260.0f, 22.0f},
                         ui::imm::rgba(0x0B, 0x10, 0x18, 0xC0));
    ui::imm::rect_outline(math::Vec2{8.0f, 8.0f}, math::Vec2{260.0f, 22.0f},
                          ui::imm::rgba(0x60, 0x70, 0x88));
    ui::imm::label(math::Vec2{16.0f, 14.0f}, line,
                   health > 0.0f ? ui::imm::rgba(0xE6, 0xF0, 0xFF)
                                 : ui::imm::rgba(0xFF, 0x60, 0x50));
    ui::imm::label(math::Vec2{16.0f, fb.height - 18.0f},
                   "WASD move  -  mouse look  -  LMB fire  -  ESC quit",
                   ui::imm::rgba(0x90, 0xA0, 0xB8));
    ui::imm::end_frame();
}

// Resolve enemy deaths: despawn the entity + bump the player's kill counter.
void on_death(void* user, const gameplay::DeathInfo& info) {
    auto* game = static_cast<ShooterGame*>(user);
    if (!game)
        return;
    if (info.killer_faction == kFactionPlayer)
        ++game->player_kills;
}

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder - Shooter Demo";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;
    return desc;
}

int run_demo(const app::AppArgs& args, app::WindowApp& app_host) {
    const u32 smoke_frames = args.smoke_frames;
    auto* window = &app_host.window();

    ShooterGame game{};
    g_game = &game;
    game.renderer = &app_host.rendering_system();
    game.scene = &app_host.create_active_scene();
    app_host.set_scene_lighting_enabled(true);  // hybrid + raster scene lights

    // Pre-size the ECS + render pools so nothing grows in the steady state. The
    // W12-4 death-ragdoll pool pre-spawns kMaxRagdolls * kRagdollBodies (= 21)
    // limb meshes at load, so the renderable + entity reserves include them.
    constexpr u32 kCorpseMeshes = kMaxRagdolls * kRagdollBodies;
    game.scene->prewarm_capacity(scene::ScenePrewarmConfig{
        .scene_entities = 80u + kCorpseMeshes,  // + pickup/door/trigger (W10-3) + corpse limbs (W12-4)
        .renderables = 40u + kCorpseMeshes,
        .cameras = 2u,
        .lights = 4u,
        .render_items = 40u + kCorpseMeshes,
    });
    app_host.reserve_scene_capacity(32u + kCorpseMeshes, 8u);

    // NOTE: building the level + actors below incrementally add()s combat / AI
    // components, which migrates each entity through several archetypes and grows
    // the ECS chunk pool a few times AT LOAD (the engine logs a one-time
    // "pooled storage grew" advisory per new archetype). That is load-time only:
    // once every entity reaches its final archetype the pool is stable and the
    // per-frame loop below performs ZERO heap allocation (verified: the advisory
    // never fires again past frame 0, even over a long run).
    build_room(game);
    make_lights(game);
    spawn_player(game);
    spawn_enemies(game);
    build_mechanics(game);     // W10-3: health pickup + trigger-opened door
    build_ragdoll_pool(game);  // W12-4: corpse ragdoll pool (floor + hidden limb meshes)

    // Reserve combat + AI scratch once (alloc-free per frame thereafter).
    game.combat.config.friendly_fire = gameplay::FriendlyFire::Off;  // factions matter
    game.combat.reserve(/*max_damage*/ 32u, /*max_deaths*/ 8u, /*max_despawn*/ 16u);
    game.ai.ctx.los = los_hook;
    game.ai.ctx.fire = fire_hook;
    game.ai.ctx.apply_move = apply_move_hook;

    PSY_LOG_INFO("shooter_demo: room built, {} enemies, hybrid render{}",
                 game.enemy_count,
                 smoke_frames > 0 ? std::string{" (smoke mode)"} : std::string{});

    u64 last_ticks = platform::Clock::ticks_now();
    u32 frame = 0;
    u32 peak_settled_ragdolls = 0u;  // W12-4: most corpses settled simultaneously

    while (!window->should_close()) {
        window->poll_events();

        const u64 now_ticks = platform::Clock::ticks_now();
        auto* input = platform::input();
        if (input && input->key_down(platform::KeyCode::Escape) && !editor::overlays_capturing()) {
            PSY_LOG_INFO("shooter_demo: escape pressed, exiting");
            break;
        }

        const f32 dt =
            (smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(now_ticks - last_ticks)));
        last_ticks = now_ticks;

        const editor::Mode edit_mode = app_host.engine_frame_update(dt);

        // Clear colour + depth for the new frame (we drive a custom render loop,
        // so the framework's engine_frame_begin is not called for us). Without a
        // depth clear the raster z-test would reject every triangle.
        app_host.engine_frame_begin(app::FrameClear::color_depth(rgba8(18, 20, 26)));

        // 1. Player input (or the scripted smoke walk: drift inward, aim + fire
        //    at the nearest live enemy so the headless run exercises the whole
        //    player -> hitscan -> kill chain).
        bool fire_pressed = false;
        math::Vec3 fire_dir = game.controller.forward();
        if (smoke_frames > 0) {
            const f32 t01 = std::clamp(static_cast<f32>(frame) / static_cast<f32>(smoke_frames),
                                       0.0f, 1.0f);
            game.controller.set_position(
                {0.0f, kFloorY + kEyeHeight, kRoomZ1 - 2.0f - 8.0f * t01});
            game.controller.set_look(0.0f, 0.0f);
            if ((frame % 4u) == 0u && aim_at_nearest_enemy(game, fire_dir))
                fire_pressed = true;
        } else if (edit_mode != editor::Mode::Edit && input && !editor::overlays_capturing()) {
            game.controller.update(*input, dt);
            const bool left = input->mouse().left;
            fire_pressed = left && !game.prev_fire;  // edge-trigger
            game.prev_fire = left;
            fire_dir = game.controller.forward();
        }

        // 2. Physics step (room is static; keeps the world consistent + matches
        //    the documented per-frame order for when dynamic bodies are added).
        game.world.step(dt);

        // 3. Sync the player eye into the camera + combat entity transforms.
        sync_player_transform(game);

        // 4-6. Combat tick, ordered begin -> fire -> flush so the damage that AI
        //      `act` (the enemy fire hook) and the player hitscan QUEUE this frame
        //      is applied this same frame. (We drive the free combat systems by
        //      hand instead of CombatSystems::update() because that facade calls
        //      begin_tick() at its top, which would clear the very DamageEvents we
        //      just queued from fire_weapon.)
        scene::EcsRegistry& registry = game.scene->registry();
        gameplay::CombatContext& cctx = game.combat.ctx;
        const gameplay::CombatConfig& ccfg = game.combat.config;

        cctx.begin_tick();
        gameplay::tick_weapon_cooldowns(registry, dt);
        gameplay::tick_projectiles(registry, dt, cctx, ccfg);

        // 4. AI: perceive -> think -> act. The Attack branch routes through the
        //    fire hook -> gameplay::fire_weapon, queueing damage into cctx.
        game.ai.update(registry, dt);

        // 5. Player hitscan on a fresh left-click / scripted pulse (queues into cctx).
        if (fire_pressed)
            player_fire(game, fire_dir);

        // 6. Apply the queued damage, clean up spent projectiles, resolve deaths
        //    (despawn enemies). on_death bumps player_kills for player-faction kills.
        gameplay::flush_damage_events(registry, cctx, ccfg);
        gameplay::cleanup_projectiles(*game.scene, cctx);

        // 6a. W12-4 death ragdolls: every entity that hit 0 HP this tick
        //     (cctx.deaths, populated by flush_damage_events) is still alive HERE
        //     -- resolve_deaths below is what despawns it. So this is the window to
        //     read the dying enemy's pose and spawn a flopping corpse at it. We
        //     only ragdoll enemies; the impulse pushes the corpse away from the
        //     player (the killer), a deterministic function of their positions.
        {
            const math::Vec3 killer = game.controller.eye();
            for (Entity dead : cctx.deaths) {
                if (gameplay::entity_faction(registry, dead) == kFactionEnemy)
                    spawn_corpse(game, dead, killer);
            }
        }

        (void)gameplay::resolve_deaths(*game.scene, /*despawn*/ true, &on_death, &game);

        // 6a'. Step every active death ragdoll's joint pass + drive its limb
        //      render boxes (the World already integrated the bodies in step()).
        //      settled_ragdolls feeds the smoke settle gate below.
        const u32 settled_ragdolls = tick_ragdolls(game, dt);
        peak_settled_ragdolls = std::max(peak_settled_ragdolls, settled_ragdolls);

        // 6b. W10-3 mechanics: triggers -> doors -> pickups (player grabs the
        //     health item; crossing the trigger slides the door open, clearing
        //     LOS + the path). Drives the door's mesh + physics slab + logs the
        //     pickup/door events the smoke gate looks for.
        tick_mechanics(game, dt);

        // 7. Render: build the hybrid shadow occluder (Hybrid mode) then raster.
        if (app_host.active_scene()) {
            const scene::RenderSettings rs =
                scene::sanitize_render_settings(game.scene->render_settings());
            if (rs.render_mode == scene::RenderMode::Hybrid && rs.shadows_enabled != 0u) {
                render::raster::ShadowOccluder occluder{};
                const render::hybrid::ShadowSceneStats stats =
                    render::hybrid::build_shadow_scene(*game.scene, *game.renderer, occluder);
                if (stats.built && occluder.active()) {
                    occluder.opacity = rs.shadow_opacity;
                    occluder.softness = rs.shadow_softness;
                    occluder.samples = 1u;
                    app_host.set_pending_shadow_occluder(occluder);
                }
            }
        }
        (void)app_host.engine_frame_render();
        draw_hud(game, app_host.framebuffer());
        app_host.present();

        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("shooter_demo: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    g_game = nullptr;

    // W12-4 smoke gate: the kills above each spawned a death ragdoll; assert at
    // least one settled (came to rest, grounded + finite) by the end and that
    // every still-active corpse is grounded + finite right now. A non-settling /
    // exploding ragdoll fails the gate (non-zero exit), so the headless CI run
    // actually proves the flop-and-settle behaviour, not just "it spawned".
    bool smoke_pass = true;
    if (smoke_frames > 0) {
        u32 active = 0u;
        u32 grounded = 0u;
        u32 finite = 0u;
        for (u32 slot = 0; slot < shooter::RagdollPool::max_corpses(); ++slot) {
            const shooter::CorpseState st = game.ragdolls.state(slot);
            if (!st.active)
                continue;
            ++active;
            if (st.finite)
                ++finite;
            if (st.grounded)
                ++grounded;
        }
        const bool spawned_any = game.ragdolls.spawned_total() > 0u;
        const bool any_settled = peak_settled_ragdolls > 0u;
        const bool all_grounded_finite = (active == grounded) && (active == finite);
        smoke_pass = spawned_any && any_settled && all_grounded_finite;
        PSY_LOG_INFO(
            "shooter_demo: ragdolls_spawned={} peak_settled={} active_corpses={} "
            "grounded={} finite={}",
            game.ragdolls.spawned_total(), peak_settled_ragdolls, active, grounded, finite);
        if (smoke_pass)
            PSY_LOG_INFO(
                "shooter_demo: {} ragdolls settled, all bodies grounded + finite -- ragdoll PASS",
                peak_settled_ragdolls);
        else
            PSY_LOG_ERROR(
                "shooter_demo: ragdoll FAIL (spawned={} settled={} grounded/finite={})",
                spawned_any, any_settled, all_grounded_finite);
    }

    teardown_ragdolls(game);

    const bool capture_ok = app_host.write_capture_if_requested("shooter_demo");
    return (capture_ok && smoke_pass) ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct ShooterDemo {
    static constexpr std::string_view log_name() noexcept { return "shooter_demo"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder Shooter Demo"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    int run(app::WindowApp& app_host, const app::AppArgs& args) { return run_demo(args, app_host); }
};

PSYNDER_WINDOW_SAMPLE_MAIN(ShooterDemo)
