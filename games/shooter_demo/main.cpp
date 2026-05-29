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

#include "common/CharacterController.h"
#include "ai/AiComponents.h"
#include "ai/AiSystems.h"
#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "gameplay/CombatSystems.h"
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

    // Pre-size the ECS + render pools so nothing grows in the steady state.
    game.scene->prewarm_capacity(scene::ScenePrewarmConfig{
        .scene_entities = 64u,
        .renderables = 32u,
        .cameras = 2u,
        .lights = 4u,
        .render_items = 32u,
    });
    app_host.reserve_scene_capacity(32u, 8u);

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
        (void)gameplay::resolve_deaths(*game.scene, /*despawn*/ true, &on_death, &game);

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
    const bool capture_ok = app_host.write_capture_if_requested("shooter_demo");
    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
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
