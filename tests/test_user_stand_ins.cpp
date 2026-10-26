#include <catch2/catch_test_macros.hpp>

#include "app/app_internal.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/lua_state.hpp"
#include "lua/match_records.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/special_files.hpp"
#include "lua/url_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "lua/user_stand_ins.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

#include <cstddef>
#include <set>
#include <string>
#include <type_traits>

using osc::lua::StandInAnswer;
using osc::lua::StandInText;
using osc::lua::UserStandIn;
namespace stand_in = osc::lua::stand_in;

// No entry can answer nil: there is no nil kind, a string is a literal, and
// an entry has an answer. (Each line would compile if one could.)
static_assert(!std::is_constructible_v<StandInAnswer, std::nullptr_t>);
static_assert(!std::is_constructible_v<stand_in::ReturnsBool, std::nullptr_t>);
static_assert(!std::is_constructible_v<stand_in::ReturnsBool, int>);
static_assert(!std::is_constructible_v<stand_in::ReturnsBool, const char*>);
static_assert(!std::is_constructible_v<StandInText, std::nullptr_t>);
static_assert(!std::is_constructible_v<StandInText, const char*>);
static_assert(!std::is_constructible_v<UserStandIn, const char (&)[5]>);
static_assert(std::is_constructible_v<stand_in::ReturnsBool, bool>);

namespace {

/// The UI state's real bindings, as App::init_ui_state registers them
/// before the stand-ins (its Lua init scripts aside).
void register_boot_bindings(osc::lua::LuaState& ui, osc::sim::SimState& sim,
                            osc::ui::UIControlRegistry& registry) {
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_special_file_bindings(ui, nullptr);
    osc::lua::register_match_record_bindings(ui);
    osc::lua::register_url_bindings(ui, nullptr);
    osc::lua::register_blueprint_bindings(ui);
    osc::lua::register_ui_bindings(ui, registry);
    osc::lua::register_user_bindings(ui);
    osc::lua::register_core_bindings(ui);
    osc::app::register_session_ui_globals(ui);
    osc::lua::register_prefetch_bindings(ui);
    osc::lua::register_category_bindings(ui);
}

bool global_is_nil(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool nil = lua_isnil(L, -1);
    lua_pop(L, 1);
    return nil;
}

} // namespace

TEST_CASE("Each user-state stand-in is registered once", "[frontend][lua]") {
    // Once in the table, and by no real binding the boot registers first:
    // the table is registered unconditionally, so a name a binding has too
    // would replace the real function.
    std::set<std::string> names;
    for (const UserStandIn& s : osc::lua::user_stand_ins()) {
        INFO(s.name.c_str());
        CHECK(names.insert(s.name.c_str()).second);
    }

    osc::lua::LuaState ui;
    osc::sim::SimState sim(ui.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    register_boot_bindings(ui, sim, registry);
    lua_State* L = ui.raw();
    for (const UserStandIn& s : osc::lua::user_stand_ins()) {
        INFO(s.name.c_str());
        CHECK(global_is_nil(L, s.name.c_str()));
    }

    osc::lua::register_user_stand_ins(ui);
    for (const UserStandIn& s : osc::lua::user_stand_ins()) {
        INFO(s.name.c_str());
        CHECK_FALSE(global_is_nil(L, s.name.c_str()));
    }
}

TEST_CASE("User-state stand-ins answer their stated kinds", "[frontend][lua]") {
    osc::lua::LuaState ui;
    osc::lua::register_user_stand_ins(ui);
    auto result = ui.do_string(R"(
        local function count(...) return arg.n end
        answers = count(PrefetchSession('map', {}, true)) == 0 and count(ClearFrame()) == 0 and
                  __language == 'us' and __installedlanguages[1] == 'us'
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    lua_State* L = ui.raw();
    lua_getglobal(L, "answers");
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    // Every function stand-in is the one C function, so the coverage
    // report can tell it from a binding
    for (const char* name : {"PrefetchSession", "ClearFrame"}) {
        INFO(name);
        lua_getglobal(L, name);
        CHECK(osc::lua::is_user_stand_in(lua_tocfunction(L, -1)));
        lua_pop(L, 1);
    }
    lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
    CHECK_FALSE(osc::lua::is_user_stand_in(lua_tocfunction(L, -1)));
    lua_pop(L, 1);
}

TEST_CASE("The user-state stand-ins define no global Moho lacks", "[frontend][lua]") {
    // Moho's user state has no GetResolution, SetResolution or
    // HasCommandLineArg2 (neither its executable nor faf-re names them; it
    // has HasCommandLineArg), so a script's `if GetResolution then` must
    // find nil, as under Moho.
    osc::lua::LuaState ui;
    osc::sim::SimState sim(ui.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    register_boot_bindings(ui, sim, registry);
    osc::lua::register_user_stand_ins(ui);

    lua_State* L = ui.raw();
    for (const char* name : {"GetResolution", "SetResolution", "HasCommandLineArg2"}) {
        INFO(name);
        CHECK(global_is_nil(L, name));
    }
    CHECK_FALSE(global_is_nil(L, "HasCommandLineArg"));
}
