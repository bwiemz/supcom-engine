#pragma once

// The map's border (Moho's MapImager; faf-re render/MapImager.cpp): the
// skin's frame round the map, which retail's UpdateWorldBorderState asks for
// with MapBorderClear and MapBorderAdd while the world_border option is on
// (its default). Each mesh blueprint added is one mesh instance, unrotated:
// - at the heightfield's centre, its height midway between the field's
//   lowest and highest plus the map's imager elevation offset (.scmap 57 on),
//   as UpdateMeshStances sets it each frame;
// - scaled by the field's width times the mesh blueprint's UniformScale
//   (AddBorder).
// Its technique, MapImager, draws before the water and casts no shadow; it
// has no reflection stage and no cartographic technique.

#include "core/types.hpp"
#include "renderer/shadow_camera.hpp" // Box

#include <array>
#include <string>
#include <vector>

struct lua_State;

namespace osc::renderer {

/// The border's mesh blueprints, in the order MapBorderAdd added them since
/// the last MapBorderClear (the UI state's list; moho_bindings.cpp).
std::vector<std::string> map_border_meshes(lua_State* L);

/// A border mesh's model matrix, column-major, over the heightfield's box.
std::array<f32, 16> map_border_model(const Box& field, f32 elevation_offset, f32 uniform_scale);

} // namespace osc::renderer
