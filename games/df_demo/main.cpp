// SPDX-License-Identifier: MIT
// Psynder - DEMO GAME 3: psynder_df_demo.
//
// A Delta Force-style OUTDOOR tactical shooter on heightmap terrain. It is the
// open-world sibling of the indoor psynder_shooter_demo: same engine stack
// (level + hybrid render + physics + combat + AI), but the box room is replaced
// by a large rolling-hills + ridge heightfield, the player and the AI soldiers
// are grounded ON that terrain, and the fights happen across long sightlines
// under a low directional sun that throws long ridge shadows:
//
//   * TERRAIN  - the sample-06 procedural heightfield (editor::world::
//                build_demo_heightmap, a 256x256 value-noise + central X-ridge
//                map) brought into the scene as a single grid mesh entity via
//                editor::world::load_terrain_into_scene. We keep the owning u16
//                height array + its HeightmapDesc alive for the whole run so we
//                can sample the surface height at any (x,z) (bilinear) to ground
//                the player + the soldiers and to feed the shadow heightmap.
//   * RENDER   - a global DIRECTIONAL SUN + ambient via scene::RenderSettings,
//                RenderMode = Hybrid (raster primary visibility + per-pixel
//                traced shadow rays). The hybrid occluder is built each frame
//                from the scene's RT-visible meshes AND the terrain heightmap
//                (the build_shadow_scene overload taking a render::rt::Heightmap),
//                so the terrain self-shadows + casts long ridge shadows. Falls
//                back to plain raster + sun automatically if nothing traces.
//   * PLAYER   - a first-person samples::CharacterController (WASD + mouse-look)
//                whose floor_y is RE-SAMPLED from the terrain every frame so the
//                eye rides the relief instead of a flat plane; an ECS combat
//                entity carrying a Weapon + Health(faction=player) + Hitbox so
//                the soldiers can target it and left-click fires a hitscan.
//   * ENEMIES  - several ECS soldier entities scattered across the terrain, each
//                Health(faction=enemy) + Hitbox + AiAgent + Perception + Patrol +
//                Weapon. They perceive -> chase -> shoot the player over long
//                sightlines via the AI host hooks; their kinematic move step is
//                snapped back onto the terrain height so they walk the hills.
//   * AI HOOKS - LOS  = a terrain heightmap-march (a ridge between two soldiers
//                breaks sight) MAX-combined with a physics World::raycast against
//                the body occluders (the player + the other soldiers each get a
//                static collision box re-posed every frame), so LOS is occluded
//                by BOTH terrain and bodies, exactly as the brief asks.
//                FIRE = gameplay::fire_weapon (hitscan vs hostile hitboxes).
//                MOVE = a kinematic step clamped to the map interior + snapped to
//                the terrain height, pushed into the scene graph for rendering.
//   * COMBAT   - gameplay hitscan / damage / death (engine/gameplay). Player
//                hitscan and soldier fire both route through fire_weapon; deaths
//                are resolved (soldiers despawn + their occluder body removed)
//                once per tick.
//
// PER-FRAME SYSTEM ORDER (DOTS, alloc-free in the steady state):
//   1. read input (or the scripted smoke walk) -> advance the player controller
//      with floor_y re-sampled from the terrain so the eye stays grounded
//   2. physics.step(dt)                              (occluder bodies are static)
//   3. sync the player camera + combat entity transforms to the eye; re-pose the
//      per-actor occluder bodies to the live actor positions for LOS raycasts
//   4-6. combat tick, ordered begin -> queue -> apply so the shots fired THIS
//        frame land this frame:
//        a. CombatContext::begin_tick(); tick_weapon_cooldowns; tick_projectiles
//        b. ai.update(): perceive -> think -> act (Attack -> fire hook ->
//           gameplay::fire_weapon, queueing damage)
//        c. player left-click / scripted pulse -> gameplay::fire_weapon
//        d. flush_damage_events; cleanup_projectiles; resolve_deaths (despawn)
//   7. render: build the hybrid shadow occluder (mesh TLAS + terrain heightmap)
//      then raster + HUD (health, enemies left, kills)
//
// ALLOC-FREE ARGUMENT: the terrain mesh + height array, every scene entity, the
// occluder physics bodies, the shadow-heightmap y_data, and the scene + render
// pools are all created once at load. The per-frame loop only reads input, steps
// fixed-size systems, and mutates pooled ECS columns / a handful of body
// positions in place. AiContext / CombatContext own pre-reserved scratch
// (reserve() at load) that is clear()ed, never freed. The HUD uses ui::imm's
// frame-scoped immediate buffers. No std::vector or other heap allocation
// happens inside the loop. (As in shooter_demo, incrementally add()-ing combat /
// AI components AT LOAD migrates entities through archetypes and grows the ECS
// chunk pool a few times; that is load-time only -- the advisory never fires
// again past frame 0.)
//
// HYBRID-vs-RASTER CHOICE: we render through engine_frame_render() in Hybrid
// mode after building the scene shadow TLAS + the terrain heightmap occluder. If
// the shadow build finds nothing traceable (or shadows are disabled) the
// occluder stays inactive and the same call renders plain raster + the sun -- so
// the demo always shows a lit landscape.
//
// DEFERRED (engine gaps / scope): VEHICLES + a helicopter on the terrain are
// deferred -- the physics vehicle module's per-wheel suspension contacts a
// single FLAT ground plane (physics::vehicle::set_ground_plane takes one y, not
// a heightfield; see racer_demo's note), so a drivable vehicle would float over
// the hills until physics grows a heightfield ground query. Also deferred:
// multi-objective mission flow, networked multiplayer, ragdolls, and tracer /
// muzzle VFX. The first playable increment shipped here is: walk the terrain,
// see + shoot AI soldiers at range, and they shoot back.
//
// CLI flags (shared app args):
//   --smoke-frames=N         Headless CI run: scripted walk + auto-fire N frames.
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
#include "editor/world/LevelSource.h"
#include "gameplay/CombatSystems.h"
#include "gameplay/GameplayComponents.h"
#include "math/Math.h"
#include "physics/Physics.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/RenderingSystem.h"
#include "render/hybrid/ShadowScene.h"
#include "render/rt/HeightmapShadow.h"
#include "scene/RenderSettings.h"
#include "scene/SceneEcs.h"
#include "ui/imm/Imm.h"
#include "world/outdoor/Terrain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

// --- Factions --------------------------------------------------------------
constexpr u32 kFactionPlayer = 1u;
constexpr u32 kFactionEnemy = 2u;

// --- Render config -------------------------------------------------------
constexpr u32 kFbW = 640;
constexpr u32 kFbH = 360;

// --- Terrain config -------------------------------------------------------
// 256x256 @ 1 m spacing => a 255 m x 255 m map; heights 0..~30 m. The sample-06
// generator carries a central X-ridge + rolling value-noise hills (see
// editor::world::build_demo_heightmap), which is the Delta Force terrain look.
constexpr u32 kTerrainSize = 256u;
constexpr f32 kTerrainSpacing = 1.0f;
constexpr f32 kTerrainHeightScale = 30.0f / 65535.0f;  // matches the generator default
constexpr f32 kEyeHeight = 1.7f;                       // soldier-eye above the ground
constexpr f32 kBodyHalfH = 0.85f;                      // actor occluder box half-height

constexpr u32 kMaxEnemies = 6u;

// Pack RGBA8 in the engine's 0xAABBGGRR layout (R in the low byte).
constexpr u32 rgba8(u32 r, u32 g, u32 b, u32 a = 0xFFu) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}

inline math::Vec3 v3(f32 x, f32 y, f32 z) noexcept { return {x, y, z}; }

// --- Terrain height field (owns the samples for the whole run) -------------
// build_demo_heightmap returns the owning u16 vector; HeightmapDesc::heights
// points into it. We keep both alive so we can sample the surface any time.
// Bilinear sampler in WORLD space (x,z). World origin is texel (0,0): the mesh
// load uses world_pos = (x*spacing, h[z*nx+x]*height_scale, z*spacing), so this
// sampler reproduces the rendered surface exactly.
struct TerrainField {
    std::vector<u16> heights{};
    world::outdoor::HeightmapDesc desc{};
    std::vector<f32> shadow_y{};  // f32 surface heights for the rt::Heightmap
    f32 y_min = 0.0f;
    f32 y_max = 0.0f;

    [[nodiscard]] f32 world_extent_x() const noexcept {
        return static_cast<f32>(desc.size_x - 1u) * desc.spacing;
    }
    [[nodiscard]] f32 world_extent_z() const noexcept {
        return static_cast<f32>(desc.size_z - 1u) * desc.spacing;
    }

    [[nodiscard]] f32 raw_at(u32 x, u32 z) const noexcept {
        const usize i = static_cast<usize>(z) * desc.size_x + x;
        return static_cast<f32>(desc.heights[i]) * desc.height_scale;
    }

    // Bilinearly-interpolated surface height at world (wx, wz). Clamps to the
    // map edges so an actor never reads out of bounds.
    [[nodiscard]] f32 height_at(f32 wx, f32 wz) const noexcept {
        const f32 inv = 1.0f / desc.spacing;
        f32 fx = wx * inv;
        f32 fz = wz * inv;
        const f32 max_x = static_cast<f32>(desc.size_x - 1u);
        const f32 max_z = static_cast<f32>(desc.size_z - 1u);
        fx = std::clamp(fx, 0.0f, max_x);
        fz = std::clamp(fz, 0.0f, max_z);
        const u32 x0 = static_cast<u32>(fx);
        const u32 z0 = static_cast<u32>(fz);
        const u32 x1 = x0 + 1u < desc.size_x ? x0 + 1u : x0;
        const u32 z1 = z0 + 1u < desc.size_z ? z0 + 1u : z0;
        const f32 tx = fx - static_cast<f32>(x0);
        const f32 tz = fz - static_cast<f32>(z0);
        const f32 h00 = raw_at(x0, z0);
        const f32 h10 = raw_at(x1, z0);
        const f32 h01 = raw_at(x0, z1);
        const f32 h11 = raw_at(x1, z1);
        const f32 a = h00 + (h10 - h00) * tx;
        const f32 b = h01 + (h11 - h01) * tx;
        return a + (b - a) * tz;
    }
};

// --- Game state ----------------------------------------------------------
// Everything is constructed once; the per-frame loop only mutates it in place.
struct DfGame {
    scene::Scene* scene = nullptr;
    render::RenderingSystem* renderer = nullptr;

    physics::World world{};  // owns the per-actor LOS occluder bodies

    editor::world::LevelGeometry geometry{1u};  // owns the terrain mesh geometry
    TerrainField terrain{};
    render::rt::Heightmap shadow_hm{};  // borrows terrain.shadow_y for the lifetime

    samples::CharacterController controller{};

    // Player combat entity (so soldiers can target it + the player can fire).
    Entity player{};
    physics::BodyId player_body{};
    Entity camera{};

    std::array<Entity, kMaxEnemies> enemies{};
    std::array<physics::BodyId, kMaxEnemies> enemy_bodies{};
    u32 enemy_count = 0u;

    gameplay::CombatSystems combat{};
    ai::AiSystems ai{};

    bool prev_fire = false;  // edge-trigger for left-click
    u32 player_kills = 0u;
};

// The AI LOS / FIRE / MOVE hooks all need the live game; stash a pointer so the
// plain function-pointer hooks (no std::function heap alloc) can reach it.
DfGame* g_game = nullptr;

// --- Terrain ---------------------------------------------------------------
void build_terrain(DfGame& game) {
    // Build the sample-06 heightfield (owns the u16 samples; desc points in).
    game.terrain.heights = editor::world::build_demo_heightmap(
        game.terrain.desc, kTerrainSize, kTerrainSpacing, kTerrainHeightScale);

    // Compute the f32 surface for the shadow heightmap + the global y range.
    const u32 nx = game.terrain.desc.size_x;
    const u32 nz = game.terrain.desc.size_z;
    game.terrain.shadow_y.resize(static_cast<usize>(nx) * nz);
    f32 ymin = 1e30f;
    f32 ymax = -1e30f;
    for (u32 z = 0; z < nz; ++z) {
        for (u32 x = 0; x < nx; ++x) {
            const f32 h = game.terrain.raw_at(x, z);
            game.terrain.shadow_y[static_cast<usize>(z) * nx + x] = h;
            ymin = std::min(ymin, h);
            ymax = std::max(ymax, h);
        }
    }
    game.terrain.y_min = ymin;
    game.terrain.y_max = ymax;

    // Render the terrain as a single grid mesh entity through the standard scene
    // render path. The loader builds the grid geometry into game.geometry (which
    // owns the vertex / index pools for the whole run) and spawns a renderable,
    // but it allocates that renderable's geometry_id from its own LevelGeometry
    // counter -- NOT from this host's render::MeshLibrary, where geometry_id is a
    // packed MeshId.raw. So we register each produced LevelMesh.desc into the
    // renderer's mesh library to get a real MeshId, then patch the spawned
    // entity's RenderableComponent.geometry_id to that id so gather resolves it.
    std::vector<Entity> terrain_entities{};
    editor::world::LoadTerrainOptions opts{};
    opts.albedo_rgba8 = rgba8(96, 116, 78);  // dry-grass green
    opts.name = "DF Terrain";
    const editor::world::LoadResult r = editor::world::load_terrain_into_scene(
        *game.scene, game.terrain.desc, game.geometry, opts, &terrain_entities);
    PSY_LOG_INFO("df_demo: terrain {}x{} ({:.0f}x{:.0f} m), y=[{:.1f},{:.1f}], {} mesh entity",
                 nx, nz, static_cast<double>(game.terrain.world_extent_x()),
                 static_cast<double>(game.terrain.world_extent_z()),
                 static_cast<double>(ymin), static_cast<double>(ymax), r.entities_created);

    const std::span<const editor::world::LevelMesh> meshes = game.geometry.meshes();
    for (usize i = 0; i < terrain_entities.size() && i < meshes.size(); ++i) {
        const render::MeshId mesh_id = game.renderer->meshes().create_mesh(meshes[i].desc);
        (void)scene::set_renderable_geometry(game.scene->registry(), terrain_entities[i],
                                             scene::GeometryKind::Mesh, mesh_id.raw,
                                             meshes[i].local_bounds);
    }

    // Borrowed render::rt::Heightmap for the hybrid shadow march. The march
    // convention matches the lane-11 layout this terrain uses (origin at cell 0,
    // row-major j*width + i), so terrain self-shadows + ridge shadows are real.
    game.shadow_hm.y_data = game.terrain.shadow_y.data();
    game.shadow_hm.width = nx;
    game.shadow_hm.height = nz;
    game.shadow_hm.origin_xz = math::Vec2{0.0f, 0.0f};
    game.shadow_hm.cell_size = game.terrain.desc.spacing;
    game.shadow_hm.y_min = ymin;
    game.shadow_hm.y_max = ymax;
}

// --- Lighting + render settings --------------------------------------------
void make_lighting(DfGame& game) {
    scene::RenderSettings rs = game.scene->render_settings();
    rs.render_mode = scene::RenderMode::Hybrid;
    rs.shadows_enabled = 1u;
    rs.shadow_opacity = 0.72f;
    rs.shadow_softness = 0.35f;
    // A low, warm directional sun raking across the X-ridge from the +X/+Z side
    // so the ridge throws a long shadow down the map (the Delta Force look).
    rs.sun_enabled = 1u;
    rs.sun_direction = scene::sanitize_render_sun_direction(v3(-0.55f, -0.42f, -0.72f));
    rs.sun_color_rgba8 = rgba8(255, 238, 205);
    rs.sun_intensity = 2.2f;
    rs.ambient_color_rgba8 = rgba8(70, 84, 104);  // cool sky fill
    rs.ambient_intensity = 1.0f;
    game.scene->set_render_settings(rs);
}

// --- Combat actor authoring ------------------------------------------------
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

// A static physics box used purely as a LOS occluder for the actor; re-posed to
// the actor's body centre each frame so World::raycast occludes sight lines that
// pass through another soldier (body occlusion, alongside the terrain march).
physics::BodyId make_occluder_body(DfGame& game, math::Vec3 centre, math::Vec3 half) {
    physics::BodyDesc desc{};
    desc.shape = physics::Shape::Box;
    desc.mass = 0.0f;  // static
    desc.position = centre;
    desc.half_extent = half;
    return game.world.create_body(desc);
}

void spawn_player(DfGame& game) {
    // Start near the +Z edge looking down the ridge toward -Z.
    const f32 sx = 0.5f * game.terrain.world_extent_x();
    const f32 sz = game.terrain.world_extent_z() - 24.0f;
    const f32 ground = game.terrain.height_at(sx, sz);
    const math::Vec3 start = v3(sx, ground + kEyeHeight, sz);

    scene::LocalTransform local{};
    local.translation = start;
    game.player = game.scene->create_entity(local);
    make_health(*game.scene, game.player, 100.0f, kFactionPlayer);
    make_hitbox(*game.scene, game.player, 0.6f);
    make_weapon(*game.scene, game.player, /*damage*/ 45.0f, /*range*/ 200.0f, /*fire_rate*/ 6.0f,
                /*ammo*/ 999u);
    // Player occluder body sits at the body centre (eye minus half the body).
    game.player_body =
        make_occluder_body(game, v3(start.x, ground + kBodyHalfH, start.z),
                           v3(0.35f, kBodyHalfH, 0.35f));

    // First-person controller. A huge walk volume spanning the map keeps the
    // built-in box clamp from interfering; we re-pin floor_y to the terrain each
    // frame so the eye rides the relief (the controller pins Y to floor_y +
    // eye_height at the end of update()).
    samples::CharacterControllerConfig cfg{};
    cfg.floor_y = ground;
    cfg.eye_height = kEyeHeight;
    cfg.walk_speed = 7.0f;  // brisk soldier pace across the open map
    cfg.bounds_skin = 0.0f;
    game.controller.set_config(cfg);
    game.controller.set_mode(samples::ControllerMode::Fps);
    game.controller.set_position(start);
    game.controller.set_look(0.0f, 0.0f);

    scene::CameraDesc cam{};
    cam.position = start;
    cam.look_at = v3(sx, start.y, sz - 1.0f);
    cam.fov_y_rad = 70.0f * math::kDegToRad;
    cam.near_z = 0.1f;
    cam.far_z = 600.0f;  // long sightlines across the terrain
    game.camera = game.scene->spawn_camera(cam);
}

void spawn_enemy(DfGame& game,
                 render::MeshId cube,
                 render::MaterialId mat,
                 f32 wx,
                 f32 wz,
                 math::Vec3 patrol_a,
                 math::Vec3 patrol_b) {
    if (game.enemy_count >= kMaxEnemies)
        return;

    const f32 ground = game.terrain.height_at(wx, wz);
    const math::Vec3 pos = v3(wx, ground + kBodyHalfH, wz);

    scene::LocalTransform local{};
    local.translation = pos;
    local.scale = v3(0.7f, 1.7f, 0.7f);  // a humanoid-ish box
    const Entity e =
        game.scene->spawn_mesh_instance(cube, mat, local, scene::kInvalidSceneNode,
                                        scene::RenderableFlags::Visible,
                                        scene::ObjectMobility::Dynamic);

    make_health(*game.scene, e, 80.0f, kFactionEnemy);
    make_hitbox(*game.scene, e, 0.8f);
    make_weapon(*game.scene, e, /*damage*/ 6.0f, /*range*/ 160.0f, /*fire_rate*/ 1.4f, /*ammo*/ 999u);

    ai::AiAgentComponent agent{};
    agent.state = ai::AiState::Patrol;
    agent.sight_range = 140.0f;  // long sightlines
    agent.fov_cos = -1.0f;       // omnidirectional senses (no head-turn model yet)
    agent.attack_range = 130.0f;
    agent.think_interval = 0.12f;
    agent.move_speed = 3.2f;
    game.scene->registry().add<ai::AiAgentComponent>(e, ai::sanitize_ai_agent(agent));
    game.scene->registry().add<ai::PerceptionComponent>(e, ai::PerceptionComponent{});

    ai::PatrolComponent route{};
    route.count = 2u;
    route.waypoints[0] = patrol_a;
    route.waypoints[1] = patrol_b;
    route.wait_time = 1.0f;
    route.arrive_radius = 1.5f;
    game.scene->registry().add<ai::PatrolComponent>(e, ai::sanitize_patrol(route));

    const physics::BodyId body =
        make_occluder_body(game, pos, v3(0.35f, kBodyHalfH, 0.35f));

    game.enemies[game.enemy_count] = e;
    game.enemy_bodies[game.enemy_count] = body;
    ++game.enemy_count;
}

void spawn_enemies(DfGame& game) {
    render::MeshId cube = game.renderer->builtin_mesh(render::BuiltInMesh::UnitCube);
    render::MaterialDesc md{};
    md.albedo_rgba8 = rgba8(168, 92, 64);  // brown soldier
    md.winding = render::MaterialWinding::DoubleSided;
    const render::MaterialId enemy_mat = game.scene->materials().create(md);

    const f32 ex = game.terrain.world_extent_x();
    const f32 ez = game.terrain.world_extent_z();
    // Scatter soldiers across the map; short patrol legs along Z. Helper grounds
    // each patrol waypoint to the terrain so the steer target is on the surface.
    const auto wp = [&](f32 x, f32 z) noexcept -> math::Vec3 {
        return v3(x, game.terrain.height_at(x, z) + kBodyHalfH, z);
    };
    spawn_enemy(game, cube, enemy_mat, ex * 0.30f, ez * 0.40f,
                wp(ex * 0.30f, ez * 0.40f), wp(ex * 0.30f, ez * 0.30f));
    spawn_enemy(game, cube, enemy_mat, ex * 0.70f, ez * 0.42f,
                wp(ex * 0.70f, ez * 0.42f), wp(ex * 0.70f, ez * 0.32f));
    spawn_enemy(game, cube, enemy_mat, ex * 0.50f, ez * 0.28f,
                wp(ex * 0.50f, ez * 0.28f), wp(ex * 0.50f, ez * 0.20f));
    spawn_enemy(game, cube, enemy_mat, ex * 0.22f, ez * 0.55f,
                wp(ex * 0.22f, ez * 0.55f), wp(ex * 0.30f, ez * 0.55f));
    spawn_enemy(game, cube, enemy_mat, ex * 0.80f, ez * 0.20f,
                wp(ex * 0.80f, ez * 0.20f), wp(ex * 0.72f, ez * 0.20f));
    spawn_enemy(game, cube, enemy_mat, ex * 0.45f, ez * 0.50f,
                wp(ex * 0.45f, ez * 0.50f), wp(ex * 0.55f, ez * 0.50f));
}

// --- AI host hooks ---------------------------------------------------------

// LOS: clear when NEITHER the terrain heightmap march NOR a physics raycast
// against the actor occluder bodies blocks the segment origin -> target before
// reaching the target. A ridge between two soldiers (terrain) OR a soldier body
// standing in the line (raycast) both break sight -- "terrain + body occlusion".
bool los_hook(void*, math::Vec3 origin, math::Vec3 target) {
    if (!g_game)
        return true;
    const math::Vec3 to = math::sub(target, origin);
    const f32 dist = math::length(to);
    if (dist <= 1e-4f)
        return true;
    const math::Vec3 dir = math::mul(to, 1.0f / dist);

    // Terrain occlusion: march the heightmap toward the target (skin the ends so
    // an actor's own ground cell does not self-occlude).
    render::rt::Ray ray{};
    ray.origin = math::add(origin, math::mul(dir, 0.5f));
    ray.direction = dir;
    ray.t_min = 0.0f;
    ray.t_max = std::max(0.0f, dist - 1.0f);
    if (g_game->shadow_hm.valid() &&
        render::rt::trace_heightmap_shadow(g_game->shadow_hm, ray))
        return false;  // terrain ridge blocks the line of sight

    // Body occlusion: a hit strictly closer than the target blocks sight.
    const physics::World::RaycastHit hit = g_game->world.raycast(origin, dir, dist);
    return !(hit.hit && hit.t < dist - 0.6f);
}

// FIRE: the agent attacks the player via the combat hitscan.
bool fire_hook(void*, Entity agent, Entity target) {
    if (!g_game)
        return false;
    scene::Scene& scene = *g_game->scene;
    math::Vec3 origin{};
    math::Vec3 tpos{};
    if (!gameplay::entity_position(scene.registry(), agent, origin) ||
        !gameplay::entity_position(scene.registry(), target, tpos))
        return false;
    origin.y += 0.8f;  // muzzle around chest height
    const math::Vec3 to = math::sub(tpos, origin);
    const f32 dist = math::length(to);
    if (dist <= 1e-4f)
        return false;
    const math::Vec3 dir = math::mul(to, 1.0f / dist);
    const gameplay::HitResult r = g_game->combat.fire(scene, agent, origin, dir);
    return r.hit;
}

// MOVE: clamp the desired step to the map interior, SNAP it to the terrain
// height so the soldier walks the hills, push it into the scene graph node (so
// the mesh renders at the new spot), and return the grounded position (which
// act() also stores into the agent's TransformComponent, keeping the two in
// sync). Pure per-row write through the host -- no shared state.
math::Vec3 apply_move_hook(void*, Entity agent, math::Vec3 desired) {
    if (!g_game)
        return desired;
    constexpr f32 kSkin = 2.0f;
    const f32 ex = g_game->terrain.world_extent_x();
    const f32 ez = g_game->terrain.world_extent_z();
    desired.x = std::clamp(desired.x, kSkin, ex - kSkin);
    desired.z = std::clamp(desired.z, kSkin, ez - kSkin);
    desired.y = g_game->terrain.height_at(desired.x, desired.z) + kBodyHalfH;
    scene::Scene& scene = *g_game->scene;
    if (auto* t = scene.registry().get<scene::TransformComponent>(agent)) {
        scene::LocalTransform local = t->local;
        local.translation = desired;
        (void)scene.set_transform(agent, local);
    }
    return desired;
}

// --- Player ----------------------------------------------------------------

// Ground the controller on the terrain (re-pin floor_y to the surface under the
// current eye), then push the eye into the camera + combat entity transforms +
// re-pose the player occluder body so soldiers aim at the live position.
void sync_player_transform(DfGame& game) {
    const math::Vec3 eye = game.controller.eye();
    const math::Vec3 fwd = game.controller.forward();

    // Camera: position at the eye, oriented down `forward`.
    scene::LocalTransform cam_local{};
    cam_local.translation = eye;
    cam_local.rotation =
        scene::camera_rotation_towards(eye, math::add(eye, fwd), v3(0.0f, 1.0f, 0.0f));
    (void)game.scene->set_transform(game.camera, cam_local);

    // Combat entity: a lowered "body" position is what soldiers aim at.
    const math::Vec3 body = v3(eye.x, eye.y - 0.6f, eye.z);
    scene::LocalTransform body_local{};
    body_local.translation = body;
    (void)game.scene->set_transform(game.player, body_local);

    // Re-pose the player occluder body (LOS raycasts pass through it).
    game.world.set_body_position(game.player_body, body);
}

// Snap the controller's floor to the terrain under the current eye so the next
// update()'s ground-pin keeps the player on the surface.
void ground_controller(DfGame& game) {
    const math::Vec3 eye = game.controller.eye();
    samples::CharacterControllerConfig cfg = game.controller.config();
    cfg.floor_y = game.terrain.height_at(eye.x, eye.z);
    game.controller.set_config(cfg);
}

// Player hitscan: fire from the eye along `dir` against hostile hitboxes.
void player_fire(DfGame& game, math::Vec3 dir) {
    const math::Vec3 eye = game.controller.eye();
    const gameplay::HitResult r = game.combat.fire(*game.scene, game.player, eye, dir);
    if (r.hit)
        PSY_LOG_INFO("df_demo: player hitscan struck a soldier at {:.1f}m", r.distance);
}

// Direction from the eye to the nearest still-alive soldier (for the headless
// smoke path, which has no mouse to aim with). Returns false when every soldier
// is dead.
bool aim_at_nearest_enemy(DfGame& game, math::Vec3& out_dir) {
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

// Re-pose every live soldier's occluder body to its current transform so body
// LOS raycasts stay accurate.
void sync_enemy_occluders(DfGame& game) {
    for (u32 i = 0; i < game.enemy_count; ++i) {
        const Entity e = game.enemies[i];
        if (!game.scene->registry().alive(e))
            continue;
        math::Vec3 pos{};
        if (gameplay::entity_position(game.scene->registry(), e, pos))
            game.world.set_body_position(game.enemy_bodies[i], pos);
    }
}

// --- HUD --------------------------------------------------------------------
void draw_hud(DfGame& game, render::Framebuffer& fb) {
    const auto* hp = game.scene->registry().get<scene::HealthComponent>(game.player);
    const f32 health = hp ? hp->current_health : 0.0f;

    u32 alive = 0u;
    for (u32 i = 0; i < game.enemy_count; ++i) {
        if (game.scene->registry().alive(game.enemies[i]))
            ++alive;
    }

    char line[96];
    std::snprintf(line, sizeof(line), "HP %3.0f   SOLDIERS %u   KILLS %u",
                  static_cast<double>(health), alive, game.player_kills);

    ui::imm::begin_frame(fb);
    ui::imm::filled_rect(math::Vec2{8.0f, 8.0f}, math::Vec2{280.0f, 22.0f},
                         ui::imm::rgba(0x0B, 0x10, 0x18, 0xC0));
    ui::imm::rect_outline(math::Vec2{8.0f, 8.0f}, math::Vec2{280.0f, 22.0f},
                          ui::imm::rgba(0x60, 0x70, 0x88));
    ui::imm::label(math::Vec2{16.0f, 14.0f}, line,
                   health > 0.0f ? ui::imm::rgba(0xE6, 0xF0, 0xFF)
                                 : ui::imm::rgba(0xFF, 0x60, 0x50));
    ui::imm::label(math::Vec2{16.0f, fb.height - 18.0f},
                   "WASD move  -  mouse look  -  LMB fire  -  ESC quit",
                   ui::imm::rgba(0x90, 0xA0, 0xB8));
    ui::imm::end_frame();
}

// Resolve soldier deaths: bump the player's kill counter on a player-faction
// kill. (Despawn + the body-occluder removal happen via resolve_deaths +
// the dead-entity skip in sync_enemy_occluders.)
void on_death(void* user, const gameplay::DeathInfo& info) {
    auto* game = static_cast<DfGame*>(user);
    if (!game)
        return;
    if (info.killer_faction == kFactionPlayer)
        ++game->player_kills;
}

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder - Delta Force Demo";
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

    DfGame game{};
    g_game = &game;
    game.renderer = &app_host.rendering_system();
    game.scene = &app_host.create_active_scene();
    app_host.set_scene_lighting_enabled(true);  // hybrid + raster scene lights + sun

    // Pre-size the ECS + render pools so nothing grows in the steady state.
    // Generous headroom: the terrain + the 6 soldiers + the player + camera each
    // migrate through several archetypes as combat / AI components are add()ed at
    // load, so size for the chunk churn to keep even the load phase alloc-quiet.
    game.scene->prewarm_capacity(scene::ScenePrewarmConfig{
        .scene_entities = 128u,
        .renderables = 16u,
        .cameras = 2u,
        .lights = 4u,
        .render_items = 16u,
    });
    app_host.reserve_scene_capacity(16u, 4u);

    // Build the level + actors (load-time archetype migration; see header note).
    build_terrain(game);
    make_lighting(game);
    spawn_player(game);
    spawn_enemies(game);

    // Reserve combat + AI scratch once (alloc-free per frame thereafter).
    game.combat.config.friendly_fire = gameplay::FriendlyFire::Off;  // factions matter
    game.combat.reserve(/*max_damage*/ 32u, /*max_deaths*/ 16u, /*max_despawn*/ 16u);
    game.ai.ctx.los = los_hook;
    game.ai.ctx.fire = fire_hook;
    game.ai.ctx.apply_move = apply_move_hook;

    PSY_LOG_INFO("df_demo: terrain built, {} soldiers, hybrid render + sun{}",
                 game.enemy_count,
                 smoke_frames > 0 ? std::string{" (smoke mode)"} : std::string{});

    u64 last_ticks = platform::Clock::ticks_now();
    u32 frame = 0;
    bool smoke_grounded_logged = false;

    while (!window->should_close()) {
        window->poll_events();

        const u64 now_ticks = platform::Clock::ticks_now();
        auto* input = platform::input();
        if (input && input->key_down(platform::KeyCode::Escape) && !editor::overlays_capturing()) {
            PSY_LOG_INFO("df_demo: escape pressed, exiting");
            break;
        }

        const f32 dt =
            (smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(now_ticks - last_ticks)));
        last_ticks = now_ticks;

        const editor::Mode edit_mode = app_host.engine_frame_update(dt);

        // Clear colour + depth (we drive a custom render loop, so engine_frame_begin
        // is not called for us). Without a depth clear the raster z-test rejects
        // every triangle. A sky-blue clear frames the open landscape.
        app_host.engine_frame_begin(app::FrameClear::color_depth(rgba8(120, 152, 196)));

        // 1. Ground the controller on the terrain, then take input (or the
        //    scripted smoke walk: stride inward toward the soldiers, aim + fire at
        //    the nearest live one so the headless run exercises the full player ->
        //    hitscan -> kill chain). The smoke walk uses the controller so its
        //    Y is terrain-grounded each frame just like interactive play.
        ground_controller(game);
        bool fire_pressed = false;
        math::Vec3 fire_dir = game.controller.forward();
        if (smoke_frames > 0) {
            const f32 t01 = std::clamp(static_cast<f32>(frame) / static_cast<f32>(smoke_frames),
                                       0.0f, 1.0f);
            // Walk from the +Z spawn edge toward the soldier cluster near -Z.
            const f32 sx = 0.5f * game.terrain.world_extent_x();
            const f32 z0 = game.terrain.world_extent_z() - 24.0f;
            const f32 z1 = game.terrain.world_extent_z() * 0.45f;
            const f32 wz = z0 + (z1 - z0) * t01;
            const f32 ground = game.terrain.height_at(sx, wz);
            game.controller.set_position(v3(sx, ground + kEyeHeight, wz));
            game.controller.set_look(0.0f, 0.0f);
            if ((frame % 3u) == 0u && aim_at_nearest_enemy(game, fire_dir))
                fire_pressed = true;
            if (!smoke_grounded_logged) {
                PSY_LOG_INFO("df_demo: player grounded on terrain at "
                             "({:.1f},{:.1f},{:.1f}) ground={:.1f}",
                             static_cast<double>(sx), static_cast<double>(ground + kEyeHeight),
                             static_cast<double>(wz), static_cast<double>(ground));
                smoke_grounded_logged = true;
            }
        } else if (edit_mode != editor::Mode::Edit && input && !editor::overlays_capturing()) {
            game.controller.update(*input, dt);
            const bool left = input->mouse().left;
            fire_pressed = left && !game.prev_fire;  // edge-trigger
            game.prev_fire = left;
            fire_dir = game.controller.forward();
        }

        // 2. Physics step (occluder bodies are static; matches the documented
        //    per-frame order for when dynamic bodies are added).
        game.world.step(dt);

        // 3. Sync the player eye -> camera + combat entity + occluder body, and
        //    re-pose every live soldier's occluder body for LOS raycasts.
        sync_player_transform(game);
        sync_enemy_occluders(game);

        // 4-6. Combat tick, ordered begin -> fire -> flush so the damage that AI
        //      `act` (soldier fire hook) and the player hitscan QUEUE this frame is
        //      applied this same frame. (We drive the free combat systems by hand
        //      instead of CombatSystems::update() because that facade calls
        //      begin_tick() at its top, which would clear the DamageEvents we just
        //      queued from fire_weapon.)
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
        //    (despawn soldiers). on_death bumps player_kills for player kills.
        gameplay::flush_damage_events(registry, cctx, ccfg);
        gameplay::cleanup_projectiles(*game.scene, cctx);
        (void)gameplay::resolve_deaths(*game.scene, /*despawn*/ true, &on_death, &game);

        // 7. Render: build the hybrid shadow occluder (mesh TLAS + terrain
        //    heightmap march) then raster + sun.
        if (app_host.active_scene()) {
            const scene::RenderSettings rs =
                scene::sanitize_render_settings(game.scene->render_settings());
            if (rs.render_mode == scene::RenderMode::Hybrid && rs.shadows_enabled != 0u) {
                render::raster::ShadowOccluder occluder{};
                const render::hybrid::ShadowSceneStats stats =
                    render::hybrid::build_shadow_scene(*game.scene, *game.renderer,
                                                       &game.shadow_hm, occluder);
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
            u32 alive = 0u;
            for (u32 i = 0; i < game.enemy_count; ++i)
                if (game.scene->registry().alive(game.enemies[i]))
                    ++alive;
            PSY_LOG_INFO("df_demo: smoke target reached ({}); kills={} soldiers_left={}; exiting",
                         smoke_frames, game.player_kills, alive);
            break;
        }
    }

    g_game = nullptr;
    const bool capture_ok = app_host.write_capture_if_requested("df_demo");
    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct DfDemo {
    static constexpr std::string_view log_name() noexcept { return "df_demo"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder Delta Force Demo"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    int run(app::WindowApp& app_host, const app::AppArgs& args) { return run_demo(args, app_host); }
};

PSYNDER_WINDOW_SAMPLE_MAIN(DfDemo)
