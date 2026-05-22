// SPDX-License-Identifier: MIT
// Psynder — scene entity authoring helpers for renderable objects.

#pragma once

#include "render/Material.h"
#include "render/RenderingSystem.h"

namespace psynder::render::entity_helpers {

[[nodiscard]] inline scene::RenderableComponent make_mesh_renderable(
    RenderingSystem& renderer,
    MeshId mesh,
    MaterialId material,
    scene::RenderableFlags flags = scene::RenderableFlags::Visible,
    math::Aabb local_bounds = math::aabb_empty(),
    scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
    return renderer.make_mesh_renderable(mesh, material, flags, local_bounds, mobility);
}

[[nodiscard]] inline SceneMeshEntity create_mesh_entity(
    RenderingSystem& renderer,
    scene::Scene& scene,
    const MeshDesc& mesh_desc,
    const scene::LocalTransform& local = {},
    scene::SceneNode parent = scene::kInvalidSceneNode,
    scene::RenderableFlags flags = scene::RenderableFlags::Visible,
    scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
    return renderer.create_mesh_entity(scene, mesh_desc, MaterialDesc{}, local, parent, flags, mobility);
}

[[nodiscard]] inline SceneMeshEntity create_mesh_entity(
    RenderingSystem& renderer,
    scene::Scene& scene,
    const MeshDesc& mesh_desc,
    MaterialId material,
    const scene::LocalTransform& local = {},
    scene::SceneNode parent = scene::kInvalidSceneNode,
    scene::RenderableFlags flags = scene::RenderableFlags::Visible,
    scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
    return renderer.create_mesh_entity(scene, mesh_desc, material, local, parent, flags, mobility);
}

[[nodiscard]] inline SceneMeshEntity create_mesh_entity(
    RenderingSystem& renderer,
    scene::Scene& scene,
    const MeshDesc& mesh_desc,
    const MaterialDesc& material,
    const scene::LocalTransform& local = {},
    scene::SceneNode parent = scene::kInvalidSceneNode,
    scene::RenderableFlags flags = scene::RenderableFlags::Visible,
    scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
    return renderer.create_mesh_entity(scene, mesh_desc, material, local, parent, flags, mobility);
}

[[nodiscard]] inline SceneMeshEntity create_textured_triangle(
    RenderingSystem& renderer,
    scene::Scene& scene,
    TextureAsset& texture,
    const scene::LocalTransform& local = {},
    scene::SceneNode parent = scene::kInvalidSceneNode,
    scene::RenderableFlags flags = scene::RenderableFlags::Visible,
    scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
    const MeshId mesh = renderer.builtin_mesh(BuiltInMesh::TexturedTriangle, &texture);
    return renderer.create_mesh_instance(scene, mesh, MaterialId{}, local, parent, flags, mobility);
}

}  // namespace psynder::render::entity_helpers
