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
#include "ai/NavBuild.h"
#include "asset/Vault.h"
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
#include "world/bsp/Bsp.h"
#include "world/bsp/BspDraw.h"
#include "world/bsp/BspFormat.h"
#include "world/bsp/Portal.h"
#include "world/bsp/PvsBuild.h"

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
    bool map_loaded = false;   // true => game.bsp came from the on-disk .psybsp
    bool vault_open = false;   // trigger toggles this -> rebuilds PVS (in-memory only)
    i32 cam_cluster = kCluStart;
    u32 visible_leaves = 0u;   // last frame's PVS-visible leaf count (HUD)
    u32 total_leaves = 0u;

    // --- BSP face geometry (W10-2) ---
    // Loaded from the same on-disk .psybsp; the per-frame PVS-culled DrawItem
    // stream that renders the level FROM the loaded BSP faces (replacing the
    // hand-authored wall shell). `bsp_draws` is reused each frame (warm capacity).
    world::bsp::BspGeometry bsp_geom{};
    std::vector<render::raster::DrawItem> bsp_draws{};
    bool bsp_geom_loaded = false;
    u32 total_faces = 0u;        // faces in the loaded BSP
    u32 visible_faces = 0u;      // last frame's PVS-culled visible face count
    bool logged_faces = false;   // one-shot "rendering BSP faces" log for smoke
    // W12-2: baked-lightmap stats (set at load from the .psybsp lightmap chunk).
    u32 lit_faces = 0u;          // faces carrying a baked lightmap.
    u32 lightmap_w = 0u;         // lumels across per lit face (N).
    u32 lightmap_h = 0u;         // lumels down per lit face (N).

    std::array<RoomMeshes, kClusterCount> rooms{};

    // --- AI NAVIGATION (W11-2) ---
    // A NavGrid over the whole level interior, built ONCE from the static room /
    // corridor / wall geometry. Cells inside a room or corridor are walkable;
    // solid space (and the cover pillars) is blocked, so an A* over this grid
    // routes a chasing soldier THROUGH the corridors / around the pillars toward
    // the player instead of straight-lining into a wall. The grid is host-owned;
    // ai.ctx.nav_grid points at it (opt-in). The corridor-gating door re-stamps
    // its own cells when it opens (carve walkable) so AI can advance past it.
    ai::NavGrid nav_grid{};
    f32 nav_agent_radius = 0.45f;
    bool nav_path_followed = false;   // an enemy followed a >=3-waypoint route.
    u32 nav_best_waypoints = 0u;      // longest routed path observed (HUD/log).
    bool logged_nav = false;          // one-shot "nav path followed" log.

    // --- DOOR / TRIGGER / PICKUP (W11-2) ---
    // A sliding slab on the HUB<->EAST corridor (C1). Closed it blocks the EAST
    // soldiers' LOS + nav path to the player; a TriggerVolume the player crosses
    // in the HUB opens it, clearing LOS + (re-carving) the nav so they advance.
    Entity corridor_door{};
    Entity corridor_trigger{};
    physics::BodyId door_body{};       // door's static LOS/bullet slab (moves w/ door).
    math::Vec3 door_closed_pos{};
    math::Vec3 door_half{};
    bool door_opened_logged = false;   // one-shot "door opened" log.
    bool door_nav_carved = false;      // true once the open door's cells are carved.
    bool trigger_fired = false;        // latched once the corridor trigger fires.

    Entity health_pickup{};            // +HP item on the player's spine path.
    Entity ammo_pickup{};              // +ammo item on the player's spine path.
    bool pickup_taken = false;         // latched once any pickup is granted.

    // Actor view (player + live enemies) fed to tick_triggers / tick_pickups.
    // Sized once; rebuilt in place each frame (no per-frame heap).
    std::array<gameplay::Actor, 1u + kMaxEnemies> actors{};
    u32 actor_count = 0u;
    std::array<gameplay::TriggerEvent, 8u> trigger_events{};
    std::array<gameplay::PickupEvent, 8u> pickup_events{};
    std::array<Entity, 8u> pickup_despawn{};

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

// --- AI navigation grid (W11-2) --------------------------------------------
// Build a uniform NavGrid over the whole level interior so chasing soldiers
// route THROUGH the corridors / around the cover pillars + the corridor door
// instead of straight-lining into walls. NavGrid starts all-walkable and the
// builder only OR-s blocked cells, so we invert: block the entire grid bounds,
// then carve every room/corridor interior back to walkable (shrunk by the agent
// radius so an agent keeps clearance from the walls), then re-block the cover
// pillars via build_nav_occupancy. The result: walkable == room/corridor space.
//
// Pillar obstacles, in the exact terms build_shell spawns them (see below). Kept
// as a named list so the door-carve + the pillar-stamp share one source.
constexpr std::array<math::Vec3, 2> kPillarCenters{{{0.5f, 0.0f, 0.0f}, {9.5f, 0.0f, 1.5f}}};
constexpr math::Vec3 kPillarHalf{0.45f, 1.5f, 0.45f};

// Carve a room/corridor interior to WALKABLE, shrunk inward by `inset` so an
// agent keeps clearance from the walls. Operates per-cell over the grid.
void nav_carve_walkable(ai::NavGrid& grid, math::Vec3 lo, math::Vec3 hi, f32 inset) {
    const f32 x0 = lo.x + inset;
    const f32 x1 = hi.x - inset;
    const f32 z0 = lo.z + inset;
    const f32 z1 = hi.z - inset;
    if (!(x1 > x0) || !(z1 > z0))
        return;
    const ai::NavCell c0 = grid.world_to_cell(math::Vec3{x0, 0.0f, z0});
    const ai::NavCell c1 = grid.world_to_cell(math::Vec3{x1, 0.0f, z1});
    for (i32 z = c0.z; z <= c1.z; ++z) {
        for (i32 x = c0.x; x <= c1.x; ++x) {
            const ai::NavCell c{x, z};
            // Only carve cells whose CENTRE actually lies inside the shrunk box,
            // so the walkable region matches the room interior exactly.
            const math::Vec3 wc = grid.cell_to_world(c);
            if (wc.x >= x0 && wc.x <= x1 && wc.z >= z0 && wc.z <= z1)
                grid.set_blocked(c, false);
        }
    }
}

void build_nav_grid(DukeGame& game) {
    using namespace ai;
    // Level bounds = union of all volumes, with a one-cell margin.
    f32 minx = 1e30f, minz = 1e30f, maxx = -1e30f, maxz = -1e30f;
    for (const Volume& v : kVolumes) {
        minx = std::min(minx, v.lo.x);
        minz = std::min(minz, v.lo.z);
        maxx = std::max(maxx, v.hi.x);
        maxz = std::max(maxz, v.hi.z);
    }
    constexpr f32 kCell = 0.5f;
    const math::Vec3 origin{minx - kCell, kFloorY + 0.85f, minz - kCell};
    const u32 w = static_cast<u32>(std::ceil((maxx - minx) / kCell)) + 2u;
    const u32 h = static_cast<u32>(std::ceil((maxz - minz) / kCell)) + 2u;
    game.nav_grid.resize(w, h, kCell, origin);

    // 1. Block the entire grid (everything starts solid).
    for (u32 z = 0; z < h; ++z)
        for (u32 x = 0; x < w; ++x)
            game.nav_grid.set_blocked(NavCell{static_cast<i32>(x), static_cast<i32>(z)}, true);

    // 2. Carve each room/corridor interior back to walkable. NO inset here: an
    //    inset on adjacent volumes would pull BOTH sides of a portal mouth away
    //    from the shared edge and leave a blocked gap at every junction, severing
    //    connectivity. We instead carve the full interior (solid space stays
    //    blocked from step 1) and get wall/cover clearance from the radius-grown
    //    pillar + door occupancy in steps 3-4.
    for (const Volume& v : kVolumes)
        nav_carve_walkable(game.nav_grid, v.lo, v.hi, 0.0f);

    // 3. Re-block the cover pillars so AI routes AROUND them (agent-radius grown).
    std::array<NavBox, kPillarCenters.size()> pillars{};
    for (usize i = 0; i < kPillarCenters.size(); ++i)
        pillars[i] = NavBox{kPillarCenters[i], kPillarHalf, 0.0f};
    (void)build_nav_occupancy(game.nav_grid, std::span<const NavBox>{pillars.data(), pillars.size()},
                              game.nav_agent_radius);

    // 4. Block the CLOSED corridor door's cells so the EAST soldiers cannot route
    //    to the player until the player's HUB trigger opens it (then nav_open_door
    //    re-carves these cells walkable). The door is authored before this call.
    if (game.corridor_door.valid()) {
        const NavBox dbox{game.door_closed_pos,
                          math::Vec3{game.door_half.x, kPillarHalf.y, game.door_half.z}, 0.0f};
        (void)build_nav_occupancy(game.nav_grid, dbox, game.nav_agent_radius);
    }

    PSY_LOG_INFO("duke_demo: nav grid {}x{} cells @ {:.2f}m (agent r={:.2f})", w, h, kCell,
                 static_cast<double>(game.nav_agent_radius));
}

// Re-carve the corridor-door cells to WALKABLE once the door opens, so a chasing
// soldier can route past it. Alloc-free (per-cell writes into the host grid).
void nav_open_door(DukeGame& game) {
    if (game.door_nav_carved)
        return;
    const math::Vec3 lo{game.door_closed_pos.x - game.door_half.x, kFloorY,
                        game.door_closed_pos.z - game.door_half.z};
    const math::Vec3 hi{game.door_closed_pos.x + game.door_half.x, kCeilY,
                        game.door_closed_pos.z + game.door_half.z};
    // Carve with no inset (the corridor is already narrow) so the full mouth opens.
    nav_carve_walkable(game.nav_grid, lo, hi, 0.0f);
    game.door_nav_carved = true;
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

// Mount the asset roots that hold the baked .psybsp. We try the build-tree dir
// first (where the CMake bake step writes via lm_qbsp --rooms), then the source
// tree (so a hand-run `lm_qbsp --rooms` also resolves). Idempotent: mounting a
// missing dir is a no-op. Defined via the duke CMakeLists compile definitions.
void mount_bsp_assets() {
#if defined(DUKE_BSP_ASSET_DIR)
    (void)asset::Vault::Get().mount_directory(DUKE_BSP_ASSET_DIR);
#endif
#if defined(DUKE_BSP_SOURCE_DIR)
    (void)asset::Vault::Get().mount_directory(DUKE_BSP_SOURCE_DIR);
#endif
}

// Load the REAL on-disk BSP asset (engine PBSP v1: leaves + clusters + portals
// + baked PVS) into game.bsp via the runtime loader. On success the demo uses
// the loaded leaves/clusters/PVS for culling (no in-memory build, no runtime
// flood). Returns true if the map loaded; false leaves game.bsp untouched so
// the caller can fall back to the in-memory authoring path.
bool load_bsp_asset(DukeGame& game) {
    using namespace world::bsp;
    mount_bsp_assets();

#if defined(DUKE_BSP_VPATH)
    constexpr std::string_view kVPath = DUKE_BSP_VPATH;
#else
    constexpr std::string_view kVPath = "maps/duke_e1m1.psybsp";
#endif

    BspMap loaded{};
    if (!load(kVPath, loaded)) {
        PSY_LOG_WARN("duke_demo: on-disk BSP '{}' not found/invalid - falling back to in-memory map",
                     kVPath);
        return false;
    }

    game.bsp = std::move(loaded);
    game.map_loaded = true;
    game.total_leaves = static_cast<u32>(game.bsp.leaves.size());
    game.total_faces = static_cast<u32>(game.bsp.faces.size());

    // W10-2: load the face geometry (vertices + indices) from the SAME blob so we
    // can render the level FROM the BSP. The loader above fills nodes/leaves/
    // faces/pvs; this companion fills the vertex+index slabs. Reserve the draw
    // stream to the face count so the per-frame build is alloc-free.
    game.bsp_geom_loaded = load_geometry(kVPath, game.bsp_geom);
    if (game.bsp_geom_loaded) {
        game.bsp_draws.reserve(game.total_faces);
        PSY_LOG_INFO("duke_demo: loaded BSP geometry - {} faces, {} verts, {} indices",
                     game.total_faces, game.bsp_geom.vertices.size(), game.bsp_geom.indices.size());
        // W12-2: load the baked per-face lightmaps from the SAME blob. The
        // loader decodes the on-disk RGB16F lumels into RGBA8 chunks that
        // BspDraw wires into each lit face's DrawItem, so the rooms render LIT
        // (brighter near the baked lights, darker in corners). An unlit blob
        // leaves these empty and the faces stay full-bright (no regression).
        if (load_lightmaps(kVPath, game.total_faces, game.bsp_geom)) {
            game.lit_faces = 0u;
            for (u32 v : game.bsp_geom.face_lightmap)
                if (v != world::bsp::BspGeometry::kNoLightmap)
                    ++game.lit_faces;
            if (!game.bsp_geom.lightmaps.empty()) {
                game.lightmap_w = game.bsp_geom.lightmaps[0].width;
                game.lightmap_h = game.bsp_geom.lightmaps[0].height;
            }
            PSY_LOG_INFO("duke_demo: lightmap baked {}x{} lumels/face, {} lit faces ({} RGBA8 texels)",
                         game.lightmap_w, game.lightmap_h, game.lit_faces,
                         static_cast<u32>(game.bsp_geom.lightmap_texels.size()));
        }
    } else {
        PSY_LOG_WARN("duke_demo: BSP '{}' carries no renderable geometry (faces={})", kVPath,
                     game.total_faces);
    }

    // Derive the PVS row width from the baked table: row_bytes == ceil(C/8),
    // and pvs.size() == clusters * row_bytes. Recover clusters from max leaf
    // cluster id (matches the loader's own walk_visible_leaves derivation).
    i32 max_cluster = 0;
    for (const BspLeaf& l : game.bsp.leaves) {
        if (l.cluster > max_cluster)
            max_cluster = l.cluster;
    }
    const u32 cluster_count = static_cast<u32>(max_cluster + 1);
    game.pvs_row_bytes =
        (cluster_count > 0u && !game.bsp.pvs.empty())
            ? static_cast<u32>(game.bsp.pvs.size()) / cluster_count
            : 0u;

    PSY_LOG_INFO("duke_demo: loaded on-disk BSP '{}' - {} leaves, {} clusters, {} pvs bytes/row",
                 kVPath, game.bsp.leaves.size(), cluster_count, game.pvs_row_bytes);
    return true;
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

// Locate the camera's leaf/cluster. When the map was loaded from disk we resolve
// against the LOADED leaf bounds + cluster ids (the on-disk asset is the source
// of truth); otherwise we use the in-memory kVolumes. A tiny skin lets the eye
// height + wall thickness not eject us; a nearest-centre fallback keeps a point
// in a doorway resolving. Integer-deterministic.
i32 camera_cluster(const DukeGame& game, math::Vec3 eye) {
    constexpr f32 kSkin = 0.6f;
    if (game.map_loaded) {
        i32 best = kCluStart;
        f32 best_d = 1e30f;
        for (const world::bsp::BspLeaf& l : game.bsp.leaves) {
            if (l.cluster < 0)
                continue;  // skip solid leaves
            if (eye.x >= l.bounds.min.x - kSkin && eye.x <= l.bounds.max.x + kSkin &&
                eye.z >= l.bounds.min.z - kSkin && eye.z <= l.bounds.max.z + kSkin) {
                return l.cluster;
            }
            const f32 cx = 0.5f * (l.bounds.min.x + l.bounds.max.x);
            const f32 cz = 0.5f * (l.bounds.min.z + l.bounds.max.z);
            const f32 dx = eye.x - cx;
            const f32 dz = eye.z - cz;
            const f32 d = dx * dx + dz * dz;
            if (d < best_d) {
                best_d = d;
                best = l.cluster;
            }
        }
        return best;
    }
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

// --- BSP face render (W10-2) -----------------------------------------------
// Build the PVS-culled DrawItem stream from the LOADED BSP faces. We walk the
// camera's PVS via world::bsp::walk_visible_leaves (the same baked PVS the cull
// pass uses) and emit one DrawItem per face of each visible leaf via BspDraw's
// build_leaf_draws. Faces of culled leaves are never touched -> the visible-face
// count is strictly less than the total when any room is culled. Returns the
// visible-face count; fills game.bsp_draws (warm capacity, no per-frame alloc in
// the steady state).
struct BspDrawAccum {
    DukeGame* game;
};
void accum_leaf_draws(const world::bsp::BspLeaf& leaf, void* user) {
    auto* acc = static_cast<BspDrawAccum*>(user);
    world::bsp::BspMaterialResolve resolve{};  // pass the raw material id through
    world::bsp::build_leaf_draws(acc->game->bsp, acc->game->bsp_geom, leaf, resolve,
                                 acc->game->bsp_draws);
}

u32 build_bsp_face_draws(DukeGame& game, math::Vec3 eye) {
    game.bsp_draws.clear();
    if (!game.bsp_geom_loaded || game.bsp.faces.empty())
        return 0u;
    BspDrawAccum acc{&game};
    // PVS-culled walk: only visible leaves' faces are emitted.
    world::bsp::walk_visible_leaves(game.bsp, eye, &accum_leaf_draws, &acc);
    // Two-sided + full-bright vertex colour so the inward-facing walls read from
    // inside the room regardless of the unlit BSP draw pass (no scene lights are
    // threaded into this secondary pass; flat/unlit is in scope, lightmaps are
    // deferred). The vertex colour already carries the per-room tint baked by
    // lm_qbsp; the albedo tint stays white (identity) so the colour shows.
    for (render::raster::DrawItem& di : game.bsp_draws)
        di.cull = render::raster::CullMode::None;
    return static_cast<u32>(game.bsp_draws.size());
}

// Render the PVS-culled BSP faces in a SECONDARY raster pass over the same frame.
// The engine scene pass already populated the depth buffer; the rasterizer's
// begin_frame does NOT clear, so this pass depth-tests against the scene and the
// inward walls composite correctly. Uses the active camera's view/projection.
void render_bsp_faces(DukeGame& game, app::WindowApp& app_host) {
    if (game.bsp_draws.empty())
        return;
    scene::Scene* scene = app_host.active_scene();
    if (scene == nullptr)
        return;
    render::Framebuffer& fb = app_host.framebuffer();
    const f32 aspect =
        (fb.height > 0u) ? static_cast<f32>(fb.width) / static_cast<f32>(fb.height) : 1.0f;
    scene::SceneCameraView camera{};
    if (!scene->active_camera_view(aspect, camera))
        return;
    render::raster::ViewState view{};
    view.target = fb;
    view.view = camera.view;
    view.projection = camera.projection;
    view.tile_w = camera.tile_w;
    view.tile_h = camera.tile_h;
    (void)app_host.rendering_system().render_raster_draws(view, game.bsp_draws);
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
    // Room-scale sight so a soldier only acquires the player when genuinely close
    // (clear LOS in the same room/corridor). Far rooms PATROL until then, so the
    // navigate pass routes their multi-room patrol around the cover pillars rather
    // than chasing a never-seen target toward a degenerate last-seen origin.
    agent.sight_range = 13.0f;
    agent.fov_cos = -1.0f;
    agent.attack_range = 11.0f;
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

    // W11-2: a NavAgentComponent makes this soldier FOLLOW an A* route (planned by
    // ai::navigate against game.nav_grid) instead of straight-line steering. The
    // navigate pass routes around walls / the cover pillars / the corridor door.
    ai::NavAgentComponent nav{};
    nav.repath_interval = 0.35f;
    nav.repath_dist = 1.5f;
    nav.arrive_radius = 0.5f;
    nav.separation_radius = 0.8f;
    nav.separation_weight = 1.0f;
    game.scene->registry().add<ai::NavAgentComponent>(e, ai::sanitize_nav_agent(nav));

    game.enemies[game.enemy_count++] = e;
}

void spawn_enemies(DukeGame& game) {
    render::MeshId cube = game.renderer->builtin_mesh(render::BuiltInMesh::UnitCube);
    const render::MaterialId enemy_mat = make_material(*game.scene, rgba8(210, 70, 60));
    const f32 ey = kFloorY + 0.85f;
    const Volume& east = volume_for(kCluEast);
    const Volume& hub = volume_for(kCluHub);
    // Two in EAST, one in HUB, one in VAULT (revealed after the door opens). The
    // EAST soldiers patrol corner-to-corner ACROSS the EAST cover pillar (at
    // {9.5,1.5}); since their patrol line is blocked by the pillar, the navigate
    // pass must route an A* path AROUND it -> a genuine multi-waypoint route the
    // soldier follows, even before the player is in sight. (Once the player
    // reaches EAST through the opened door they additionally Chase him.)
    spawn_enemy(game, cube, enemy_mat, {east.lo.x + 1.0f, ey, -2.5f},
                {{{east.lo.x + 1.0f, ey, -2.5f}, {east.hi.x - 1.0f, ey, 2.5f}}});
    spawn_enemy(game, cube, enemy_mat, {east.hi.x - 1.0f, ey, -2.5f},
                {{{east.hi.x - 1.0f, ey, -2.5f}, {east.lo.x + 1.0f, ey, 2.5f}}});
    spawn_enemy(game, cube, enemy_mat, {2.0f, ey, -2.5f},
                {{{2.0f, ey, -2.5f}, {2.0f, ey, 2.5f}}});
    const Volume& vault = volume_for(kCluVault);
    spawn_enemy(game, cube, enemy_mat,
                {0.5f * (vault.lo.x + vault.hi.x), ey, vault.hi.z - 1.5f},
                {{{vault.lo.x + 1.0f, ey, vault.hi.z - 1.5f},
                  {vault.hi.x - 1.0f, ey, vault.hi.z - 1.5f}}});
    (void)hub;
}

// --- DOOR / TRIGGER / PICKUP authoring (W11-2) ------------------------------
// A trigger-opened door on the HUB<->EAST corridor (C1) plus a health and an
// ammo pickup on the player's spine path. The smoke player walks START -> HUB ->
// EAST along z=0; crossing the HUB trigger opens the C1 door (clearing the EAST
// soldiers' LOS + nav route to the player) and the spine overlaps both pickups.
void build_mechanics(DukeGame& game) {
    render::MeshId cube = game.renderer->builtin_mesh(render::BuiltInMesh::UnitCube);

    // -- Corridor door: a sliding slab spanning the C1 corridor mouth (x~5, z=0).
    //    Closed it blocks LOS + bullets (combat hitbox) + the EAST nav route; the
    //    HUB trigger slides it up out of the way. --
    const render::MaterialId door_mat = make_material(*game.scene, rgba8(150, 110, 60));
    const f32 door_h = kCeilY - kFloorY;
    const math::Vec3 door_closed{5.0f, kFloorY + 0.5f * door_h, 0.0f};
    const math::Vec3 door_open = math::add(door_closed, math::Vec3{0.0f, door_h + 0.2f, 0.0f});
    const math::Vec3 door_half{0.3f, 0.5f * door_h, 1.2f};  // spans the C1 width (z +/-1.2)
    game.door_closed_pos = door_closed;
    game.door_half = door_half;
    {
        scene::LocalTransform local{};
        local.translation = door_closed;
        local.scale = {2.0f * door_half.x, 2.0f * door_half.y, 2.0f * door_half.z};
        game.corridor_door =
            game.scene->spawn_mesh_instance(cube, door_mat, local, scene::kInvalidSceneNode,
                                            scene::RenderableFlags::Visible,
                                            scene::ObjectMobility::Dynamic);
        // Record the door under the C1 room so the PVS pass shows/hides it with C1.
        RoomMeshes& c1 = game.rooms[kCluC1];
        if (c1.count < c1.entities.size())
            c1.entities[c1.count++] = game.corridor_door;

        // Combat hitbox: a gameplay raycast (player hitscan / probe) is blocked when
        // closed; tick_doors disables it when fully open.
        gameplay::HitboxComponent hb{};
        hb.radius = 0.0f;  // AABB
        hb.half_extent = door_half;
        hb.enabled = 1u;
        game.scene->registry().add<gameplay::HitboxComponent>(game.corridor_door,
                                                              gameplay::sanitize_hitbox(hb));

        gameplay::DoorComponent dc{};
        dc.closed_pos = door_closed;
        dc.open_pos = door_open;
        dc.move_time = 0.5f;
        dc.hitbox_entity = game.corridor_door;
        game.scene->registry().add<gameplay::DoorComponent>(game.corridor_door,
                                                           gameplay::sanitize_door(dc));

        // Static physics slab so the AI physics-raycast LOS is blocked when closed;
        // moved with the door each tick (set_body_position) so LOS tracks it.
        physics::BodyDesc desc{};
        desc.shape = physics::Shape::Box;
        desc.mass = 0.0f;
        desc.position = door_closed;
        desc.half_extent = door_half;
        game.door_body = game.world.create_body(desc);
    }

    // -- Trigger: a volume at the HUB centre the player crosses, opening the door. --
    {
        scene::LocalTransform local{};
        local.translation = {0.5f, kFloorY + 1.0f, 0.0f};
        game.corridor_trigger = game.scene->create_entity(local);
        gameplay::TriggerVolumeComponent tv{};
        tv.radius = 1.8f;
        tv.fire_faction = kFactionPlayer;  // the player opens it
        tv.target = game.corridor_door;
        tv.open_target_door = 1u;
        game.scene->registry().add<gameplay::TriggerVolumeComponent>(
            game.corridor_trigger, gameplay::sanitize_trigger_volume(tv));
    }

    // -- Health pickup: a green box on the START->HUB spine (x=-4, z=0). --
    {
        const render::MaterialId hp_mat = make_material(*game.scene, rgba8(80, 220, 120));
        scene::LocalTransform local{};
        local.translation = {-4.0f, kFloorY + 1.0f, 0.0f};
        local.scale = {0.4f, 0.4f, 0.4f};
        game.health_pickup =
            game.scene->spawn_mesh_instance(cube, hp_mat, local, scene::kInvalidSceneNode,
                                            scene::RenderableFlags::Visible,
                                            scene::ObjectMobility::Dynamic);
        RoomMeshes& c0 = game.rooms[kCluC0];
        if (c0.count < c0.entities.size())
            c0.entities[c0.count++] = game.health_pickup;
        gameplay::PickupComponent pk{};
        pk.kind = gameplay::PickupKind::Health;
        pk.amount = 25.0f;
        pk.radius = 1.6f;
        pk.pickup_faction = kFactionPlayer;
        game.scene->registry().add<gameplay::PickupComponent>(game.health_pickup,
                                                             gameplay::sanitize_pickup(pk));
    }

    // -- Ammo pickup: a yellow box on the HUB->EAST spine (x=4, z=0), past the door. --
    {
        const render::MaterialId am_mat = make_material(*game.scene, rgba8(230, 210, 70));
        scene::LocalTransform local{};
        local.translation = {2.0f, kFloorY + 1.0f, 0.0f};
        local.scale = {0.4f, 0.4f, 0.4f};
        game.ammo_pickup =
            game.scene->spawn_mesh_instance(cube, am_mat, local, scene::kInvalidSceneNode,
                                            scene::RenderableFlags::Visible,
                                            scene::ObjectMobility::Dynamic);
        RoomMeshes& hub = game.rooms[kCluHub];
        if (hub.count < hub.entities.size())
            hub.entities[hub.count++] = game.ammo_pickup;
        gameplay::PickupComponent pk{};
        pk.kind = gameplay::PickupKind::Ammo;
        pk.amount = 30.0f;
        pk.radius = 1.6f;
        pk.pickup_faction = kFactionPlayer;
        game.scene->registry().add<gameplay::PickupComponent>(game.ammo_pickup,
                                                             gameplay::sanitize_pickup(pk));
    }
}

// Rebuild the actor view (player + live enemies) for the trigger / pickup ticks.
void refresh_actors(DukeGame& game) {
    game.actor_count = 0u;
    game.actors[game.actor_count++] = gameplay::Actor{game.player, kFactionPlayer};
    for (u32 i = 0; i < game.enemy_count; ++i) {
        if (game.scene->registry().alive(game.enemies[i]))
            game.actors[game.actor_count++] = gameplay::Actor{game.enemies[i], kFactionEnemy};
    }
}

// One tick of the door/trigger/pickup mechanics: triggers -> doors -> pickups,
// driving the door's physics slab + mesh + re-carving the nav when it opens, and
// logging the events the smoke gate looks for. Alloc-free (fixed-capacity spans).
void tick_mechanics(DukeGame& game, f32 dt) {
    scene::EcsRegistry& registry = game.scene->registry();
    refresh_actors(game);
    const std::span<const gameplay::Actor> actors{game.actors.data(), game.actor_count};

    // Triggers: a player entry opens the linked door (sets open_request).
    u32 tev = 0u;
    (void)gameplay::tick_triggers(registry, actors, game.trigger_events, &tev);
    for (u32 i = 0; i < tev; ++i) {
        const gameplay::TriggerEvent& e = game.trigger_events[i];
        game.trigger_fired = true;
        PSY_LOG_INFO("duke_demo: trigger fired (entry #{}) -> corridor door opening", e.fire_count);
    }

    // Doors: advance the slab animation + gate the combat hitbox.
    gameplay::tick_doors(registry, dt);

    // Drive the door's animated position into the mesh + physics slab, and once it
    // is fully open re-carve its nav cells so the EAST soldiers can route through.
    if (const auto* d = registry.get<gameplay::DoorComponent>(game.corridor_door)) {
        if (auto* t = registry.get<scene::TransformComponent>(game.corridor_door)) {
            scene::LocalTransform local = t->local;
            local.translation = d->position;
            (void)game.scene->set_transform(game.corridor_door, local);
        }
        if (game.door_body.valid())
            game.world.set_body_position(game.door_body, d->position);
        if (d->state == gameplay::DoorState::Open) {
            nav_open_door(game);  // carve the corridor cells walkable (one-shot).
            if (!game.door_opened_logged) {
                game.door_opened_logged = true;
                PSY_LOG_INFO("duke_demo: corridor door opened (LOS + nav path clear to EAST)");
            }
        }
    }

    // Pickups: grant on overlap, then despawn the spent items.
    u32 pev = 0u;
    u32 desp = 0u;
    (void)gameplay::tick_pickups(registry, actors, game.pickup_despawn, &desp, game.pickup_events,
                                 &pev);
    for (u32 i = 0; i < pev; ++i) {
        const gameplay::PickupEvent& e = game.pickup_events[i];
        game.pickup_taken = true;
        const char* kind = (e.kind == gameplay::PickupKind::Health) ? "health"
                           : (e.kind == gameplay::PickupKind::Ammo)  ? "ammo"
                                                                     : "weapon";
        PSY_LOG_INFO("duke_demo: picked up {} {:.0f}->{:.0f}", kind,
                     static_cast<double>(e.before), static_cast<double>(e.after));
    }
    for (u32 i = 0; i < desp; ++i)
        (void)game.scene->despawn_entity(game.pickup_despawn[i]);
}

// Poll the enemies' routed nav state: latch nav_path_followed once any soldier is
// actually FOLLOWING a multi-waypoint A* route (has_path with >=3 waypoints and a
// cursor that has advanced past the start cell). Proves the AI navigates rather
// than steers straight. Read-only over the NavAgentComponent the navigate pass
// wrote; alloc-free.
void poll_nav_stat(DukeGame& game) {
    for (u32 i = 0; i < game.enemy_count; ++i) {
        const Entity e = game.enemies[i];
        if (!game.scene->registry().alive(e))
            continue;
        const auto* nav = game.scene->registry().get<ai::NavAgentComponent>(e);
        if (nav == nullptr || nav->has_path == 0u)
            continue;
        if (nav->count > game.nav_best_waypoints)
            game.nav_best_waypoints = nav->count;
        if (nav->count >= 3u && nav->cursor > 0u) {
            if (!game.nav_path_followed) {
                game.nav_path_followed = true;
                PSY_LOG_INFO("duke_demo: AI followed a multi-waypoint nav route - {} waypoints, "
                             "cursor {} (enemy {})",
                             nav->count, nav->cursor, i);
            }
        }
    }
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
    char line[200];
    std::snprintf(line, sizeof(line),
                  "HP %3.0f  ENEMIES %u  KILLS %u  PVS %u/%u  FACES %u/%u  NAV %u  DOOR %s",
                  static_cast<double>(health), alive, game.player_kills, game.visible_leaves,
                  game.total_leaves, game.visible_faces, game.total_faces, game.nav_best_waypoints,
                  game.door_opened_logged ? "OPEN" : "SHUT");

    ui::imm::begin_frame(fb);
    ui::imm::filled_rect(math::Vec2{8.0f, 8.0f}, math::Vec2{520.0f, 22.0f},
                         ui::imm::rgba(0x0B, 0x10, 0x18, 0xC0));
    ui::imm::rect_outline(math::Vec2{8.0f, 8.0f}, math::Vec2{520.0f, 22.0f},
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
// vault. In the in-memory fallback path this rebuilds the portal set + floods a
// fresh PVS so the sealed vault becomes visible. With the on-disk map the PVS is
// statically baked (the .rooms source seals the vault chain on purpose so the
// loaded PVS culls it), so we only flag the door open for the HUD; dynamic re-
// baking of a loaded PVS is deferred (see file notes). Returns true the frame
// the door opens.
bool check_vault_trigger(DukeGame& game, math::Vec3 eye) {
    if (game.vault_open)
        return false;
    if (camera_cluster(game, eye) == kCluHub) {
        game.vault_open = true;
        if (!game.map_loaded) {
            build_portals(game);
            rebuild_pvs(game);
        }
        PSY_LOG_INFO("duke_demo: HUB trigger reached - VAULT door opened{}",
                     game.map_loaded ? " (HUD only; baked PVS keeps vault sealed)" : "");
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
        .scene_entities = 112u,  // + door / trigger / health+ammo pickups (W11-2)
        .renderables = 56u,
        .cameras = 2u,
        .lights = 6u,
        .render_items = 56u,
    });
    app_host.reserve_scene_capacity(56u, 12u);
    game.walk_volumes.reserve(kClusterCount);

    // --- Level geometry (visual rooms + physics shell) ---
    render::MeshId cube = game.renderer->builtin_mesh(render::BuiltInMesh::UnitCube);
    for (const Volume& v : kVolumes)
        build_volume(game, cube, v);
    build_shell(game, cube);
    make_lights(game);
    spawn_player(game);
    spawn_enemies(game);
    build_mechanics(game);  // W11-2: corridor door + trigger + health/ammo pickups
    build_nav_grid(game);   // W11-2: nav grid over the interior (after the door)

    // --- BSP + portals + PVS ---
    // PRIMARY PATH: load the REAL on-disk .psybsp built offline by lm_qbsp from
    // assets/maps/duke_e1m1.rooms. It carries the leaves/clusters/portals and a
    // BAKED PVS, so we use them directly - no in-memory authoring, no runtime
    // flood. FALLBACK: if the asset is missing, assemble the in-memory map +
    // flood the PVS at runtime (the pre-W9-1 behaviour) so the demo still runs.
    if (!load_bsp_asset(game)) {
        build_bsp(game);
        build_portals(game);
        game.pvs_scratch.reserve_for(game.bsp.leaves.size(), kClusterCount,
                                     /*max portals*/ 6u);
        rebuild_pvs(game);
    }

    game.combat.config.friendly_fire = gameplay::FriendlyFire::Off;
    game.combat.reserve(32u, 8u, 16u);
    game.ai.ctx.los = los_hook;
    game.ai.ctx.fire = fire_hook;
    game.ai.ctx.apply_move = apply_move_hook;
    game.ai.ctx.nav_grid = &game.nav_grid;  // W11-2: opt-in A* path navigation

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

        // 4b. BSP FACE STREAM (W10-2): build the PVS-culled DrawItem stream FROM
        //     the loaded BSP faces. Culled leaves contribute no faces, so the
        //     visible-face count drops below the total whenever a room is culled.
        game.visible_faces = build_bsp_face_draws(game, eye);
        if (!game.logged_faces && game.bsp_geom_loaded && game.visible_faces < game.total_faces) {
            PSY_LOG_INFO("duke_demo: BSP faces rendered - visible {} / {} faces, {} draw items "
                         "(cam cluster {})",
                         game.visible_faces, game.total_faces,
                         static_cast<u32>(game.bsp_draws.size()), game.cam_cluster);
            game.logged_faces = true;
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

        // 7b. W11-2 mechanics: triggers -> doors -> pickups (player grabs the
        //     health/ammo items; crossing the HUB trigger slides the C1 door open,
        //     clearing LOS + re-carving the nav so the EAST soldiers advance), then
        //     poll the soldiers' routed nav state for the multi-waypoint stat.
        tick_mechanics(game, dt);
        poll_nav_stat(game);

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
        // W10-2: composite the PVS-culled BSP face geometry over the scene pass
        // (same depth buffer; begin_frame does not clear) so the level is drawn
        // FROM the loaded BSP. Runs after the engine scene pass, before the HUD.
        render_bsp_faces(game, app_host);
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
            // below) that the REAL on-disk map loaded, the PVS genuinely culled
            // at least one leaf, the level renders FROM the loaded BSP faces with
            // the PVS visibly culling some (visible_faces < total_faces > 0), the
            // AI NAVIGATED a multi-waypoint route, the corridor TRIGGER fired (door
            // opened), a PICKUP was taken, AND the player scored a kill.
            const bool map_ok = game.map_loaded;
            const bool pvs_ok = game.logged_cull;
            const bool kill_ok = game.player_kills > 0u;
            const bool faces_ok = game.bsp_geom_loaded && game.total_faces > 0u &&
                                  game.visible_faces > 0u && game.logged_faces;
            const bool nav_ok = game.nav_path_followed;
            const bool trig_ok = game.trigger_fired && game.door_opened_logged;
            const bool pickup_ok = game.pickup_taken;
            const bool pass = map_ok && pvs_ok && kill_ok && faces_ok && nav_ok && trig_ok &&
                              pickup_ok;
            PSY_LOG_INFO(
                "duke_demo: smoke target reached ({}); map_loaded={} kills={} enemies_left={} "
                "vault_open={} pvs_culled={} bsp_faces={}/{} lightmap={}x{}_lumels/face_{}_lit_faces "
                "nav_path_followed={} (max_wp={}) trigger_fired={} pickup_taken={} -> {}",
                smoke_frames, map_ok ? 1 : 0, game.player_kills, alive, game.vault_open ? 1 : 0,
                pvs_ok ? 1 : 0, game.visible_faces, game.total_faces,
                game.lightmap_w, game.lightmap_h,
                game.lit_faces, nav_ok ? 1 : 0,
                game.nav_best_waypoints, trig_ok ? 1 : 0, pickup_ok ? 1 : 0, pass ? "PASS" : "FAIL");
            g_game = nullptr;
            if (!pass)
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
