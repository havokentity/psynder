// SPDX-License-Identifier: MIT
// Psynder — in-engine editor / sandbox mode (Garry's Mod-style). Lane 18 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <string_view>

namespace psynder::editor {

enum class Mode : u8 { Play, Edit };

void toggle_mode();
Mode current_mode();

// Brush CSG primitives — lane 18 implements the CSG ops + BSP compile.
enum class BrushShape : u8 { Box, Wedge, Cylinder, Prism };
void brush_create(BrushShape s, math::Vec3 origin, math::Vec3 extents);
void brush_subtract(u32 brush_id, BrushShape s, math::Vec3 origin, math::Vec3 extents);
void brush_commit();

// Heightmap sculpt
void sculpt_raise(math::Vec3 world_point, f32 radius, f32 strength);
void sculpt_lower(math::Vec3 world_point, f32 radius, f32 strength);
void sculpt_smooth(math::Vec3 world_point, f32 radius, f32 strength);
void sculpt_paint(math::Vec3 world_point, f32 radius, u8 material_index, f32 weight);

// Physgun
void physgun_grab(u32 body_id);
void physgun_drop();
void physgun_freeze();

// Save / load
bool save_level(std::string_view virtual_path);
bool load_level(std::string_view virtual_path);
bool save_contraption(std::string_view virtual_path);

}  // namespace psynder::editor
