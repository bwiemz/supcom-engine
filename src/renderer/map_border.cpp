#include "renderer/map_border.hpp"

#include "renderer/command_feedback.hpp" // feedback_model

#include <lua.h>

namespace osc::renderer {

namespace {
/// Where MapBorderAdd keeps them (l_MapBorderAdd, moho_bindings.cpp).
constexpr const char* kMapBorderKey = "__osc_map_border_meshes";
} // namespace

std::vector<std::string> map_border_meshes(lua_State* L) {
    std::vector<std::string> meshes;
    if (!L) {
        return meshes;
    }
    lua_pushstring(L, kMapBorderKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) {
        for (int i = 1;; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) != LUA_TSTRING) {
                lua_pop(L, 1);
                break;
            }
            meshes.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return meshes;
}

std::array<f32, 16> map_border_model(const Box& field, f32 elevation_offset, f32 uniform_scale) {
    const sim::Vector3 centre{(field.min[0] + field.max[0]) * 0.5f,
                              (field.min[1] + field.max[1]) * 0.5f + elevation_offset,
                              (field.min[2] + field.max[2]) * 0.5f};
    return feedback_model(centre, (field.max[0] - field.min[0]) * uniform_scale);
}

} // namespace osc::renderer
