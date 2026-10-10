#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sim/manipulator.hpp"
#include "sim/prop.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <memory>
#include <string>

using osc::sim::CommandType;
using osc::sim::Prop;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct WreckSite {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};
    Unit* engineer = nullptr;
    osc::u32 built_id = 0;

    explicit WreckSite(const char* extra_blueprints = "") {
        luaopen_base(L);
        luaopen_table(L);
        lua_settop(L, 0);
        const std::string code = std::string(R"(
            local function extractor(id)
                return {
                    BlueprintId = id,
                    Footprint = { SizeX = 2, SizeZ = 2 },
                    Economy = { BuildTime = 100, BuildCostMass = 36, BuildCostEnergy = 360,
                                RebuildBonusIds = { id } },
                }
            end
            __blueprints = { ueb1103 = extractor('ueb1103'), uab1103 = extractor('uab1103') }
            Engineer = {
                GetRebuildBonus = function(self, bp) return 0.5 end,
                GetReclaimCosts = function(self, target) return 1, 0, 30 end,
            }
            Engineer.__index = Engineer
        )") + extra_blueprints;
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);

        lua_pushstring(L, "__osc_create_building_unit");
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, &WreckSite::create_building_unit, 1);
        lua_rawset(L, LUA_REGISTRYINDEX);

        auto u = std::make_unique<Unit>();
        u->set_position({11.0f, 0.0f, 5.0f});
        u->set_build_rate(10.0f);
        u->set_max_build_distance(10.0f);
        engineer = u.get();
        sim.entity_registry().register_entity(std::move(u));
        bind(*engineer, "Engineer");
    }
    ~WreckSite() { lua_close(L); }
    WreckSite(const WreckSite&) = delete;
    WreckSite& operator=(const WreckSite&) = delete;

    static int create_building_unit(lua_State* L) {
        auto* self = static_cast<WreckSite*>(lua_touserdata(L, lua_upvalueindex(1)));
        auto u = std::make_unique<Unit>();
        u->set_blueprint_id(lua_tostring(L, 1));
        u->set_unit_id(lua_tostring(L, 1));
        u->set_position({static_cast<osc::f32>(lua_tonumber(L, 3)), 0.0f,
                         static_cast<osc::f32>(lua_tonumber(L, 5))});
        u->set_max_health(100.0f);
        u->set_health(0.0f);
        u->set_fraction_complete(0.0f);
        u->set_is_being_built(true);
        Unit* raw = u.get();
        self->sim.entity_registry().register_entity(std::move(u));
        self->bind(*raw, nullptr);
        self->built_id = raw->entity_id();
        lua_pushnumber(L, raw->entity_id());
        lua_rawgeti(L, LUA_REGISTRYINDEX, raw->lua_table_ref());
        return 2;
    }

    void bind(osc::sim::Entity& e, const char* cls) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, &e);
        lua_rawset(L, -3);
        if (cls) {
            lua_getglobal(L, cls);
            lua_setmetatable(L, -2);
        }
        e.set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    }

    osc::u32 prop(osc::f32 x, osc::f32 z, const char* associated, bool reclaimable,
                  bool obstructs = false) {
        auto p = std::make_unique<Prop>();
        p->set_position({x, 0.0f, z});
        p->reclaimable_category = reclaimable;
        p->obstructs_building = obstructs;
        Prop* raw = p.get();
        sim.entity_registry().register_entity(std::move(p));
        bind(*raw, nullptr);
        if (associated) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, raw->lua_table_ref());
            lua_pushstring(L, "AssociatedBP");
            lua_pushstring(L, associated);
            lua_rawset(L, -3);
            lua_pop(L, 1);
        }
        return raw->entity_id();
    }

    void build(const char* bp_id) {
        UnitCommand c;
        c.type = CommandType::BuildMobile;
        c.blueprint_id = bp_id;
        c.target_pos = {11.0f, 0.0f, 11.0f};
        engineer->push_command(c, true);
    }

    Unit* built() {
        auto* e = built_id != 0 ? sim.entity_registry().find(built_id) : nullptr;
        return e ? static_cast<Unit*>(e) : nullptr;
    }
};

} // namespace

TEST_CASE("A structure built on its own wreck starts half built, and the wreck goes",
          "[build][wreck]") {
    WreckSite w;
    const osc::u32 wreck = w.prop(11.0f, 11.0f, "ueb1103", true);
    w.build("ueb1103");
    w.sim.tick();
    REQUIRE(w.built());
    CHECK(w.built()->fraction_complete() == Catch::Approx(0.51f));
    CHECK(w.sim.entity_registry().find(wreck) == nullptr);
}

TEST_CASE("A wreck of another structure is reclaimed before the build starts", "[build][wreck]") {
    WreckSite w;
    const osc::u32 wreck = w.prop(11.0f, 11.0f, "ueb1103", true);
    w.build("uab1103");
    w.sim.tick();
    CHECK_FALSE(w.built());
    REQUIRE(w.engineer->command_queue().size() == 1);
    CHECK(w.engineer->command_queue().front().type == CommandType::BuildMobile);
    CHECK(w.engineer->reclaim_target_id() == wreck);
    for (int i = 0; i < 20 && !w.built(); ++i) {
        w.sim.tick();
    }
    REQUIRE(w.built());
    CHECK(w.sim.entity_registry().find(wreck) == nullptr);
    CHECK(w.built()->fraction_complete() < 0.05f);
}

TEST_CASE("A build stopped while it clears its site stops reclaiming", "[build][wreck]") {
    WreckSite w;
    w.prop(11.0f, 11.0f, "ueb1103", true);
    w.build("uab1103");
    w.sim.tick();
    REQUIRE(w.engineer->reclaim_target_id() != 0);
    w.engineer->clear_commands();
    w.sim.tick();
    CHECK(w.engineer->reclaim_target_id() == 0);
}

TEST_CASE("With OBSTRUCTSBUILDING in the data, only such props are cleared first",
          "[build][wreck]") {
    WreckSite w(
        "__blueprints.wreck = { BlueprintId = 'wreck', Categories = { 'OBSTRUCTSBUILDING' } }");
    const osc::u32 tree = w.prop(10.5f, 10.5f, nullptr, true);
    w.build("uab1103");
    w.sim.tick();
    REQUIRE(w.built());
    CHECK(w.sim.entity_registry().find(tree) != nullptr);
}

TEST_CASE("A wreck worth nothing is reclaimed off the site all the same", "[build][wreck]") {
    WreckSite w("Engineer.GetReclaimCosts = function(self, target) return 1, 0, 0 end");
    const osc::u32 wreck = w.prop(11.0f, 11.0f, "ueb1103", true);
    w.build("uab1103");
    for (int i = 0; i < 20 && !w.built(); ++i) {
        w.sim.tick();
    }
    REQUIRE(w.built());
    CHECK(w.sim.entity_registry().find(wreck) == nullptr);
}

TEST_CASE("A paused engineer clears its site, then makes its frame only on a retry after unpause",
          "[build][wreck][pause]") {
    WreckSite w;
    const osc::u32 wreck = w.prop(11.0f, 11.0f, "ueb1103", true);
    w.engineer->set_paused(true);
    w.build("uab1103");
    for (int i = 0; i < 40 && w.sim.entity_registry().find(wreck); ++i) {
        w.sim.tick();
    }
    CHECK(w.sim.entity_registry().find(wreck) == nullptr);
    for (int i = 0; i < 15; ++i) {
        w.sim.tick();
    }
    CHECK_FALSE(w.built());
    REQUIRE(w.engineer->command_queue().size() == 1);
    w.engineer->set_paused(false);
    int ticks = 0;
    while (!w.built() && ticks < 20) {
        w.sim.tick();
        ++ticks;
    }
    CHECK(ticks <= 10);
}

TEST_CASE("A paused engineer's frame decays while it holds it, and builds on after a retry",
          "[build][pause]") {
    WreckSite w;
    w.build("ueb1103");
    for (int i = 0; i < 3; ++i) {
        w.sim.tick();
    }
    REQUIRE(w.built());
    const osc::f32 frac = w.built()->fraction_complete();
    const osc::f32 work = w.engineer->work_progress();
    w.engineer->set_paused(true);
    for (int i = 0; i < 15; ++i) {
        w.sim.tick();
    }
    CHECK(w.engineer->is_building());
    CHECK(w.engineer->command_queue().size() == 1);
    CHECK(w.engineer->work_progress() == work);
    CHECK(w.built()->fraction_complete() == Catch::Approx(frac - 14 * 0.1f / 360.0f));
    w.engineer->set_paused(false);
    osc::f32 last = w.built()->fraction_complete();
    w.sim.tick();
    CHECK(w.built()->fraction_complete() < last);
    for (int i = 0; i < 10; ++i) {
        w.sim.tick();
    }
    CHECK(w.built()->fraction_complete() > last);
}
