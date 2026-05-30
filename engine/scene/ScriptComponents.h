// SPDX-License-Identifier: MIT
// Psynder - SCENE-LEVEL authoring visual-scripting component (no-code editor
// authoring lane). Sibling to GameplayComponents.h / PhysicsComponents.h.
//
// A designer authors a PsyGraph (the visual-script node graph) in the editor's
// graph panel; that authored graph must (a) live with the scene, (b) round-trip
// through the .psyscene file, and (c) be compiled + run during Play. This header
// owns the scene-level link between an entity and its authored graph.
//
// CRITICAL LAYERING RULE (same as GameplayComponents.h): engine/scene must NOT
// depend on engine/script. The PsyGraph VM / Compiler / Serializer live in
// engine/script/psygraph, which itself depends on engine/scene (never the
// reverse). So engine/scene cannot reference psygraph::Graph. Instead the
// authored graph is stored as an OPAQUE serialized blob (the byte output of
// psygraph::serialize_graph) on a Scene-owned side table (see Scene::
// add_script_graph / script_graph). This component only carries the small POD
// SLOT INDEX into that side table; it is trivially copyable, as the ECS requires.
//
// The scene<->psygraph mapping (deserialize the blob, compile it, bind a VM
// instance) happens only at the Play boundary inside editor/play/PlayRuntime,
// which DOES include script/psygraph. The editor (player/main.cpp) authors the
// blob over the main-thread IPC and stores it into the scene; the SceneFile
// serializer persists it (v5 SSCG/SCGB chunks).

#pragma once

#include "core/Types.h"
#include "scene/EcsRegistry.h"

namespace psynder::scene {

// "no graph" sentinel for ScriptGraphComponent::graph_slot.
inline constexpr u32 kInvalidScriptGraphSlot = 0xFFFFFFFFu;

// POD link: which Scene-owned authored-graph blob this entity runs. The blob is
// opaque (psygraph serialized bytes) and lives in Scene's script_graphs_ side
// table at index `graph_slot`. PlayRuntime deserializes + compiles + binds it
// when a Play session begins.
PSYNDER_COMPONENT(ScriptGraphComponent) {
    u32 graph_slot = kInvalidScriptGraphSlot;
};

}  // namespace psynder::scene
