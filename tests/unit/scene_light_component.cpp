// SPDX-License-Identifier: MIT
// Psynder - scene light component authoring contract.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "scene/SceneEcs.h"

#include <vector>

using namespace psynder;
using namespace psynder::scene;

TEST_CASE("scene: light component exposes stable authoring defaults",
          "[scene][light][component]") {
    LightComponent light{};

    REQUIRE(light.kind == LightKind::Point);
    REQUIRE(light.color_rgba8 == 0xFFFFFFFFu);
    REQUIRE(light.intensity == 3.0f);
    REQUIRE(light.range == 8.0f);
    REQUIRE(light.casts_shadow == 0u);
}

TEST_CASE("scene: light component can be authored and attached through registry.add",
          "[scene][light][component]") {
    auto& registry = EcsRegistry::Get();
    registry.clear();

    const Entity entity = registry.create();
    LightComponent authored{};
    authored.kind = LightKind::Spot;
    authored.color_rgba8 = 0x80FFE0C0u;
    authored.intensity = 12.5f;
    authored.range = 42.0f;
    authored.casts_shadow = 1u;

    registry.add<LightComponent>(entity, authored);

    LightComponent* stored = registry.get<LightComponent>(entity);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->kind == LightKind::Spot);
    REQUIRE(stored->color_rgba8 == 0x80FFE0C0u);
    REQUIRE(stored->intensity == 12.5f);
    REQUIRE(stored->range == 42.0f);
    REQUIRE(stored->casts_shadow == 1u);

    registry.clear();
}

TEST_CASE("scene: light gather publishes world-space authoring snapshot",
          "[scene][light][component]") {
    auto& registry = EcsRegistry::Get();
    registry.clear();
    registry.set_structural_deferred(false);

    Scene scene{registry};
    LocalTransform local{};
    local.translation = math::Vec3{2.0f, 3.0f, 4.0f};
    const Entity entity = scene.create_entity(local);

    LightComponent light{};
    light.kind = LightKind::Point;
    light.color_rgba8 = 0xFFB0F3FFu;
    light.intensity = 5.0f;
    light.range = 12.0f;
    light.casts_shadow = 1u;
    REQUIRE(scene.attach_light(entity, light));

    std::vector<SceneLightItem> lights;
    scene.update_transforms();
    scene.gather_lights(lights);

    REQUIRE(lights.size() == 1u);
    REQUIRE(lights[0].entity == entity);
    REQUIRE(lights[0].kind == LightKind::Point);
    REQUIRE(lights[0].color_rgba8 == light.color_rgba8);
    REQUIRE(lights[0].casts_shadow);
    REQUIRE_THAT(static_cast<double>(lights[0].position.x),
                 Catch::Matchers::WithinAbs(2.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(lights[0].position.y),
                 Catch::Matchers::WithinAbs(3.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(lights[0].position.z),
                 Catch::Matchers::WithinAbs(4.0, 1e-5));

    registry.clear();
}
