// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE physics components (DESIGN.md SS10.1, editor-only
// direction 2026-05-26).
//
// The authoring physics components (RigidBodyComponent / VehicleComponent /
// HelicopterComponent / CharacterControllerComponent) were PROMOTED to
// engine/scene so the scene serializer can persist them and the editor
// Inspector (scene-level only) can see them. They now live in
// scene/PhysicsComponents.h as psynder::scene::* PODs that reference NO
// physics:: type (collider shape is scene::ColliderShape; runtime handles are
// opaque u32).
//
// This header is kept as a thin compatibility include so existing
// editor/play/* includes keep resolving; the physics<->u32 mapping lives in
// PlayRuntime, the single place that includes physics/Physics.h.

#pragma once

#include "scene/PhysicsComponents.h"
