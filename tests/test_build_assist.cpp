#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sim/lua_sim.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <memory>
#include <string>

using osc::sim::AimManipulator;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::test::tick_until;

namespace {

int set_unit_state(lua_State* L) {
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* unit = static_cast<Unit*>(static_cast<osc::sim::Entity*>(lua_touserdata(L, -1)));
    lua_pop(L, 1);
    unit->set_unit_state(luaL_checkstring(L, 2), lua_toboolean(L, 3) != 0);
    return 0;
}

struct AssistSim {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};
    Unit* site = nullptr;

    AssistSim() {
        luaopen_base(L);
        luaopen_table(L);
        lua_settop(L, 0);
        lua_register(L, "SetUnitState", set_unit_state);
        const char* code = R"(
            log = {}
            __blueprints = {site = {Economy = {BuildTime = 100, BuildCostMass = 10, BuildCostEnergy = 10}}}
            Builder = {
                OnStartBuild = function(self, unit, order)
                    table.insert(log, self.name .. ' start ' .. tostring(order))
                end,
                OnStopBuild = function(self, unit)
                    table.insert(log, self.name .. ' stop')
                end,
                OnPrepareArmToBuild = function(self)
                    table.insert(log, self.name .. ' arm')
                end,
            }
            Builder.__index = Builder
        )";
        REQUIRE(luaL_loadbuffer(L, code, std::string(code).size(), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);

        auto s = std::make_unique<Unit>();
        s->set_unit_id("site");
        s->set_position({20.0f, 0.0f, 20.0f});
        s->set_max_health(100.0f);
        s->set_health(10.0f);
        s->set_fraction_complete(0.1f);
        s->set_is_being_built(true);
        site = s.get();
        sim.entity_registry().register_entity(std::move(s));
        bind(*site, nullptr);
    }
    ~AssistSim() { lua_close(L); }
    AssistSim(const AssistSim&) = delete;
    AssistSim& operator=(const AssistSim&) = delete;

    void bind(osc::sim::Entity& e, const char* name) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, &e);
        lua_rawset(L, -3);
        if (name) {
            lua_pushstring(L, "name");
            lua_pushstring(L, name);
            lua_rawset(L, -3);
            lua_getglobal(L, "Builder");
            lua_setmetatable(L, -2);
        }
        e.set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    }

    Unit* engineer(const char* name, osc::f32 x) {
        auto u = std::make_unique<Unit>();
        u->set_position({x, 0.0f, 20.0f});
        u->set_build_rate(10.0f);
        u->set_max_build_distance(10.0f);
        u->add_category("REPAIR");
        auto arm = std::make_unique<AimManipulator>();
        arm->set_builder_arm(true);
        u->add_manipulator(std::move(arm));
        u->set_builder_on_target(false);
        Unit* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        bind(*raw, name);
        return raw;
    }

    void script_sets_repairing() {
        const std::string code = R"(
            local start = Builder.OnStartBuild
            Builder.OnStartBuild = function(self, unit, order)
                if order == 'Repair' then SetUnitState(self, 'Repairing', true) end
                start(self, unit, order)
            end
        )";
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "faf") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    }

    std::string log() {
        const std::string code = "local s = table.concat(log, ','); log = {}; return s";
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "l") == 0);
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        std::string v = lua_tostring(L, -1);
        lua_pop(L, 1);
        return v;
    }
};

UnitCommand order(CommandType type, const Unit& target, osc::u32 id) {
    UnitCommand c;
    c.type = type;
    c.target_id = target.entity_id();
    c.target_pos = target.position();
    c.command_id = id;
    return c;
}

const AimManipulator& arm_of(const Unit& u) {
    return static_cast<const AimManipulator&>(*u.manipulators().front());
}

} // namespace

TEST_CASE("A repair of a unit under construction aims the arm and builds it as a repair",
          "[build][assist]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    // The order aims the arm; the arm turns before the next tick's order,
    // which starts the repair (Moho's MotionTick runs after its tasks).
    w.sim.tick();
    CHECK_FALSE(eng->is_building());
    CHECK(w.log() == "eng arm");
    w.sim.tick();
    CHECK(eng->is_building());
    CHECK(w.log() == "eng start Repair");
    CHECK(arm_of(*eng).has_target());

    eng->push_command(order(CommandType::Stop, *w.site, 2), true);
    w.sim.tick();
    CHECK(w.log() == "eng stop");
    CHECK_FALSE(arm_of(*eng).has_target());
}

TEST_CASE("A guard builds its unfinished target and keeps guarding when the work ends",
          "[build][assist][guard]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Guard, *w.site, 1), true);
    tick_until([&] { w.sim.tick(); }, "the guard starts building its target", 40,
               [&] { return eng->is_building(); });
    CHECK(eng->build_target_id() == w.site->entity_id());
    CHECK(w.log() == "eng arm,eng start Repair");
    const osc::f32 before = w.site->fraction_complete();
    w.sim.tick();
    CHECK(w.site->fraction_complete() > before);

    SECTION("completion keeps the guard and ends the build effects once") {
        tick_until([&] { w.sim.tick(); }, "the site is built", 300,
                   [&] { return !w.site->is_being_built(); });
        CHECK(w.site->fraction_complete() == 1.0f);
        CHECK(w.log() == "eng stop");
        REQUIRE(eng->command_queue().size() == 1);
        CHECK(eng->command_queue().front().type == CommandType::Guard);
        for (int tick = 0; tick < 10; ++tick) w.sim.tick();
        CHECK(w.log().empty());
        CHECK_FALSE(eng->is_building());
    }
    SECTION("replacing the guard releases the unfinished target") {
        eng->push_command(order(CommandType::Stop, *w.site, 2), true);
        w.sim.tick();
        CHECK(w.log() == "eng stop");
        CHECK_FALSE(eng->is_building());
        CHECK_FALSE(arm_of(*eng).has_target());
        CHECK(w.site->is_being_built());
    }
    SECTION("a destroyed target ends the guard and its build effects") {
        w.site->mark_destroyed();
        w.sim.tick();
        CHECK(w.log() == "eng stop");
        CHECK_FALSE(eng->is_building());
        CHECK(eng->command_queue().empty());
    }
}

TEST_CASE("A repair whose target is gone stops with nil for it, and fails nothing",
          "[build][assist]") {
    AssistSim w;
    const std::string code = R"(
        function Builder:OnStopBuild(unit, order)
            table.insert(log, self.name .. ' stop ' .. tostring(unit) .. ' ' .. tostring(order))
        end
        function Builder:OnFailedToBuild() table.insert(log, self.name .. ' failed') end
    )";
    REQUIRE(luaL_loadbuffer(w.L, code.c_str(), code.size(), "stop") == 0);
    REQUIRE(lua_pcall(w.L, 0, 0, 0) == 0);
    Unit* eng = w.engineer("eng", 16.0f);
    std::string stopped = "eng stop nil Repair";
    SECTION("a repair of a unit under construction") {
        eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    }
    SECTION("a guard's help with it") {
        Unit* helper = w.engineer("helper", 14.0f);
        eng->push_command(order(CommandType::Repair, *w.site, 1), true);
        helper->push_command(order(CommandType::Guard, *eng, 2), true);
        stopped += ",helper stop nil Repair";
    }
    SECTION("a repair of a damaged unit") {
        w.site->set_is_being_built(false);
        w.site->set_fraction_complete(1.0f);
        w.site->set_health(50.0f);
        eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    }
    for (int i = 0; i < 3; ++i) {
        w.sim.tick();
    }
    REQUIRE(w.log().find("start Repair") != std::string::npos);
    const osc::u32 id = w.site->entity_id();
    w.site->mark_destroyed();
    w.sim.entity_registry().unregister_entity(id);
    w.sim.tick();
    CHECK(w.log() == stopped);
}

TEST_CASE("A guard's help ends with OnStopBuild when what it guards is gone", "[build][assist]") {
    // An upgrade's end: the structure upgrading destroys itself once the
    // new one stands, and its helpers' guards go with it. Each helper's
    // script must hear its build stop (retail's OnStopBuild ends the build
    // effects and the commander's build pose); it had gone on building in
    // silence until some later build order.
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* helper = w.engineer("helper", 14.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    helper->push_command(order(CommandType::Guard, *eng, 2), true);
    tick_until([&] { w.sim.tick(); }, "the helper starts building", 10,
               [&] { return helper->is_building(); });
    w.log();
    const osc::u32 id = eng->entity_id();
    eng->mark_destroyed();
    w.sim.entity_registry().unregister_entity(id);
    w.sim.tick();
    CHECK(w.log() == "helper stop");
    CHECK_FALSE(helper->is_building());
    CHECK(helper->command_queue().empty());
}

TEST_CASE("A guard helping a build aims its arm at what it helps build", "[build][assist]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* helper = w.engineer("helper", 14.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    helper->push_command(order(CommandType::Guard, *eng, 2), true);
    // The repair starts on the second tick; the helper aims at it then and
    // starts on the third
    for (int i = 0; i < 3; ++i) {
        w.sim.tick();
    }
    CHECK(helper->is_building());
    CHECK(arm_of(*helper).has_target());
}

TEST_CASE("A repair of a unit under construction starts once the builder's arm is on it",
          "[build][assist][arm]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    auto& arm = *eng->manipulators().front();
    arm.set_enabled(false);
    eng->set_builder_on_target(false);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    w.sim.tick();
    CHECK(w.log() == "eng arm");
    for (int i = 0; i < 3; ++i) {
        w.sim.tick();
        CHECK_FALSE(eng->is_building());
        CHECK(arm_of(*eng).has_target());
    }
    CHECK(w.log().empty());
    CHECK(w.site->fraction_complete() <= 0.1f);

    // Enabled, the arm turns after the next tick's order, and the order
    // after that finds it on target
    arm.set_enabled(true);
    w.sim.tick();
    CHECK_FALSE(eng->is_building());
    w.sim.tick();
    CHECK(eng->is_building());
    CHECK(w.log() == "eng start Repair");
}

TEST_CASE("A paused repairer of a unit under construction holds it without work",
          "[build][assist][pause]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    tick_until([&] { w.sim.tick(); }, "the repair starts", 10, [&] { return eng->is_building(); });
    const osc::f32 frac = w.site->fraction_complete();
    eng->set_paused(true);
    for (int i = 0; i < 20; ++i) {
        w.sim.tick();
    }
    CHECK(eng->is_building());
    CHECK(eng->command_queue().size() == 1);
    CHECK(w.site->fraction_complete() == frac);
    eng->set_paused(false);
    w.sim.tick();
    CHECK(w.site->fraction_complete() > frac);
}

TEST_CASE("A repairer paused before it starts waits at its target until a retry after unpause",
          "[build][assist][pause]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    eng->set_paused(true);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    for (int i = 0; i < 15; ++i) {
        w.sim.tick();
        CHECK_FALSE(eng->is_building());
    }
    CHECK(eng->command_queue().size() == 1);
    CHECK(arm_of(*eng).has_target());
    // On target from the second tick, the order meets the pause there and
    // retries each 10th tick: the 22nd, the 7th after the unpause.
    eng->set_paused(false);
    for (int i = 0; i < 6; ++i) {
        w.sim.tick();
        CHECK_FALSE(eng->is_building());
    }
    w.sim.tick();
    CHECK(eng->is_building());
}

TEST_CASE("A guard paused before it helps a build waits until a retry after unpause",
          "[build][assist][pause]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* helper = w.engineer("helper", 14.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    helper->set_paused(true);
    helper->push_command(order(CommandType::Guard, *eng, 2), true);
    for (int i = 0; i < 15; ++i) {
        w.sim.tick();
        CHECK_FALSE(helper->is_building());
    }
    // The helper's arm is on the build from the third tick, where it meets
    // the pause, then each 10th: the 23rd, the 8th after the unpause.
    helper->set_paused(false);
    for (int i = 0; i < 7; ++i) {
        w.sim.tick();
        CHECK_FALSE(helper->is_building());
    }
    w.sim.tick();
    CHECK(helper->is_building());
}

TEST_CASE("A guard paused before building its ward consumes its retry timer once per tick",
          "[build][assist][guard][pause]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    eng->set_paused(true);
    eng->push_command(order(CommandType::Guard, *w.site, 1), true);
    for (int tick = 0; tick < 15; ++tick) {
        w.sim.tick();
        CHECK_FALSE(eng->is_building());
    }
    // Like a direct repair, the arm arrives on tick 2, then retries on
    // ticks 12 and 22. Unpausing on tick 15 leaves seven ticks to wait.
    eng->set_paused(false);
    for (int tick = 0; tick < 6; ++tick) {
        w.sim.tick();
        CHECK_FALSE(eng->is_building());
    }
    w.sim.tick();
    CHECK(eng->is_building());
}

TEST_CASE("A paused guard helping a build holds it and adds nothing", "[build][assist][pause]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* helper = w.engineer("helper", 14.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    helper->push_command(order(CommandType::Guard, *eng, 2), true);
    tick_until([&] { w.sim.tick(); }, "the helper starts building", 10,
               [&] { return helper->is_building(); });
    helper->set_paused(true);
    const osc::f32 before = w.site->fraction_complete();
    w.sim.tick();
    const osc::f32 step = w.site->fraction_complete() - before;
    w.sim.tick();
    CHECK(helper->is_building());
    CHECK(w.site->fraction_complete() - before == Catch::Approx(2 * step));
    CHECK(step == Catch::Approx(0.01f));
}

TEST_CASE("A paused repairer of a damaged unit holds it without work", "[build][assist][pause]") {
    AssistSim w;
    w.site->set_unit_id("site");
    w.site->set_is_being_built(false);
    w.site->set_fraction_complete(1.0f);
    w.site->set_health(50.0f);
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    tick_until([&] { w.sim.tick(); }, "the repair starts", 10, [&] { return eng->is_repairing(); });
    const osc::f32 hp = w.site->health();
    REQUIRE(hp > 50.0f);
    eng->set_paused(true);
    for (int i = 0; i < 5; ++i) {
        w.sim.tick();
    }
    CHECK(eng->is_repairing());
    CHECK(w.site->health() == hp);
    eng->set_paused(false);
    w.sim.tick();
    CHECK(w.site->health() > hp);
}

TEST_CASE("A builder that must face its target turns to it before it repairs",
          "[build][assist][face]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    Unit::Drive drive;
    drive.turn_rate = 1.5f;
    eng->set_drive(drive);
    eng->set_need_to_face_target_to_build(true);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    w.sim.tick();
    CHECK(w.log() == "eng arm");
    CHECK_FALSE(eng->is_building());
    const int ticks = 1 + tick_until([&] { w.sim.tick(); }, "the builder turns to it and starts",
                                     60, [&] { return eng->is_building(); });
    CHECK(ticks > 5);
    CHECK(w.log() == "eng start Repair");
    CHECK(osc::sim::quat_yaw(eng->orientation()) > 1.2f);
    CHECK(eng->attack_facing().x == 0.0f);
    CHECK(eng->attack_facing().z == 0.0f);
}

TEST_CASE("A guard that must face what it helps build turns to it first", "[build][assist][face]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* helper = w.engineer("helper", 14.0f);
    Unit::Drive drive;
    drive.turn_rate = 1.5f;
    helper->set_drive(drive);
    helper->set_need_to_face_target_to_build(true);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    helper->push_command(order(CommandType::Guard, *eng, 2), true);
    w.sim.tick();
    w.sim.tick();
    CHECK_FALSE(helper->is_building());
    tick_until([&] { w.sim.tick(); }, "the helper turns to the build and starts", 60,
               [&] { return helper->is_building(); });
    CHECK(osc::sim::quat_yaw(helper->orientation()) > 1.2f);
}

TEST_CASE("Repairing a script set goes when a repair of a damaged unit ends, done or stopped",
          "[build][assist][repair]") {
    AssistSim w;
    w.script_sets_repairing();
    w.site->set_is_being_built(false);
    w.site->set_fraction_complete(1.0f);
    w.site->set_health(50.0f);
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    tick_until([&] { w.sim.tick(); }, "the repair starts", 10, [&] { return eng->is_repairing(); });
    REQUIRE(eng->has_unit_state("Repairing"));
    tick_until([&] { w.sim.tick(); }, "the repair ends", 200, [&] { return !eng->is_repairing(); });
    REQUIRE(w.site->health() == w.site->max_health());
    CHECK_FALSE(eng->has_unit_state("Repairing"));

    w.site->set_health(50.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 2), true);
    tick_until([&] { w.sim.tick(); }, "the second repair starts", 10,
               [&] { return eng->is_repairing(); });
    REQUIRE(eng->has_unit_state("Repairing"));
    eng->push_command(order(CommandType::Stop, *w.site, 3), true);
    w.sim.tick();
    REQUIRE_FALSE(eng->is_repairing());
    CHECK_FALSE(eng->has_unit_state("Repairing"));
}

TEST_CASE("Repairing a script set goes when a repair of a unit under construction ends, done or "
          "stopped",
          "[build][assist][repair]") {
    AssistSim w;
    w.script_sets_repairing();
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* helper = w.engineer("helper", 14.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    helper->push_command(order(CommandType::Guard, *eng, 2), true);
    tick_until([&] { w.sim.tick(); }, "the repair and its help start", 10,
               [&] { return eng->is_building() && helper->is_building(); });
    REQUIRE(eng->has_unit_state("Repairing"));
    REQUIRE(helper->has_unit_state("Repairing"));
    eng->push_command(order(CommandType::Stop, *w.site, 3), true);
    tick_until([&] { w.sim.tick(); }, "the repair and its help stop", 10,
               [&] { return !eng->is_building() && !helper->is_building(); });
    CHECK_FALSE(eng->has_unit_state("Repairing"));
    CHECK_FALSE(helper->has_unit_state("Repairing"));

    eng->push_command(order(CommandType::Repair, *w.site, 4), true);
    tick_until([&] { w.sim.tick(); }, "the second repair starts", 10,
               [&] { return eng->is_building(); });
    REQUIRE(eng->has_unit_state("Repairing"));
    tick_until([&] { w.sim.tick(); }, "the second repair ends", 200,
               [&] { return !eng->is_building(); });
    REQUIRE(w.site->fraction_complete() == 1.0f);
    CHECK_FALSE(eng->has_unit_state("Repairing"));
}

TEST_CASE("A repairer's focus is the unit it repairs, and nothing once it is done or stopped",
          "[build][assist][repair]") {
    AssistSim w;
    w.site->set_is_being_built(false);
    w.site->set_fraction_complete(1.0f);
    w.site->set_health(50.0f);
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    tick_until([&] { w.sim.tick(); }, "the repair starts", 10, [&] { return eng->is_repairing(); });
    CHECK(eng->focus_id() == w.site->entity_id());
    tick_until([&] { w.sim.tick(); }, "the repair ends", 200, [&] { return !eng->is_repairing(); });
    REQUIRE(w.site->health() == w.site->max_health());
    CHECK(eng->focus_id() == 0);

    w.site->set_health(50.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 2), true);
    tick_until([&] { w.sim.tick(); }, "the second repair starts", 10,
               [&] { return eng->is_repairing(); });
    REQUIRE(eng->focus_id() == w.site->entity_id());
    eng->push_command(order(CommandType::Stop, *w.site, 3), true);
    w.sim.tick();
    CHECK(eng->focus_id() == 0);
}

TEST_CASE("A helper with no build rate takes up the build it guards and adds no work",
          "[build][assist][guard]") {
    // Moho's build bots (ura0001, BuildRate 0): the build is their parent's,
    // their own start and stop show it (probed 2026-10-23, `buildbots`).
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* bot = w.engineer("bot", 14.0f);
    bot->set_build_rate(0.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    // The original's guard of the frame ends with it, one of its builder goes
    // on; the engine keeps both (as the test of a guard's help above pins).
    bool guards_builder = false;
    SECTION("guarding the unit under construction") {
        bot->push_command(order(CommandType::Guard, *w.site, 2), true);
    }
    SECTION("guarding its builder") {
        bot->push_command(order(CommandType::Guard, *eng, 2), true);
        guards_builder = true;
    }
    tick_until([&] { w.sim.tick(); }, "the bot takes up the build", 40,
               [&] { return bot->is_building(); });
    CHECK(bot->build_target_id() == w.site->entity_id());
    CHECK(w.log().find("bot start Repair") != std::string::npos);
    CHECK(bot->economy().consumption_mass == 0);
    CHECK(bot->economy().consumption_energy == 0);

    // The engineer's rate alone: 10 of a BuildTime of 100 a second
    const osc::f32 before = w.site->fraction_complete();
    w.sim.tick();
    CHECK(w.site->fraction_complete() - before == Catch::Approx(0.01f).margin(1e-5f));
    CHECK(bot->work_progress() == Catch::Approx(w.site->fraction_complete()).margin(0.011f));

    tick_until(
        [&] {
            w.sim.tick();
            if (w.site->is_being_built()) {
                CHECK(bot->is_building());
            }
        },
        "the site is built", 300, [&] { return !w.site->is_being_built(); });
    w.sim.tick();
    CHECK(w.log().find("bot stop") != std::string::npos);
    CHECK_FALSE(bot->is_building());
    if (guards_builder) {
        REQUIRE(bot->command_queue().size() == 1);
        CHECK(bot->command_queue().front().type == CommandType::Guard);
    }
}

TEST_CASE("A helper with no build rate leaves the build's completion to a builder that works on it",
          "[build][assist][guard]") {
    AssistSim w;
    const std::string code = R"(
        function hear_built(site)
            site.OnStopBeingBuilt = function(self, builder)
                table.insert(log, 'built by ' .. tostring(builder and builder.name))
            end
        end
    )";
    REQUIRE(luaL_loadbuffer(w.L, code.c_str(), code.size(), "built") == 0);
    REQUIRE(lua_pcall(w.L, 0, 0, 0) == 0);
    lua_getglobal(w.L, "hear_built");
    lua_rawgeti(w.L, LUA_REGISTRYINDEX, w.site->lua_table_ref());
    REQUIRE(lua_pcall(w.L, 1, 0, 0) == 0);
    // The bot ticks first; the helper, last, brings the build to 1 after the
    // engineer's turn, so the next tick the bot is first to see it done.
    Unit* bot = w.engineer("bot", 14.0f);
    bot->set_build_rate(0.0f);
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* helper = w.engineer("helper", 18.0f);
    bot->push_command(order(CommandType::Guard, *w.site, 1), true);
    eng->push_command(order(CommandType::Repair, *w.site, 2), true);
    helper->push_command(order(CommandType::Guard, *eng, 3), true);
    tick_until([&] { w.sim.tick(); }, "the bot and the helper take up the build", 40,
               [&] { return bot->is_building() && helper->is_building(); });
    w.log();
    w.site->set_fraction_complete(0.985f);
    w.sim.tick();
    REQUIRE(w.site->fraction_complete() >= 1.0f);
    REQUIRE(w.site->is_being_built());
    w.sim.tick();
    CHECK_FALSE(w.site->is_being_built());
    CHECK(w.log().find("built by eng") != std::string::npos);
}

TEST_CASE("A repairer with no build rate takes up a damaged unit's repair and heals nothing",
          "[build][assist][repair]") {
    AssistSim w;
    w.site->set_is_being_built(false);
    w.site->set_fraction_complete(1.0f);
    w.site->set_health(50.0f);
    Unit* bot = w.engineer("bot", 16.0f);
    bot->set_build_rate(0.0f);
    bot->push_command(order(CommandType::Repair, *w.site, 1), true);
    tick_until([&] { w.sim.tick(); }, "the bot takes up the repair", 10,
               [&] { return bot->is_repairing(); });
    CHECK(w.log() == "bot arm,bot start Repair");
    for (int tick = 0; tick < 30; ++tick) {
        w.sim.tick();
        CHECK(bot->is_repairing());
    }
    CHECK(w.site->health() == 50.0f);
    CHECK(bot->focus_id() == w.site->entity_id());
    CHECK(bot->command_queue().size() == 1);
    bot->set_build_rate(10.0f);
    w.sim.tick();
    CHECK(w.site->health() > 50.0f);
}

TEST_CASE("Each build step brings a unit under construction level with its health first, so a "
          "fresh frame's first hit point counts",
          "[build][assist]") {
    AssistSim w;
    w.site->set_health(1.0f);
    w.site->set_fraction_complete(0.0f);
    Unit* eng = w.engineer("eng", 16.0f);
    const auto first_steps = [&](osc::f32 rate) {
        eng->set_build_rate(rate);
        eng->push_command(order(CommandType::Repair, *w.site, 1), true);
        tick_until([&] { w.sim.tick(); }, "the first build step", 10,
                   [&] { return w.site->fraction_complete() != 0.0f; });
    };
    SECTION("a step smaller than that hit point's share of 100 lifts it to the share") {
        first_steps(0.5f); // 0.0005 a step against 0.01
        CHECK(w.site->fraction_complete() == Catch::Approx(0.01f));
        CHECK(w.site->health() == Catch::Approx(1.05f));
        w.sim.tick();
        CHECK(w.site->fraction_complete() == Catch::Approx(0.0105f));
        CHECK(w.site->health() == Catch::Approx(1.1f));
    }
    SECTION("a larger step adds to the fraction, the health a hit point ahead") {
        first_steps(20.0f); // 0.02 a step
        CHECK(w.site->fraction_complete() == Catch::Approx(0.02f));
        CHECK(w.site->health() == Catch::Approx(3.0f));
        w.sim.tick();
        CHECK(w.site->fraction_complete() == Catch::Approx(0.04f));
        CHECK(w.site->health() == Catch::Approx(5.0f));
    }
    CHECK(eng->work_progress() == w.site->fraction_complete());
}

TEST_CASE("A slow helper's step brings the unit it helps build level with its health",
          "[build][assist][guard]") {
    AssistSim w;
    w.site->set_health(1.0f);
    w.site->set_fraction_complete(0.0f);
    Unit* eng = w.engineer("eng", 16.0f);
    eng->set_build_rate(20.0f);
    Unit* helper = w.engineer("helper", 14.0f);
    helper->set_build_rate(0.5f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    helper->push_command(order(CommandType::Guard, *eng, 2), true);
    tick_until([&] { w.sim.tick(); }, "the helper starts building", 20,
               [&] { return helper->is_building(); });
    w.sim.tick();
    w.sim.tick();
    // The helper's step of 0.0005 is the last one in front of its health
    CHECK(w.site->health() ==
          Catch::Approx(w.site->max_health() * w.site->fraction_complete() + 0.05f));
    CHECK(helper->work_progress() == w.site->fraction_complete());
}

TEST_CASE("A unit damaged while it is being built is finished with that damage",
          "[build][assist]") {
    AssistSim w;
    w.site->set_fraction_complete(0.995f);
    w.site->set_health(50.0f);
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    tick_until([&] { w.sim.tick(); }, "the site is built", 10,
               [&] { return !w.site->is_being_built(); });
    CHECK(w.site->health() == Catch::Approx(51.0f));
}

namespace {

/// OnBuildProgress and OnBeingBuiltProgress, heard in `progress` as "<who> <band>": the first of
/// 0.25, 0.5 and 0.75 the call's old and new fractions straddle, " from 0" after it for an old of
/// 0.
void listen_to_progress(AssistSim& w) {
    const std::string code = R"(
        progress = {}
        local function heard(who, old, new)
            local band = 'none'
            for _, b in {0.25, 0.5, 0.75} do
                if band == 'none' and old < b and new >= b then band = tostring(b) end
            end
            table.insert(progress, who .. ' ' .. band .. (old == 0 and ' from 0' or ''))
        end
        function Builder:OnBuildProgress(unit, old, new) heard(self.name, old, new) end
        Site = {OnBeingBuiltProgress = function(self, builder, old, new)
            heard('site:' .. builder.name, old, new)
        end}
        Site.__index = Site
    )";
    REQUIRE(luaL_loadbuffer(w.L, code.c_str(), code.size(), "progress") == 0);
    REQUIRE(lua_pcall(w.L, 0, 0, 0) == 0);
    lua_rawgeti(w.L, LUA_REGISTRYINDEX, w.site->lua_table_ref());
    lua_getglobal(w.L, "Site");
    lua_setmetatable(w.L, -2);
    lua_pop(w.L, 1);
}

std::string progress(AssistSim& w) {
    const std::string code = "local s = table.concat(progress, ','); progress = {}; return s";
    REQUIRE(luaL_loadbuffer(w.L, code.c_str(), code.size(), "p") == 0);
    REQUIRE(lua_pcall(w.L, 0, 1, 0) == 0);
    std::string v = lua_tostring(w.L, -1);
    lua_pop(w.L, 1);
    return v;
}

} // namespace

TEST_CASE("A builder and what it builds hear each quarter the build passes",
          "[build][assist][build-progress]") {
    // Moho's CBuildTaskHelper::UpdateWorkProgress: crossing 0.25, 0.5 or 0.75
    // since the builder last looked, OnBuildProgress(unit, old, new) and
    // OnBeingBuiltProgress(builder, old, new). The site goes 0.01 a tick.
    AssistSim w;
    listen_to_progress(w);
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    tick_until([&] { w.sim.tick(); }, "the site is built", 240,
               [&] { return !w.site->is_being_built(); });
    CHECK(progress(w) == "eng 0.25,site:eng 0.25,eng 0.5,site:eng 0.5,eng 0.75,site:eng 0.75");
}

TEST_CASE("A builder that joins a build late hears it passed the quarters behind it, from 0",
          "[build][assist][build-progress]") {
    // Its build task's fraction starts at 0: one call for the two quarters behind it
    AssistSim w;
    w.site->set_fraction_complete(0.6f);
    listen_to_progress(w);
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    tick_until([&] { w.sim.tick(); }, "the builder's first step", 10,
               [&] { return w.site->fraction_complete() > 0.6f; });
    CHECK(progress(w) == "eng 0.25 from 0,site:eng 0.25 from 0");
}

TEST_CASE("A builder paused through a quarter hears it on resuming, but not on FAF's executable",
          "[build][assist][build-progress][pause]") {
    // Retail's build task keeps what a paused builder last saw; FAF's copies
    // the fraction in while it is paused.
    for (const bool faf : {false, true}) {
        CAPTURE(faf);
        AssistSim w;
        osc::sim::set_sim(w.L, &w.sim); // the sim's own state holds it, as in a game
        w.sim.set_faf_exe(faf);
        w.site->set_fraction_complete(0.3f);
        listen_to_progress(w);
        Unit* eng = w.engineer("eng", 16.0f);
        Unit* other = w.engineer("other", 24.0f);
        eng->push_command(order(CommandType::Repair, *w.site, 1), true);
        other->push_command(order(CommandType::Repair, *w.site, 2), true);
        tick_until([&] { w.sim.tick(); }, "both builders start", 10,
                   [&] { return eng->is_building() && other->is_building(); });
        REQUIRE(w.site->fraction_complete() < 0.45f);
        progress(w);
        eng->set_paused(true);
        tick_until([&] { w.sim.tick(); }, "the other builder takes it past 0.5 alone", 60,
                   [&] { return w.site->fraction_complete() >= 0.55f; });
        CHECK(progress(w) == "other 0.5,site:other 0.5");
        eng->set_paused(false);
        w.sim.tick();
        CHECK(progress(w) == (faf ? "" : "eng 0.5,site:eng 0.5"));
    }
}

TEST_CASE("A helper with no build rate still hears the quarters others build",
          "[build][assist][build-progress]") {
    // It isn't paused: Moho's UpdateWorkProgress runs with nothing consumed
    AssistSim w;
    listen_to_progress(w);
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* idle = w.engineer("idle", 24.0f);
    idle->set_build_rate(0.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    idle->push_command(order(CommandType::Repair, *w.site, 2), true);
    tick_until([&] { w.sim.tick(); }, "the site is built", 240,
               [&] { return !w.site->is_being_built(); });
    const std::string heard = progress(w);
    for (const char* band : {"idle 0.25", "idle 0.5", "idle 0.75"}) {
        CAPTURE(band);
        CHECK(heard.find(band) != std::string::npos);
    }
}
