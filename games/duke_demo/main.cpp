// SPDX-License-Identifier: MIT
// Psynder - DEMO GAME 4: psynder_duke_demo.
//
// A Duke3D/Quake-style INDOOR BSP shooter that demonstrates PORTAL/PVS
// visibility culling over a multi-room level. This is the "reference-game"
// milestone for the indoor renderer: where shooter_demo is a single box room,
// duke_demo is several rooms joined by corridors, with a Potentially-Visible-
// Set built at load from the leaf-portal graph so rooms the camera cannot see
// are culled before the renderer ever touches them.
//
// WHAT IT COMPOSES (same engine stack as shooter_demo + the new world/bsp PVS):
//   * LEVEL   - four rooms (START, HUB, EAST, VAULT) + connecting corridors,
//               authored in code as scene mesh boxes (engine/scene + UnitCube)
//               with matching STATIC physics slabs (engine/physics) so geometry
//               blocks line-of-sight and bullets. A PARALLEL in-memory BSP
//               (engine/world/bsp): one cluster per room/corridor leaf, plus a
//               BspPortalSet describing which leaves open onto which. The VAULT
//               starts SEALED (no portal) so its cluster is unreachable through
//               open space and the PVS culls it; a trigger near the HUB "opens
//               the vault door", rebuilding the PVS so the vault appears.
//   * PVS     - world::bsp::build_pvs floods the portal graph (Quake-style coarse
//               portal flood) into per-cluster bit-vectors. Each frame we locate
//               the camera's leaf, fetch its PVS row, and set every OTHER room's
//               mesh entities Visible/None accordingly (engine/scene
//               set_renderable_flags). The renderer only rasterises visible
//               rooms -> genuine indoor occlusion culling, alloc-free.
//   * PLAYER  - first-person samples::CharacterController (WASD + mouse-look)
//               driving a scene camera + an ECS combat entity (Weapon + Health
//               (faction=player) + Hitbox).
//   * ENEMIES - ranged ECS agents (Health(enemy) + Hitbox + AiAgent + Perception
//               + Patrol + Weapon) that perceive -> chase -> shoot via the AI
//               host hooks; LOS uses our physics raycast against room geometry.
//   * COMBAT  - gameplay hitscan / damage / death (engine/gameplay).
//   * RENDER  - hybrid path (raster primary visibility + traced shadow rays;
//               engine/render/hybrid), point lights per room, falls back to
//               plain raster automatically.
//
// PER-FRAME SYSTEM ORDER (DOTS, alloc-free in the steady state):
//   1. read input + advance the player controller (or the scripted smoke path)
//   2. physics.step(dt)
//   3. sync the player camera + combat entity transforms to the eye
//   4. PVS CULL: locate camera leaf -> PVS row -> set per-room renderable flags
//   5-7. combat tick (begin -> AI act/fire -> player fire -> flush/resolve)
//   8. render (hybrid shadow occluder build + raster) + HUD
//
// ALLOC-FREE ARGUMENT: rooms, portals, enemies, physics bodies, and the PVS
// scratch are all built once at load (PvsBuildScratch::reserve_for). The PVS
// rebuild on the vault trigger reuses that warm scratch, so even the door event
// is alloc-stable. The per-frame loop only locates a leaf, indexes a bit-vector,
// flips pooled renderable flags, and steps fixed-size systems - no heap traffic.
//
// PVS TECHNIQUE CITATION: see engine/world/bsp/PvsBuild.h - leaf-portal flood,
// the coarse "base PVS" stage of Quake VIS (Teller 1992 / Carmack qvis), with
// the per-frame portal-frustum refinement available in PortalClip.cpp. DESIGN.md
// ADR-012 records the runtime-PVS-builder decision.
//
// CLI flags (shared app args):
//   --smoke-frames=N         Headless CI run: scripted traverse + auto-fire.
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
#include "world/bsp/Bsp.h"
#include "world/bsp/BspFormat.h"
#include "world/bsp/Portal.h"
#include "world/bsp/PvsBuild.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

// --- Factions --------------------------------------------------------------
constexpr u32 kFactionPlayer = 1u;
constexpr u32 kFactionEnemy = 2u;

// --- Render config ---------------------------------------------------------
constexpr u32 kFbW = 640;
constexpr u32 kFbH = 360;

constexpr f32 kFloorY = 0.0f;
constexpr f32 kCeilY = 3.0f;
constexpr f32 kEyeHeight = 1.6f;
constexpr f32 kWallT = 0.3f;  // half-thickness of wall/floor slabs

constexpr u32 kMaxEnemies = 4u;

// Pack RGBA8 in the engine's 0xAABBGGRR layout (R in the low byte).
constexpr u32 rgba8(u32 r, u32 g, u32 b, u32 a = 0xFFu) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}

// --- Level layout ----------------------------------------------------------
// Cluster ids (one per room/corridor leaf). Solid space is kBspSolidCluster.
// Top-down (x right, z up):
//
//     +z (north)
//      |                       [VAULT]  (cluster 4, sealed until trigger)
//      |                          |
//      |                       (corridor C2, cluster 5)
//      |   [START]--(C0,clu1)--[HUB]--(C1,clu3)--[EAST]
//      |   clu0                 clu2              clu6
//      |
//      +---------------- +x (east)
//
// START is where the player spawns; HUB is the junction; EAST holds enemies;
// VAULT is the PVS showcase (culled until the door opens). Corridors are their
// own clusters so PVS culls a room you cannot see down a corridor's far end.
enum Cluster : i32 {
    kCluStart = 0,
    kCluC0 = 1,    // START<->HUB corridor
    kCluHub = 2,
    kCluC1 = 3,    // HUB<->EAST corridor
    kCluEast = 6,
    kCluVault = 4,
    kCluC2 = 5,    // HUB<->VAULT corridor (sealed until trigger)
    kClusterCount = 7,
};

// Axis-aligned room/corridor volume (interior, metres). Walls are built just
// outside; the cluster's BSP leaf bounds match this box.
struct Volume {
    i32 cluster;
    math::Vec3 lo;  // interior min (x,z); y spans floor..ceil
    math::Vec3 hi;  // interior max
    u32 color;      // wall tint for visual room identity
};

// The seven leaf volumes. Coordinates chosen so rooms are well-separated and
// corridors are narrow (so a far room is genuinely out of sight from a near one).
constexpr std::array<Volume, kClusterCount> kVolumes{{
    {kCluStart, {-12.0f, kFloorY, -3.0f}, {-6.0f, kCeilY, 3.0f}, rgba8(150, 150, 170)},
    {kCluC0, {-6.0f, kFloorY, -1.2f}, {-2.0f, kCeilY, 1.2f}, rgba8(120, 120, 130)},
    {kCluHub, {-2.0f, kFloorY, -3.5f}, {3.0f, kCeilY, 3.5f}, rgba8(160, 150, 130)},
    {kCluC1, {3.0f, kFloorY, -1.2f}, {7.0f, kCeilY, 1.2f}, rgba8(120, 120, 130)},
    {kCluVault, {-2.0f, kFloorY, 7.0f}, {3.0f, kCeilY, 12.0f}, rgba8(180, 140, 90)},
    {kCluC2, {-0.6f, kFloorY, 3.5f}, {1.6f, kCeilY, 7.0f}, rgba8(120, 120, 130)},
    {kCluEast, {7.0f, kFloorY, -3.5f}, {12.0f, kCeilY, 3.5f}, rgba8(150, 130, 150)},
}};

// Look up a volume by cluster id (linear scan over 7 entries; build-time only).
const Volume& volume_for(i32 cluster) {
    for (const Volume& v : kVolumes) {
        if (v.cluster == cluster)
            return v;
    }
    return kVolumes[0];
}

// --- Game state ------------------------------------------------------------
struct Slab {
    math::Vec3 center{};
    math::Vec3 half_extent{};
};

// Renderable mesh entities + the cluster each one belongs to, so the PVS pass
// can flip the right room's flags. Floor/ceiling of a room share its cluster.
struct RoomMeshes {
    i32 cluster = kCluStart;
    std::array<Entity, 8> entities{};
    u32 count = 0u;
};

struct DukeGame {
    scene::Scene* scene = nullptr;
    render::RenderingSystem* renderer = nullptr;
    physics::World world{};

    samples::CharacterController controller{};
    std::vector<math::Aabb> walk_volumes{};  // all room/corridor interiors (slide)

    Entity player{};
    Entity camera{};

    std::array<Entity, kMaxEnemies> enemies{};
    u32 enemy_count = 0u;

    gameplay::CombatSystems combat{};
    ai::AiSystems ai{};

    // --- BSP / PVS ---
    world::bsp::BspMap bsp{};
    world::bsp::BspPortalSet portals{};
    world::bsp::PvsBuildScratch pvs_scratch{};
    u32 pvs_row_bytes = 0u;
    bool vault_open = false;   // trigger toggles this -> rebuilds PVS
    i32 cam_cluster = kCluStart;
    u32 visible_leaves = 0u;   // last frame's PVS-visible leaf count (HUD)
    u32 total_leaves = 0u;

    std::array<RoomMeshes, kClusterCount> rooms{};

    bool prev_fire = false;
    u32 player_kills = 0u;
    bool logged_cull = false;  // one-shot "PVS culled" log for the smoke harness
};

DukeGame* g_game = nullptr;

// --- Material + box helpers ------------------------------------------------
render::MaterialId make_material(scene::Scene& scene, u32 albedo) {
    render::MaterialDesc desc{};
    desc.albedo_rgba8 = albedo;
    desc.winding = render::MaterialWinding::DoubleSided;  // camera stands inside
    return scene.materials().create(desc);
}

// Spawn a scene box and record it under `room` so the PVS pass can toggle it.
void spawn_room_box(DukeGame& game,
                    RoomMeshes& room,
                    render::MeshId cube,
                    render::MaterialId mat,
                    math::Vec3 center,
                    math::Vec3 size) {
    scene::LocalTransform local{};
    local.translation = center;
    local.scale = size;
    const Entity e =
        game.scene->spawn_mesh_instance(cube, mat, local, scene::kInvalidSceneNode,
                                        scene::RenderableFlags::Visible,
                                        scene::ObjectMobility::Static);
    if (room.count < room.entities.size())
        room.entities[room.count++] = e;
}

// Register a static physics slab (LOS + hitscan occluder).
void add_slab(DukeGame& game, math::Vec3 center, math::Vec3 half_extent) {
    physics::BodyDesc desc{};
    desc.shape = physics::Shape::Box;
    desc.mass = 0.0f;
    desc.position = center;
    desc.half_extent = half_extent;
    (void)game.world.create_body(desc);
}

// Build one room/corridor: floor + ceiling tinted to the room colour, recorded
// under its cluster so the PVS pass flips the whole room together. Walls are
// shared/implicit (the global shell below), keeping the box count small. We add
// the interior box to the walk-volume list so the controller can slide inside.
void build_volume(DukeGame& game, render::MeshId cube, const Volume& v) {
    RoomMeshes& room = game.rooms[static_cast<usize>(&v - kVolumes.data())];
    room.cluster = v.cluster;

    const f32 cx = 0.5f * (v.lo.x + v.hi.x);
    const f32 cz = 0.5f * (v.lo.z + v.hi.z);
    const f32 sx = (v.hi.x - v.lo.x);
    const f32 sz = (v.hi.z - v.lo.z);

    const render::MaterialId floor_mat = make_material(*game.scene, rgba8(110, 120, 140));
    const render::MaterialId ceil_mat = make_material(*game.scene, v.color);
    spawn_room_box(game, room, cube, floor_mat, {cx, kFloorY - kWallT, cz},
                   {sx, 2.0f * kWallT, sz});
    spawn_room_box(game, room, cube, ceil_mat, {cx, kCeilY + kWallT, cz},
                   {sx, 2.0f * kWallT, sz});

    game.walk_volumes.push_back(math::Aabb{{v.lo.x, kFloorY, v.lo.z}, {v.hi.x, kCeilY, v.hi.z}});
}

// Outer wall shell + a few interior pillars. We approximate the indoor maze with
// physics slabs around the union of all volumes plus pillars in the bigger rooms
// (cover that blocks LOS + casts shadows). Walls don't move per-room so they live
// in the START room's mesh group (always visible) but we keep them as physics +
// a thin set of meshes only where needed for the look.
void build_shell(DukeGame& game, render::MeshId cube) {
    RoomMeshes& shell = game.rooms[kCluStart];  // attach shell visuals to START (always shown)
    const render::MaterialId wall_mat = make_material(*game.scene, rgba8(90, 95, 110));

    // A pillar in the HUB and one in EAST for cover (visual + physics).
    const f32 cy = 0.5f * (kFloorY + kCeilY);
    const f32 sy = (kCeilY - kFloorY);
    const auto pillar = [&](math::Vec3 c) {
        spawn_room_box(game, shell, cube, wall_mat, c, {0.9f, sy, 0.9f});
        add_slab(game, c, {0.45f, 0.5f * sy, 0.45f});
    };
    pillar({0.5f, cy, 0.0f});    // HUB centre
    pillar({9.5f, cy, 1.5f});    // EAST

    // Perimeter physics walls around each volume edge that is NOT a portal mouth.
    // Cheap approximation: a box ring just outside each volume; bullets/LOS stop
    // at the room edges (the corridors keep the open paths). We register slabs
    // only (visual ceilings already read the room tint).
    for (const Volume& v : kVolumes) {
        const f32 cx = 0.5f * (v.lo.x + v.hi.x);
        const f32 cz = 0.5f * (v.lo.z + v.hi.z);
        const f32 hx = 0.5f * (v.hi.x - v.lo.x);
        const f32 hz = 0.5f * (v.hi.z - v.lo.z);
        add_slab(game, {v.lo.x - kWallT, cy, cz}, {kWallT, 0.5f * sy, hz});  // -X
        add_slab(game, {v.hi.x + kWallT, cy, cz}, {kWallT, 0.5f * sy, hz});  // +X
        add_slab(game, {cx, cy, v.lo.z - kWallT}, {hx, 0.5f * sy, kWallT});  // -Z
        add_slab(game, {cx, cy, v.hi.z + kWallT}, {hx, 0.5f * sy, kWallT});  // +Z
    }
}

// --- BSP + portal authoring ------------------------------------------------
// Build the in-memory BSP that mirrors the room layout. We don't need a real
// splitting tree for the demo's PVS (the flood works off the portal graph), but
// `locate(map, eye)` must map a world point to the correct leaf. We therefore
// build a thin AABB-classifying tree: a degenerate node chain isn't ideal, so we
// instead implement locate via the leaf bounds directly (see camera_cluster()).
// The BspMap still carries one leaf per cluster (for the PVS table shape).
void build_bsp(DukeGame& game) {
    using namespace world::bsp;
    game.bsp.nodes.clear();
    game.bsp.leaves.clear();
    game.bsp.faces.clear();
    game.bsp.pvs.clear();

    // One leaf per cluster; bounds = the interior volume (used by camera_cluster).
    game.bsp.leaves.resize(kClusterCount);
    for (u32 i = 0; i < kClusterCount; ++i) {
        const Volume& v = kVolumes[i];
        BspLeaf& l = game.bsp.leaves[v.cluster];
        l.cluster = v.cluster;
        l.first_face = 0;
        l.face_count = 0;
        l.bounds.min = {v.lo.x, kFloorY, v.lo.z};
        l.bounds.max = {v.hi.x, kCeilY, v.hi.z};
    }
    game.total_leaves = kClusterCount;
}

// Build the portal set: an edge per open corridor mouth. front_leaf/back_leaf are
// LEAF INDICES (== cluster id here, since we laid leaves out one-per-cluster).
// The polygon windings are left default - the coarse PVS flood only reads the
// leaf adjacency. When `vault_open` the HUB<->C2<->VAULT chain is connected;
// otherwise those portals are omitted so the vault cluster is an island and PVS
// culls it.
void build_portals(DukeGame& game) {
    using namespace world::bsp;
    game.portals.portals.clear();
    game.portals.vertices.clear();

    const auto link = [&](i32 a, i32 b) {
        BspPortal p{};
        p.front_leaf = a;
        p.back_leaf = b;
        game.portals.portals.push_back(p);
    };
    // Always-open corridors.
    link(kCluStart, kCluC0);
    link(kCluC0, kCluHub);
    link(kCluHub, kCluC1);
    link(kCluC1, kCluEast);
    // Vault chain - only when the door is open.
    if (game.vault_open) {
        link(kCluHub, kCluC2);
        link(kCluC2, kCluVault);
    }
}

// (Re)build the PVS table from the current portal set. Reuses the warm scratch
// so this is alloc-stable even when triggered mid-game by the vault door.
void rebuild_pvs(DukeGame& game) {
    using namespace world::bsp;
    const u32 clusters =
        build_pvs(game.bsp, game.portals, game.pvs_scratch, game.bsp.pvs, game.pvs_row_bytes);
    PSY_LOG_INFO("duke_demo: PVS rebuilt - {} clusters, {} bytes/row, vault_open={}",
                 clusters, game.pvs_row_bytes, game.vault_open ? 1 : 0);
}

// Locate the camera's leaf/cluster by AABB containment (a tiny skin lets the eye
// height + wall thickness not eject us). Falls back to the nearest-centre leaf so
// a point in a doorway still resolves. Integer-deterministic.
i32 camera_cluster(const DukeGame& game, math::Vec3 eye) {
    constexpr f32 kSkin = 0.6f;
    for (const Volume& v : kVolumes) {
        if (eye.x >= v.lo.x - kSkin && eye.x <= v.hi.x + kSkin && eye.z >= v.lo.z - kSkin &&
            eye.z <= v.hi.z + kSkin) {
            return v.cluster;
        }
    }
    // Nearest-centre fallback.
    i32 best = kCluStart;
    f32 best_d = 1e30f;
    for (const Volume& v : kVolumes) {
        const f32 cx = 0.5f * (v.lo.x + v.hi.x);
        const f32 cz = 0.5f * (v.lo.z + v.hi.z);
        const f32 dx = eye.x - cx;
        const f32 dz = eye.z - cz;
        const f32 d = dx * dx + dz * dz;
        if (d < best_d) {
            best_d = d;
            best = v.cluster;
        }
    }
    return best;
}

// PVS CULL: given the camera cluster, set each room's renderable flags so only
// PVS-visible rooms are rasterised. Returns the visible-leaf count. Reads the
// bit-vector row we built; the eye's own cluster is always visible.
u32 apply_pvs_cull(DukeGame& game, i32 cam_cluster) {
    using namespace world::bsp;
    scene::EcsRegistry& reg = game.scene->registry();
    const u32 row_bytes = game.pvs_row_bytes;
    const u8* row = nullptr;
    if (!game.bsp.pvs.empty() && cam_cluster >= 0 &&
        static_cast<u32>(cam_cluster) < kClusterCount && row_bytes > 0u) {
        row = game.bsp.pvs.data() + static_cast<usize>(cam_cluster) * row_bytes;
    }

    u32 visible = 0u;
    for (RoomMeshes& room : game.rooms) {
        bool vis = true;
        if (row) {
            const u32 ci = static_cast<u32>(room.cluster);
            vis = (row[ci >> 3] & (1u << (ci & 7u))) != 0u;
        }
        if (vis)
            ++visible;
        const scene::RenderableFlags flags =
            vis ? scene::RenderableFlags::Visible : scene::RenderableFlags::None;
        for (u32 i = 0; i < room.count; ++i)
            (void)scene::set_renderable_flags(reg, room.entities[i], flags);
    }
    return visible;
}

// --- Lights + render settings ----------------------------------------------
void make_lights(DukeGame& game) {
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
    const f32 ly = kCeilY - 0.4f;
    add_light({-9.0f, ly, 0.0f}, rgba8(255, 240, 220), 2.2f, 14.0f);  // START
    add_light({0.5f, ly, 0.0f}, rgba8(230, 235, 255), 2.6f, 16.0f);   // HUB
    add_light({9.5f, ly, 0.0f}, rgba8(255, 230, 210), 2.4f, 16.0f);   // EAST
    add_light({0.5f, ly, 9.5f}, rgba8(255, 220, 180), 2.0f, 14.0f);   // VAULT

    scene::RenderSettings rs = game.scene->render_settings();
    rs.render_mode = scene::RenderMode::Hybrid;
    rs.shadows_enabled = 1u;
    rs.shadow_opacity = 0.7f;
    rs.shadow_softness = 0.4f;
    rs.ambient_color_rgba8 = rgba8(38, 42, 50);
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

void spawn_player(DukeGame& game) {
    const Volume& start = volume_for(kCluStart);
    const math::Vec3 pos{0.5f * (start.lo.x + start.hi.x), kFloorY + kEyeHeight,
                         0.5f * (start.lo.z + start.hi.z)};

    scene::LocalTransform local{};
    local.translation = pos;
    game.player = game.scene->create_entity(local);
    make_health(*game.scene, game.player, 100.0f, kFactionPlayer);
    make_hitbox(*game.scene, game.player, 0.5f);
    make_weapon(*game.scene, game.player, 50.0f, 60.0f, 6.0f, 999u);

    samples::CharacterControllerConfig cfg{};
    cfg.floor_y = kFloorY;
    cfg.eye_height = kEyeHeight;
    cfg.bounds_skin = 0.35f;
    game.controller.set_config(cfg);
    game.controller.set_volumes(game.walk_volumes.data(),
                                static_cast<u32>(game.walk_volumes.size()));
    game.controller.set_mode(samples::ControllerMode::Fps);
    game.controller.set_position(pos);
    game.controller.set_look(0.0f, 0.0f);

    scene::CameraDesc cam{};
    cam.position = pos;
    cam.look_at = {pos.x + 1.0f, pos.y, pos.z};
    cam.fov_y_rad = 75.0f * math::kDegToRad;
    cam.near_z = 0.05f;
    cam.far_z = 90.0f;
    game.camera = game.scene->spawn_camera(cam);
}

void spawn_enemy(DukeGame& game,
                 render::MeshId cube,
                 render::MaterialId mat,
                 math::Vec3 pos,
                 const std::array<math::Vec3, 2>& patrol) {
    if (game.enemy_count >= kMaxEnemies)
        return;
    scene::LocalTransform local{};
    local.translation = pos;
    local.scale = {0.7f, 1.7f, 0.7f};
    const Entity e =
        game.scene->spawn_mesh_instance(cube, mat, local, scene::kInvalidSceneNode,
                                        scene::RenderableFlags::Visible,
                                        scene::ObjectMobility::Dynamic);
    make_health(*game.scene, e, 100.0f, kFactionEnemy);
    make_hitbox(*game.scene, e, 0.7f);
    make_weapon(*game.scene, e, 8.0f, 45.0f, 1.5f, 999u);

    ai::AiAgentComponent agent{};
    agent.state = ai::AiState::Patrol;
    agent.sight_range = 36.0f;
    agent.fov_cos = -1.0f;
    agent.attack_range = 26.0f;
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

void spawn_enemies(DukeGame& game) {
    render::MeshId cube = game.renderer->builtin_mesh(render::BuiltInMesh::UnitCube);
    const render::MaterialId enemy_mat = make_material(*game.scene, rgba8(210, 70, 60));
    const f32 ey = kFloorY + 0.85f;
    const Volume& east = volume_for(kCluEast);
    const Volume& hub = volume_for(kCluHub);
    // Two in EAST, one in HUB, one in VAULT (revealed after the door opens).
    spawn_enemy(game, cube, enemy_mat, {east.lo.x + 1.5f, ey, -2.0f},
                {{{east.lo.x + 1.5f, ey, -2.0f}, {east.lo.x + 1.5f, ey, 2.0f}}});
    spawn_enemy(game, cube, enemy_mat, {east.hi.x - 1.5f, ey, 2.0f},
                {{{east.hi.x - 1.5f, ey, 2.0f}, {east.hi.x - 1.5f, ey, -2.0f}}});
    spawn_enemy(game, cube, enemy_mat, {2.0f, ey, -2.5f},
                {{{2.0f, ey, -2.5f}, {2.0f, ey, 2.5f}}});
    const Volume& vault = volume_for(kCluVault);
    spawn_enemy(game, cube, enemy_mat,
                {0.5f * (vault.lo.x + vault.hi.x), ey, vault.hi.z - 1.5f},
                {{{vault.lo.x + 1.0f, ey, vault.hi.z - 1.5f},
                  {vault.hi.x - 1.0f, ey, vault.hi.z - 1.5f}}});
    (void)hub;
}

// --- AI host hooks ---------------------------------------------------------
bool los_hook(void*, math::Vec3 origin, math::Vec3 target) {
    if (!g_game)
        return true;
    const math::Vec3 to = math::sub(target, origin);
    const f32 dist = math::length(to);
    if (dist <= 1e-4f)
        return true;
    const math::Vec3 dir = math::mul(to, 1.0f / dist);
    const physics::World::RaycastHit hit = g_game->world.raycast(origin, dir, dist);
    return !(hit.hit && hit.t < dist - 0.1f);
}

bool fire_hook(void*, Entity agent, Entity target) {
    if (!g_game)
        return false;
    scene::Scene& scene = *g_game->scene;
    math::Vec3 origin{};
    math::Vec3 tpos{};
    if (!gameplay::entity_position(scene.registry(), agent, origin) ||
        !gameplay::entity_position(scene.registry(), target, tpos))
        return false;
    origin.y += 1.0f;
    const math::Vec3 to = math::sub(tpos, origin);
    const f32 dist = math::length(to);
    if (dist <= 1e-4f)
        return false;
    const math::Vec3 dir = math::mul(to, 1.0f / dist);
    const gameplay::HitResult r = g_game->combat.fire(scene, agent, origin, dir);
    return r.hit;
}

math::Vec3 apply_move_hook(void*, Entity agent, math::Vec3 desired) {
    if (!g_game)
        return desired;
    desired.y = kFloorY + 0.85f;
    scene::Scene& scene = *g_game->scene;
    if (auto* t = scene.registry().get<scene::TransformComponent>(agent)) {
        scene::LocalTransform local = t->local;
        local.translation = desired;
        (void)scene.set_transform(agent, local);
    }
    return desired;
}

// --- Player ----------------------------------------------------------------
void sync_player_transform(DukeGame& game) {
    const math::Vec3 eye = game.controller.eye();
    const math::Vec3 fwd = game.controller.forward();
    scene::LocalTransform cam_local{};
    cam_local.translation = eye;
    cam_local.rotation =
        scene::camera_rotation_towards(eye, math::add(eye, fwd), math::Vec3{0.0f, 1.0f, 0.0f});
    (void)game.scene->set_transform(game.camera, cam_local);

    scene::LocalTransform body_local{};
    body_local.translation = math::Vec3{eye.x, eye.y - 0.5f, eye.z};
    (void)game.scene->set_transform(game.player, body_local);
}

void player_fire(DukeGame& game, math::Vec3 dir) {
    const math::Vec3 eye = game.controller.eye();
    const gameplay::HitResult r = game.combat.fire(*game.scene, game.player, eye, dir);
    if (r.hit)
        PSY_LOG_INFO("duke_demo: player hitscan struck an enemy at {:.1f}m", r.distance);
}

bool aim_at_nearest_enemy(DukeGame& game, math::Vec3& out_dir) {
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
void draw_hud(DukeGame& game, render::Framebuffer& fb) {
    const auto* hp = game.scene->registry().get<scene::HealthComponent>(game.player);
    const f32 health = hp ? hp->current_health : 0.0f;
    u32 alive = 0u;
    for (u32 i = 0; i < game.enemy_count; ++i) {
        if (game.scene->registry().alive(game.enemies[i]))
            ++alive;
    }
    char line[128];
    std::snprintf(line, sizeof(line), "HP %3.0f  ENEMIES %u  KILLS %u  PVS %u/%u  VAULT %s",
                  static_cast<double>(health), alive, game.player_kills, game.visible_leaves,
                  game.total_leaves, game.vault_open ? "OPEN" : "SEALED");

    ui::imm::begin_frame(fb);
    ui::imm::filled_rect(math::Vec2{8.0f, 8.0f}, math::Vec2{420.0f, 22.0f},
                         ui::imm::rgba(0x0B, 0x10, 0x18, 0xC0));
    ui::imm::rect_outline(math::Vec2{8.0f, 8.0f}, math::Vec2{420.0f, 22.0f},
                          ui::imm::rgba(0x60, 0x70, 0x88));
    ui::imm::label(math::Vec2{16.0f, 14.0f}, line,
                   health > 0.0f ? ui::imm::rgba(0xE6, 0xF0, 0xFF)
                                 : ui::imm::rgba(0xFF, 0x60, 0x50));
    ui::imm::label(math::Vec2{16.0f, fb.height - 18.0f},
                   "WASD move  -  mouse look  -  LMB fire  -  walk to HUB to open the vault",
                   ui::imm::rgba(0x90, 0xA0, 0xB8));
    ui::imm::end_frame();
}

void on_death(void* user, const gameplay::DeathInfo& info) {
    auto* game = static_cast<DukeGame*>(user);
    if (!game)
        return;
    if (info.killer_faction == kFactionPlayer)
        ++game->player_kills;
}

// The vault-door trigger: when the player stands inside the HUB volume, open the
// vault (rebuild portals + PVS once). Returns true the frame the door opens.
bool check_vault_trigger(DukeGame& game, math::Vec3 eye) {
    if (game.vault_open)
        return false;
    if (camera_cluster(game, eye) == kCluHub) {
        game.vault_open = true;
        build_portals(game);
        rebuild_pvs(game);
        PSY_LOG_INFO("duke_demo: HUB trigger reached - VAULT door opened");
        return true;
    }
    return false;
}

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder - Duke Demo (BSP/PVS)";
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

    DukeGame game{};
    g_game = &game;
    game.renderer = &app_host.rendering_system();
    game.scene = &app_host.create_active_scene();
    app_host.set_scene_lighting_enabled(true);

    game.scene->prewarm_capacity(scene::ScenePrewarmConfig{
        .scene_entities = 96u,
        .renderables = 48u,
        .cameras = 2u,
        .lights = 6u,
        .render_items = 48u,
    });
    app_host.reserve_scene_capacity(48u, 12u);
    game.walk_volumes.reserve(kClusterCount);

    // --- Level geometry (visual rooms + physics shell) ---
    render::MeshId cube = game.renderer->builtin_mesh(render::BuiltInMesh::UnitCube);
    for (const Volume& v : kVolumes)
        build_volume(game, cube, v);
    build_shell(game, cube);
    make_lights(game);
    spawn_player(game);
    spawn_enemies(game);

    // --- BSP + portals + PVS (vault starts sealed) ---
    build_bsp(game);
    build_portals(game);
    game.pvs_scratch.reserve_for(game.bsp.leaves.size(), kClusterCount,
                                 /*max portals*/ 6u);
    rebuild_pvs(game);

    game.combat.config.friendly_fire = gameplay::FriendlyFire::Off;
    game.combat.reserve(32u, 8u, 16u);
    game.ai.ctx.los = los_hook;
    game.ai.ctx.fire = fire_hook;
    game.ai.ctx.apply_move = apply_move_hook;

    PSY_LOG_INFO("duke_demo: {} rooms, {} enemies, hybrid render{}",
                 static_cast<u32>(kClusterCount), game.enemy_count,
                 smoke_frames > 0 ? std::string{" (smoke mode)"} : std::string{});

    u64 last_ticks = platform::Clock::ticks_now();
    u32 frame = 0;

    while (!window->should_close()) {
        window->poll_events();

        const u64 now_ticks = platform::Clock::ticks_now();
        auto* input = platform::input();
        if (input && input->key_down(platform::KeyCode::Escape) && !editor::overlays_capturing()) {
            PSY_LOG_INFO("duke_demo: escape pressed, exiting");
            break;
        }

        const f32 dt =
            (smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(now_ticks - last_ticks)));
        last_ticks = now_ticks;

        const editor::Mode edit_mode = app_host.engine_frame_update(dt);
        app_host.engine_frame_begin(app::FrameClear::color_depth(rgba8(16, 18, 24)));

        // 1. Player input or scripted smoke traverse: START -> HUB (opens vault)
        //    -> EAST, auto-firing at the nearest live enemy so the headless run
        //    exercises traverse + PVS cull + a kill.
        bool fire_pressed = false;
        math::Vec3 fire_dir = game.controller.forward();
        if (smoke_frames > 0) {
            const f32 t01 = std::clamp(static_cast<f32>(frame) / static_cast<f32>(smoke_frames),
                                       0.0f, 1.0f);
            // Piecewise path through START -> HUB -> EAST along the corridor spine.
            math::Vec3 p{};
            if (t01 < 0.4f) {
                const f32 u = t01 / 0.4f;  // START centre -> HUB centre
                p = {-9.0f + u * 9.5f, kFloorY + kEyeHeight, 0.0f};
            } else {
                const f32 u = (t01 - 0.4f) / 0.6f;  // HUB centre -> EAST centre
                p = {0.5f + u * 9.0f, kFloorY + kEyeHeight, 0.0f};
            }
            game.controller.set_position(p);
            game.controller.set_look(0.0f, 0.0f);
            if ((frame % 4u) == 0u && aim_at_nearest_enemy(game, fire_dir))
                fire_pressed = true;
        } else if (edit_mode != editor::Mode::Edit && input && !editor::overlays_capturing()) {
            game.controller.update(*input, dt);
            const bool left = input->mouse().left;
            fire_pressed = left && !game.prev_fire;
            game.prev_fire = left;
            fire_dir = game.controller.forward();
        }

        // 2. Physics (static world).
        game.world.step(dt);

        // 3. Sync player eye -> camera + combat entity.
        sync_player_transform(game);
        const math::Vec3 eye = game.controller.eye();

        // 3b. Vault trigger (opens once when the player reaches the HUB).
        (void)check_vault_trigger(game, eye);

        // 4. PVS CULL: locate the camera leaf, fetch its PVS row, flip room flags.
        game.cam_cluster = camera_cluster(game, eye);
        game.visible_leaves = apply_pvs_cull(game, game.cam_cluster);
        if (!game.logged_cull && game.visible_leaves < game.total_leaves) {
            PSY_LOG_INFO("duke_demo: PVS culled - visible {} / {} leaves (cam cluster {})",
                         game.visible_leaves, game.total_leaves, game.cam_cluster);
            game.logged_cull = true;
        }

        // 5-7. Combat tick: begin -> AI act/fire -> player fire -> flush/resolve.
        scene::EcsRegistry& registry = game.scene->registry();
        gameplay::CombatContext& cctx = game.combat.ctx;
        const gameplay::CombatConfig& ccfg = game.combat.config;

        cctx.begin_tick();
        gameplay::tick_weapon_cooldowns(registry, dt);
        gameplay::tick_projectiles(registry, dt, cctx, ccfg);
        game.ai.update(registry, dt);
        if (fire_pressed)
            player_fire(game, fire_dir);
        gameplay::flush_damage_events(registry, cctx, ccfg);
        gameplay::cleanup_projectiles(*game.scene, cctx);
        (void)gameplay::resolve_deaths(*game.scene, true, &on_death, &game);

        // 8. Render: hybrid shadow occluder build (Hybrid mode) then raster.
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
            u32 alive = 0u;
            for (u32 i = 0; i < game.enemy_count; ++i) {
                if (game.scene->registry().alive(game.enemies[i]))
                    ++alive;
            }
            // The smoke harness greps these lines. We assert (via the exit code
            // below) that the PVS genuinely culled at least one leaf at some
            // point and that the player scored at least one kill.
            const bool pvs_ok = game.logged_cull;
            const bool kill_ok = game.player_kills > 0u;
            PSY_LOG_INFO(
                "duke_demo: smoke target reached ({}); kills={} enemies_left={} "
                "vault_open={} pvs_culled={} -> {}",
                smoke_frames, game.player_kills, alive, game.vault_open ? 1 : 0,
                pvs_ok ? 1 : 0, (pvs_ok && kill_ok) ? "PASS" : "FAIL");
            g_game = nullptr;
            if (!pvs_ok || !kill_ok)
                return EXIT_FAILURE;
            const bool capture_ok = app_host.write_capture_if_requested("duke_demo");
            return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    g_game = nullptr;
    const bool capture_ok = app_host.write_capture_if_requested("duke_demo");
    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct DukeDemo {
    static constexpr std::string_view log_name() noexcept { return "duke_demo"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder Duke Demo"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    int run(app::WindowApp& app_host, const app::AppArgs& args) { return run_demo(args, app_host); }
};

PSYNDER_WINDOW_SAMPLE_MAIN(DukeDemo)
