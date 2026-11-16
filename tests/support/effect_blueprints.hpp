#pragma once

// Effect blueprints registered as retail's /lua/system/Blueprints.lua
// registers them, for tests without game data. A .bp file's
// `EmitterBlueprint { ... }` (`BeamBlueprint`, `TrailEmitterBlueprint`)
// statement hands its table to RegisterEmitterBlueprint
// (RegisterBeamBlueprint, RegisterTrailEmitterBlueprint) with its Source the
// file it came from, unless it names one, and its BlueprintId that path in
// lowercase (SetBackwardsCompatId). The effect caches read the store's
// entries, as Moho's effects read the rules' registered blueprints.

#include "blueprints/blueprint_store.hpp"
#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include <lua.h>
}

#include <string>

namespace osc::test {

class EffectBlueprints {
public:
    explicit EffectBlueprints(lua::LuaState& lua) : lua_(lua), store_(lua.raw()) {
        lua_.set_blueprint_store(&store_);
        lua::register_blueprint_store_bindings(lua_);
        const auto r = lua_.do_string(R"(
            local function kind(register)
                return function(bp)
                    bp.Source = bp.Source or __osc_test_bp_source
                    bp.BlueprintId = string.lower(bp.Source)
                    register(bp)
                end
            end
            EmitterBlueprint = kind(RegisterEmitterBlueprint)
            BeamBlueprint = kind(RegisterBeamBlueprint)
            TrailEmitterBlueprint = kind(RegisterTrailEmitterBlueprint)
        )");
        REQUIRE(r.ok());
    }
    EffectBlueprints(const EffectBlueprints&) = delete;
    EffectBlueprints& operator=(const EffectBlueprints&) = delete;

    /// Run `text`, a .bp file's statements, as the file at `path` runs.
    void add(const std::string& path, const std::string& text) {
        lua_State* L = lua_.raw();
        lua_pushstring(L, "__osc_test_bp_source");
        lua_pushstring(L, path.c_str());
        lua_rawset(L, LUA_GLOBALSINDEX);
        const auto r = lua_.do_string(text);
        UNSCOPED_INFO((r.ok() ? std::string() : r.error().message));
        REQUIRE(r.ok());
    }

    const blueprints::BlueprintStore* store() const { return &store_; }

private:
    lua::LuaState& lua_;
    blueprints::BlueprintStore store_;
};

} // namespace osc::test
