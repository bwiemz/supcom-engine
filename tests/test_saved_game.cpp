// Saved games (M208a): a save is the game's recording, and a load replays it
// to the saved tick before the player takes over.

#include <catch2/catch_test_macros.hpp>

#include "sim/build_info.hpp"
#include "sim/manipulator.hpp"
#include "sim/replay.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

using osc::sim::CommandType;
using osc::sim::SavedGame;
using osc::sim::SaveLoadError;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// The same world in every sim, as a scenario would build it.
std::vector<osc::u32> setup(SimState& sim) {
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    std::vector<osc::u32> ids;
    for (int i = 0; i < 3; ++i) {
        auto u = std::make_unique<Unit>();
        u->set_army(i % 2);
        u->set_max_speed(6.0f);
        u->set_position({static_cast<osc::f32>(i * 20), 0.0f, 0.0f});
        ids.push_back(sim.entity_registry().register_entity(std::move(u)));
    }
    osc::sim::GameSetup game;
    game.scenario = "/maps/test/test_scenario.lua";
    game.seed = 31;
    sim.set_game_setup(game);
    return ids;
}

UnitCommand move_to(osc::f32 x, osc::f32 z) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = {x, 0.0f, z};
    return c;
}

/// A player's order, as the UI gives one between ticks.
void player_order(SimState& sim, osc::u32 unit, const UnitCommand& cmd) {
    sim.set_human_input_active(true);
    sim.route_command({unit}, cmd, true);
    sim.set_human_input_active(false);
}

SavedGame sample_save() {
    SavedGame save;
    save.build = osc::sim::build_id();
    save.name = "Before the push";
    save.tick = 12;
    save.game.has_setup = true;
    save.game.setup.scenario = "/maps/SCMP_009/SCMP_009_scenario.lua";
    save.game.final_tick = 12;
    save.game.checksum_from = 1;
    save.game.checksums.assign(12, 0xabcdef01u);
    osc::sim::ScheduledCommand c;
    c.exec_tick = 13; // given before the save, not run yet
    c.unit_ids = {7};
    c.command = move_to(5.0f, 6.0f);
    save.game.commands.push_back(c);
    return save;
}

} // namespace

TEST_CASE("A saved game round-trips", "[savegame]") {
    const SavedGame save = sample_save();
    SavedGame out;
    REQUIRE(SavedGame::deserialize(save.serialize(), out) == SaveLoadError::None);
    CHECK(out.version == SavedGame::kVersion);
    CHECK(out.build == save.build);
    CHECK(out.name == "Before the push");
    CHECK(out.tick == 12);
    CHECK(out.game.setup.scenario == save.game.setup.scenario);
    CHECK(out.game.checksums == save.game.checksums);
    REQUIRE(out.game.commands.size() == 1);
    CHECK(out.game.commands[0].exec_tick == 13);
    CHECK(out.game.commands[0].command.target_pos.x == 5.0f);
}

TEST_CASE("A save that can't load says why, as retail's dialog words it", "[savegame]") {
    SavedGame out;
    const auto refused = [&](const std::vector<osc::u8>& bytes) {
        const SaveLoadError error = SavedGame::deserialize(bytes, out);
        CHECK(out.name.empty()); // nothing half-read
        return error;
    };
    CHECK(refused({}) == SaveLoadError::InvalidFormat);
    CHECK(refused({'O', 'S', 'C', 'R', 5, 0, 0, 0}) == SaveLoadError::InvalidFormat); // a replay

    SavedGame save = sample_save();
    auto bytes = save.serialize();
    // Cut short anywhere, it is refused whole.
    for (size_t cut : {size_t{3}, size_t{9}, size_t{20}, bytes.size() / 2, bytes.size() - 1}) {
        const std::vector<osc::u8> part(bytes.begin(),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK(refused(part) == SaveLoadError::InvalidFormat);
    }

    // Another save format, or another build: WrongVersion.
    auto newer = bytes;
    newer[7] = static_cast<osc::u8>(SavedGame::kVersion + 1);
    CHECK(refused(newer) == SaveLoadError::WrongVersion);
    save.build = "0.0.0-elsewhere";
    CHECK(refused(save.serialize()) == SaveLoadError::WrongVersion);

    // A recording that can't start a game, or doesn't reach the saved tick.
    save = sample_save();
    save.game.has_setup = false;
    CHECK(refused(save.serialize()) == SaveLoadError::InvalidFormat);
    save = sample_save();
    save.tick = 11;
    CHECK(refused(save.serialize()) == SaveLoadError::InvalidFormat);

    CHECK(std::string(osc::sim::save_load_error_name(SaveLoadError::WrongVersion)) ==
          "WrongVersion");
    CHECK(std::string(osc::sim::save_load_error_name(SaveLoadError::CantOpen)) == "CantOpen");
}

TEST_CASE("A loaded game plays on as the saved one did", "[savegame][sync]") {
    // --- The game, saved at tick 10 with a player's order still to run ---
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    a.set_seed(31);
    const auto ids = setup(a);
    a.set_recording(true);
    a.schedule_command(0, {ids[0]}, move_to(300.0f, 0.0f), true);
    for (int i = 0; i < 10; ++i) a.tick();
    player_order(a, ids[1], move_to(0.0f, 300.0f)); // runs at tick 11
    SavedGame loaded;
    const SavedGame save = osc::sim::save_game(a, "mid-game");
    CHECK(save.tick == 10);
    REQUIRE(SavedGame::deserialize(save.serialize(), loaded) == SaveLoadError::None);

    // The saved game plays on, the player giving one more order.
    for (int i = 0; i < 20; ++i) {
        if (a.tick_count() == 15) player_order(a, ids[2], move_to(-200.0f, 40.0f));
        a.tick();
    }

    // --- Loaded into a fresh sim ---
    LuaGuard gb;
    SimState b(gb.L, nullptr);
    b.set_seed(loaded.game.seed);
    REQUIRE(setup(b) == ids);
    b.set_recording(true);
    osc::sim::ReplayPlayback playback(loaded.game);
    playback.resume(b);
    CHECK(b.resuming());
    CHECK(b.playback()); // the player's input waits for the saved tick
    player_order(b, ids[2], move_to(999.0f, 999.0f));
    while (b.resuming()) {
        b.tick();
        REQUIRE(playback.check(b));
        REQUIRE(b.tick_count() <= 10);
    }
    CHECK(b.tick_count() == 10);
    CHECK_FALSE(b.playback()); // the player's again
    for (int i = 0; i < 20; ++i) {
        if (b.tick_count() == 15) player_order(b, ids[2], move_to(-200.0f, 40.0f));
        b.tick();
    }
    CHECK(b.compute_sync_checksum() == a.compute_sync_checksum());
    // Its recording is the whole game again, so it saves and loads too.
    CHECK(b.recorded_replay().checksums == a.recorded_replay().checksums);
    CHECK(b.recorded_replay().commands.size() == a.recorded_replay().commands.size());
    const auto* u = static_cast<const Unit*>(b.entity_registry().find(ids[1]));
    CHECK(u->position().z > 0.0f); // the order pending at the save ran
}

TEST_CASE("A game saved before its first tick is the player's at once", "[savegame]") {
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    const auto ids = setup(a);
    a.set_recording(true);
    player_order(a, ids[0], move_to(50.0f, 0.0f));
    const SavedGame save = osc::sim::save_game(a, "start");
    CHECK(save.tick == 0);
    REQUIRE(save.game.commands.size() == 1);

    LuaGuard gb;
    SimState b(gb.L, nullptr);
    setup(b);
    b.start_resume(save.game);
    CHECK_FALSE(b.resuming());
    CHECK_FALSE(b.playback());
    b.tick();
    a.tick();
    CHECK(b.compute_sync_checksum() == a.compute_sync_checksum());
}
