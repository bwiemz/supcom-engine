// ParseEntityCategory as Moho parses (faf-re EntityCategoryLookupResolver.cpp):
// commas separate clauses (a union), spaces join a clause's words (an
// intersection), and a word no blueprint has is left out.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <string>

namespace {

struct CategoryWorld {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    CategoryWorld() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        state.set_blueprint_store(&store);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'mex1', Categories = {'MASSEXTRACTION', 'TECH1', 'STRUCTURE'}}",
                 "{BlueprintId = 'mex2', Categories = {'MASSEXTRACTION', 'TECH2', 'STRUCTURE'}}",
                 "{BlueprintId = 'mex3', Categories = {'MASSEXTRACTION', 'TECH3', 'STRUCTURE'}}",
                 "{BlueprintId = 'tank', Categories = {'LAND', 'TECH1', 'MOBILE'}}",
                 "{BlueprintId = 'acu', Categories = {'COMMAND', 'LAND'}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
    }

    /// Which of the blueprints `expr` takes, as "mex1 tank ..."
    std::string takes(const char* expr) {
        const std::string code = std::string(R"(
            local cat = ParseEntityCategory(')") +
                                 expr + R"(')
            local out = ''
            for _, id in {'mex1', 'mex2', 'mex3', 'tank', 'acu'} do
                if EntityCategoryContains(cat, id) then out = out .. id .. ' ' end
            end
            __osc_takes = out
        )";
        REQUIRE(state.do_string(code).ok());
        lua_State* L = state.raw();
        lua_getglobal(L, "__osc_takes");
        std::string out = lua_tostring(L, -1);
        lua_pop(L, 1);
        return out;
    }
};

} // namespace

TEST_CASE("ParseEntityCategory: spaces intersect, commas unite", "[category]") {
    CategoryWorld w;
    CHECK(w.takes("TECH1") == "mex1 tank ");
    CHECK(w.takes("MASSEXTRACTION TECH1") == "mex1 ");
    // Retail's AI adjacency category, and a buff's targets
    CHECK(w.takes("MASSEXTRACTION TECH3, MASSEXTRACTION TECH2") == "mex2 mex3 ");
    CHECK(w.takes("TECH3,COMMAND") == "mex3 acu ");
    // A blueprint's id is a word too
    CHECK(w.takes("tank,acu") == "tank acu ");
    CHECK(w.takes("ALLUNITS") == "mex1 mex2 mex3 tank acu ");
}

TEST_CASE("ParseEntityCategory leaves out words no blueprint has", "[category]") {
    CategoryWorld w;
    // An unknown word drops out of its clause, not the clause out of the union
    CHECK(w.takes("TECH1 FOOBAR") == "mex1 tank ");
    CHECK(w.takes("FOOBAR, COMMAND") == "acu ");
    // Nothing known, nothing taken; nor from an empty string or stray commas
    CHECK(w.takes("FOOBAR").empty());
    CHECK(w.takes("").empty());
    CHECK(w.takes(" , ,TECH2 ") == "mex2 ");
    // Words are as written: case counts
    CHECK(w.takes("tech1").empty());
}

TEST_CASE("ParseEntityCategory gives one category per string while it's held", "[category]") {
    CategoryWorld w;
    REQUIRE(w.state
                .do_string(R"(
        local a = ParseEntityCategory('MASSEXTRACTION TECH3, MASSEXTRACTION TECH2')
        local b = ParseEntityCategory('MASSEXTRACTION TECH3, MASSEXTRACTION TECH2')
        if a ~= b then error('parsed twice') end
        -- it combines as any category does
        local c = a + categories.COMMAND
        if not EntityCategoryContains(c, 'acu') or not EntityCategoryContains(c, 'mex2') then
            error('combined')
        end
    )")
                .ok());
}
