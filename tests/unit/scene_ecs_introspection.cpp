// SPDX-License-Identifier: MIT
// Psynder — ECS source-of-truth introspection tests.

#include <catch2/catch_test_macros.hpp>

#include "scene/EcsRegistry.h"

#include <array>
#include <string_view>

namespace scene_ecs_introspection {

PSYNDER_COMPONENT(IntrospectPos) {
    psynder::f32 x = 0.0f;
    psynder::f32 y = 0.0f;
    psynder::f32 z = 0.0f;
};

PSYNDER_COMPONENT(IntrospectName) {
    char text[16]{};
};

}  // namespace scene_ecs_introspection

using namespace psynder;
using namespace psynder::scene;
using scene_ecs_introspection::IntrospectName;
using scene_ecs_introspection::IntrospectPos;

TEST_CASE("scene: registry snapshots live entities as source of truth",
          "[scene][ecs][introspection]") {
    auto& registry = EcsRegistry::Get();
    registry.clear();

    const Entity a = registry.create();
    const Entity b = registry.create();
    const Entity c = registry.create();
    registry.destroy(b);

    std::array<Entity, 4> entities{};
    const u32 total = registry.snapshot_live_entities(entities);
    REQUIRE(total == 2u);
    REQUIRE(registry.entity_count() == 2u);
    REQUIRE(registry.alive(entities[0]));
    REQUIRE(registry.alive(entities[1]));
    REQUIRE_FALSE(entities[2].valid());

    const bool saw_a = (entities[0] == a) || (entities[1] == a);
    const bool saw_c = (entities[0] == c) || (entities[1] == c);
    REQUIRE(saw_a);
    REQUIRE(saw_c);

    std::array<Entity, 1> tiny{};
    REQUIRE(registry.snapshot_live_entities(tiny) == 2u);
    REQUIRE(registry.alive(tiny[0]));

    registry.clear();
    REQUIRE(registry.entity_count() == 0u);
    REQUIRE_FALSE(registry.alive(a));
    REQUIRE(registry.snapshot_live_entities(entities) == 0u);
}

TEST_CASE("scene: registry snapshots entity component ids and metadata",
          "[scene][ecs][introspection]") {
    auto& registry = EcsRegistry::Get();
    registry.clear();

    const Entity e = registry.create();
    registry.add<IntrospectPos>(e, IntrospectPos{1.0f, 2.0f, 3.0f});
    registry.add<IntrospectName>(e, IntrospectName{"crate"});

    std::array<ComponentId, 4> components{};
    const u32 component_total = registry.snapshot_components(e, components);
    REQUIRE(component_total == 2u);
    REQUIRE(registry.component_count(e) == 2u);

    const ComponentId pos_id = component_id<IntrospectPos>();
    const ComponentId name_id = component_id<IntrospectName>();
    const bool saw_pos = (components[0] == pos_id) || (components[1] == pos_id);
    const bool saw_name = (components[0] == name_id) || (components[1] == name_id);
    REQUIRE(saw_pos);
    REQUIRE(saw_name);

    const ComponentTypeInfo pos_info = component_type_info(pos_id);
    REQUIRE(pos_info.id == pos_id);
    REQUIRE(pos_info.size == sizeof(IntrospectPos));
    REQUIRE(pos_info.align == alignof(IntrospectPos));
    REQUIRE(std::string_view{pos_info.name}.find("IntrospectPos") != std::string_view::npos);

    registry.remove<IntrospectName>(e);
    REQUIRE(registry.snapshot_components(e, components) == 1u);
    REQUIRE(components[0] == pos_id);

    registry.destroy(e);
    REQUIRE(registry.snapshot_components(e, components) == 0u);
    REQUIRE(registry.component_count(e) == 0u);
}

TEST_CASE("scene: registry reports and drains structural queue",
          "[scene][ecs][deferred]") {
    auto& registry = EcsRegistry::Get();
    registry.clear();

    const Entity e = registry.create();
    registry.set_structural_deferred(true);
    registry.add<IntrospectPos>(e, IntrospectPos{4.0f, 5.0f, 6.0f});
    registry.add<IntrospectName>(e, IntrospectName{"box"});
    REQUIRE(registry.pending_structural_change_count() == 2u);
    REQUIRE(registry.component_count(e) == 0u);

    registry.apply_structural_changes();
    registry.set_structural_deferred(false);
    REQUIRE(registry.pending_structural_change_count() == 0u);
    REQUIRE(registry.component_count(e) == 2u);

    registry.clear();
}
