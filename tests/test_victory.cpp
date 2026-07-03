// Victory-condition / game-mode enforcement tests.
//
// These exercise SimState's mode-aware, team-aware victory logic directly at
// the sim layer (no game data required): armies and units are synthesized in
// the registry, then the sim is ticked past its grace period.

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

using osc::sim::ArmyBrain;
using osc::sim::BrainState;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::VictoryMode;

namespace {

// Spawn a bare unit belonging to `army` carrying the given categories.
// Returns the registry entity id so the caller can later destroy it.
osc::u32 spawn(SimState& sim, int army,
               std::initializer_list<const char*> categories) {
    auto unit = std::make_unique<Unit>();
    unit->set_army(army);
    for (const char* c : categories) unit->add_category(c);
    return sim.entity_registry().register_entity(std::move(unit));
}

void destroy(SimState& sim, osc::u32 id) {
    auto* e = sim.entity_registry().find(id);
    REQUIRE(e != nullptr);
    e->mark_destroyed();
}

void tick_n(SimState& sim, int n) {
    for (int i = 0; i < n; ++i) sim.tick();
}

// A raw Lua state is all SimState needs for these tests.
struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

constexpr int kPastGrace = 55; // > kVictoryGraceTicks (50)

} // namespace

TEST_CASE("parse_victory_mode maps FA keys and aliases", "[victory][mode]") {
    CHECK(osc::sim::parse_victory_mode("demoralization") == VictoryMode::Demoralization);
    CHECK(osc::sim::parse_victory_mode("assassination") == VictoryMode::Demoralization);
    CHECK(osc::sim::parse_victory_mode("domination") == VictoryMode::Domination);
    CHECK(osc::sim::parse_victory_mode("Supremacy") == VictoryMode::Domination);
    CHECK(osc::sim::parse_victory_mode("eradication") == VictoryMode::Eradication);
    CHECK(osc::sim::parse_victory_mode("ANNIHILATION") == VictoryMode::Eradication);
    CHECK(osc::sim::parse_victory_mode("sandbox") == VictoryMode::Sandbox);
    CHECK(osc::sim::parse_victory_mode("none") == VictoryMode::Sandbox);
    CHECK(osc::sim::parse_victory_mode("nonsense") == VictoryMode::Demoralization);
}

TEST_CASE("set_victory_condition normalizes aliases into the mode", "[victory][mode]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("Assassination");
    CHECK(sim.victory_mode() == VictoryMode::Demoralization);
    sim.set_victory_condition("SANDBOX");
    CHECK(sim.victory_mode() == VictoryMode::Sandbox);
    CHECK(sim.sandbox_victory());
}

TEST_CASE("Assassination: army is defeated when its ACU dies even with units left",
          "[victory][demoralization]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("demoralization");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");

    spawn(sim, 0, {"COMMAND"});
    osc::u32 enemy_acu = spawn(sim, 1, {"COMMAND"});
    spawn(sim, 1, {"MOBILE", "LAND"}); // a tank — should NOT save army 1

    tick_n(sim, kPastGrace);
    // Both alive, game running.
    CHECK(sim.get_army(0)->state() == BrainState::InProgress);
    CHECK(sim.get_army(1)->state() == BrainState::InProgress);
    CHECK_FALSE(sim.game_ended());

    destroy(sim, enemy_acu);
    tick_n(sim, 2);

    CHECK(sim.get_army(1)->state() == BrainState::Defeat);
    CHECK(sim.get_army(0)->state() == BrainState::Victory);
    CHECK(sim.game_ended());
    CHECK(sim.player_result() == 1);
}

TEST_CASE("Annihilation: losing the ACU does not defeat you while units remain",
          "[victory][eradication]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("eradication");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");

    spawn(sim, 0, {"COMMAND"});
    osc::u32 enemy_acu = spawn(sim, 1, {"COMMAND"});
    osc::u32 enemy_tank = spawn(sim, 1, {"MOBILE", "LAND"});

    tick_n(sim, kPastGrace);
    destroy(sim, enemy_acu);
    tick_n(sim, 2);

    // ACU gone but a tank survives → army 1 still in the game.
    CHECK(sim.get_army(1)->state() == BrainState::InProgress);
    CHECK_FALSE(sim.game_ended());

    destroy(sim, enemy_tank);
    tick_n(sim, 2);

    // Last unit gone → eliminated, army 0 wins.
    CHECK(sim.get_army(1)->state() == BrainState::Defeat);
    CHECK(sim.get_army(0)->state() == BrainState::Victory);
    CHECK(sim.game_ended());
}

TEST_CASE("Supremacy: a structure keeps you alive, a lone wall does not",
          "[victory][domination]") {
    SECTION("structure survives (not eliminated even with no ACU)") {
        LuaGuard g;
        SimState sim(g.L, nullptr);
        sim.set_victory_condition("supremacy"); // alias → domination
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");

        spawn(sim, 0, {"COMMAND"});
        spawn(sim, 1, {"STRUCTURE", "FACTORY"}); // no ACU, but a factory

        tick_n(sim, kPastGrace);
        // Domination cares about structures/units, not the commander.
        CHECK(sim.get_army(1)->state() == BrainState::InProgress);
        CHECK_FALSE(sim.game_ended());
    }

    SECTION("a lone wall is not significant → eliminated") {
        LuaGuard g;
        SimState sim(g.L, nullptr);
        sim.set_victory_condition("domination");
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");

        spawn(sim, 0, {"COMMAND"});
        spawn(sim, 1, {"WALL", "STRUCTURE"}); // walls don't count for domination

        tick_n(sim, kPastGrace + 2);
        CHECK(sim.get_army(1)->state() == BrainState::Defeat);
        CHECK(sim.game_ended());
    }
}

TEST_CASE("Sandbox: no army is ever eliminated", "[victory][sandbox]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");

    spawn(sim, 0, {"COMMAND"});
    osc::u32 enemy_acu = spawn(sim, 1, {"COMMAND"});

    tick_n(sim, kPastGrace);
    destroy(sim, enemy_acu); // army 1 now has zero units
    tick_n(sim, 5);

    CHECK(sim.get_army(0)->state() == BrainState::InProgress);
    CHECK(sim.get_army(1)->state() == BrainState::InProgress);
    CHECK_FALSE(sim.game_ended());
    CHECK(sim.player_result() == 0);
}

TEST_CASE("Teams: allied victory when the last enemy team is eliminated",
          "[victory][teams]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("demoralization");
    sim.add_army("ARMY_1", "ARMY_1"); // player, team A
    sim.add_army("ARMY_2", "ARMY_2"); // ally, team A
    sim.add_army("ARMY_3", "ARMY_3"); // enemy, team B
    sim.set_alliance(0, 1, osc::sim::Alliance::Ally);

    spawn(sim, 0, {"COMMAND"});
    spawn(sim, 1, {"COMMAND"});
    osc::u32 enemy_acu = spawn(sim, 2, {"COMMAND"});

    tick_n(sim, kPastGrace);
    CHECK(sim.surviving_team_count() == 2);

    destroy(sim, enemy_acu);
    tick_n(sim, 2);

    CHECK(sim.get_army(2)->state() == BrainState::Defeat);
    CHECK(sim.get_army(0)->state() == BrainState::Victory);
    CHECK(sim.get_army(1)->state() == BrainState::Victory); // ally shares the win
    CHECK(sim.game_ended());
    CHECK(sim.player_result() == 1);
}

TEST_CASE("Teams: you lose but the game continues while an ally fights on",
          "[victory][teams]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("demoralization");
    sim.add_army("ARMY_1", "ARMY_1"); // player, team A
    sim.add_army("ARMY_2", "ARMY_2"); // ally, team A
    sim.add_army("ARMY_3", "ARMY_3"); // enemy, team B
    sim.set_alliance(0, 1, osc::sim::Alliance::Ally);

    osc::u32 player_acu = spawn(sim, 0, {"COMMAND"});
    spawn(sim, 1, {"COMMAND"});
    spawn(sim, 2, {"COMMAND"});

    tick_n(sim, kPastGrace);
    destroy(sim, player_acu);
    tick_n(sim, 2);

    CHECK(sim.get_army(0)->state() == BrainState::Defeat);
    CHECK(sim.player_result() == 2);     // the player has lost
    CHECK_FALSE(sim.game_ended());       // but team A (ally) vs team B continues
    CHECK(sim.surviving_team_count() == 2);
}

TEST_CASE("Draw: simultaneous mutual elimination", "[victory][draw]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("demoralization");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");

    osc::u32 acu0 = spawn(sim, 0, {"COMMAND"});
    osc::u32 acu1 = spawn(sim, 1, {"COMMAND"});

    tick_n(sim, kPastGrace);
    destroy(sim, acu0);
    destroy(sim, acu1);
    tick_n(sim, 2);

    CHECK(sim.get_army(0)->state() == BrainState::Draw);
    CHECK(sim.get_army(1)->state() == BrainState::Draw);
    CHECK(sim.game_ended());
    CHECK(sim.player_result() == 3);
}

TEST_CASE("Grace: an army that never spawned keeps the game open",
          "[victory][grace]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("eradication");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2"); // never gets any units

    spawn(sim, 0, {"COMMAND"});

    tick_n(sim, kPastGrace + 10);

    // Army 2 has no units but also never had any → not eliminated, so no
    // premature victory is declared for army 0.
    CHECK(sim.get_army(0)->state() == BrainState::InProgress);
    CHECK(sim.get_army(1)->state() == BrainState::InProgress);
    CHECK_FALSE(sim.game_ended());
    CHECK(sim.player_result() == 0);
}

TEST_CASE("Grace: an ACU killed during the grace window is honored after it",
          "[victory][grace]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("demoralization");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");

    spawn(sim, 0, {"COMMAND"});
    osc::u32 enemy_acu = spawn(sim, 1, {"COMMAND"});

    tick_n(sim, 5); // still inside the grace window
    destroy(sim, enemy_acu);
    tick_n(sim, 3); // still inside grace — no elimination yet
    CHECK(sim.get_army(1)->state() == BrainState::InProgress);
    CHECK_FALSE(sim.game_ended());

    tick_n(sim, kPastGrace); // cross the grace boundary
    CHECK(sim.get_army(1)->state() == BrainState::Defeat);
    CHECK(sim.game_ended());
}

TEST_CASE("parse_share_mode maps FA keys", "[victory][share]") {
    using osc::sim::ShareMode;
    CHECK(osc::sim::parse_share_mode("ShareUntilDeath") == ShareMode::ShareUntilDeath);
    CHECK(osc::sim::parse_share_mode("FullShare") == ShareMode::FullShare);
    CHECK(osc::sim::parse_share_mode("CivilianDeserter") == ShareMode::CivilianDeserter);
    CHECK(osc::sim::parse_share_mode("whatever") == ShareMode::ShareUntilDeath);
}

TEST_CASE("ShareUntilDeath destroys a defeated army's remaining units",
          "[victory][share]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("demoralization"); // default share = ShareUntilDeath
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");

    spawn(sim, 0, {"COMMAND"});
    osc::u32 enemy_acu = spawn(sim, 1, {"COMMAND"});
    osc::u32 enemy_tank = spawn(sim, 1, {"MOBILE", "LAND"});

    tick_n(sim, kPastGrace);
    destroy(sim, enemy_acu);
    tick_n(sim, 2);

    auto* tank = static_cast<Unit*>(sim.entity_registry().find(enemy_tank));
    REQUIRE(tank != nullptr);
    CHECK(tank->is_dying());               // leftover unit is being destroyed
    CHECK(sim.get_army(1)->state() == BrainState::Defeat);
}

TEST_CASE("FullShare transfers a defeated army's units to a surviving ally",
          "[victory][share]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("demoralization");
    sim.set_share_condition("FullShare");
    sim.add_army("ARMY_1", "ARMY_1"); // player, team A
    sim.add_army("ARMY_2", "ARMY_2"); // ally, team A
    sim.add_army("ARMY_3", "ARMY_3"); // enemy, team B
    sim.set_alliance(0, 1, osc::sim::Alliance::Ally);

    osc::u32 player_acu = spawn(sim, 0, {"COMMAND"});
    osc::u32 player_tank = spawn(sim, 0, {"MOBILE", "LAND"});
    spawn(sim, 1, {"COMMAND"});
    spawn(sim, 2, {"COMMAND"});

    tick_n(sim, kPastGrace);
    destroy(sim, player_acu);
    tick_n(sim, 2);

    auto* tank = static_cast<Unit*>(sim.entity_registry().find(player_tank));
    REQUIRE(tank != nullptr);
    CHECK(tank->army() == 1);              // handed to the ally
    CHECK_FALSE(tank->is_dying());
    CHECK(sim.get_army(0)->state() == BrainState::Defeat);
    CHECK_FALSE(sim.game_ended());         // team A (ally) fights on
}

TEST_CASE("CivilianDeserter hands a defeated army's units to a civilian army",
          "[victory][share]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("demoralization");
    sim.set_share_condition("CivilianDeserter");
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.add_army("CIVILIAN", "CIVILIAN"); // civilian recipient (not a participant)

    spawn(sim, 0, {"COMMAND"});
    osc::u32 enemy_acu = spawn(sim, 1, {"COMMAND"});
    osc::u32 enemy_tank = spawn(sim, 1, {"MOBILE", "LAND"});

    tick_n(sim, kPastGrace);
    destroy(sim, enemy_acu);
    tick_n(sim, 2);

    auto* tank = static_cast<Unit*>(sim.entity_registry().find(enemy_tank));
    REQUIRE(tank != nullptr);
    CHECK(tank->army() == 2);              // deserted to the civilians
    CHECK(sim.get_army(1)->state() == BrainState::Defeat);
    CHECK(sim.get_army(0)->state() == BrainState::Victory);
}

TEST_CASE("Single participant never auto-resolves", "[victory][edge]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("eradication");
    sim.add_army("ARMY_1", "ARMY_1"); // only participant

    spawn(sim, 0, {"COMMAND"});
    tick_n(sim, kPastGrace + 5);

    CHECK(sim.get_army(0)->state() == BrainState::InProgress);
    CHECK_FALSE(sim.game_ended());
}
