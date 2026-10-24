// Saved games (M208a): a save is the game's recording, and a load replays it
// to the saved tick before the player takes over.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/session_clients.hpp"
#include "lua/sim_sync.hpp"
#include "sim/build_info.hpp"
#include "sim/lua_sim.hpp"
#include "sim/manipulator.hpp"
#include "sim/replay.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using osc::sim::CommandType;
using osc::sim::Replay;
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

/// What SessionIsBeingRecorded() answers in the UI's state of `sim`'s game.
bool ui_says_recorded(SimState& sim) {
    osc::lua::LuaState ui;
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_ui_bindings(ui, registry);
    osc::GameStateManager game_state; // in a game, as the function asks
    game_state.transition_to(osc::GameState::GAME, nullptr);
    lua_pushstring(ui.raw(), "__osc_game_state_mgr");
    lua_pushlightuserdata(ui.raw(), &game_state);
    lua_rawset(ui.raw(), LUA_REGISTRYINDEX);
    auto r = ui.do_string("recorded = SessionIsBeingRecorded()");
    INFO((r.ok() ? std::string() : r.error().message));
    REQUIRE(r.ok());
    lua_pushstring(ui.raw(), "recorded");
    lua_rawget(ui.raw(), LUA_GLOBALSINDEX);
    REQUIRE(lua_type(ui.raw(), -1) == LUA_TBOOLEAN);
    const bool recorded = lua_toboolean(ui.raw(), -1) != 0;
    lua_pop(ui.raw(), 1);
    return recorded;
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
    save.beat = 12;
    save.game.has_setup = true;
    save.game.setup.scenario = "/maps/SCMP_009/SCMP_009_scenario.lua";
    save.game.final_beat = 12;
    save.game.checksum_from = 1;
    save.game.checksums.assign(12, 0xabcdef01u);
    osc::sim::ScheduledCommand c;
    c.exec_beat = 13; // given before the save, not run yet
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
    CHECK(out.beat == 12);
    CHECK(out.game.setup.scenario == save.game.setup.scenario);
    CHECK(out.game.checksums == save.game.checksums);
    REQUIRE(out.game.commands.size() == 1);
    CHECK(out.game.commands[0].exec_beat == 13);
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
    save.beat = 11;
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
    player_order(a, ids[1], move_to(0.0f, 300.0f)); // runs in beat 11
    SavedGame loaded;
    const SavedGame save = osc::sim::save_game(a, "mid-game");
    CHECK(save.beat == 10);
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
    CHECK(save.beat == 0);
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

TEST_CASE("A game restored from its snapshot plays on as the saved one did", "[savegame][sync]") {
    // --- The game, saved at tick 10 with a player's order still to run ---
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    a.set_seed(31);
    const auto ids = setup(a);
    a.set_recording(true);
    a.schedule_command(0, {ids[0]}, move_to(300.0f, 0.0f), true);
    for (int i = 0; i < 10; ++i) a.tick();
    player_order(a, ids[1], move_to(0.0f, 300.0f)); // runs in beat 11
    const SavedGame save = osc::sim::save_game(a, "mid-game");
    REQUIRE_FALSE(save.snapshot.empty());
    SavedGame loaded;
    REQUIRE(SavedGame::deserialize(save.serialize(), loaded) == SaveLoadError::None);
    CHECK(loaded.snapshot == save.snapshot); // through the file's compression
    for (int i = 0; i < 20; ++i) {
        if (a.tick_count() == 15) player_order(a, ids[2], move_to(-200.0f, 40.0f));
        a.tick();
    }

    // --- Restored into a sim booted for the same game ---
    LuaGuard gb;
    SimState b(gb.L, nullptr);
    b.set_seed(loaded.game.seed);
    REQUIRE(setup(b) == ids);
    const std::string err = osc::sim::load_snapshot(b, loaded.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    b.set_recording(true);
    b.adopt_history(loaded.game);
    CHECK(b.tick_count() == 10);
    CHECK(b.compute_sync_checksum() == loaded.game.checksums.back());
    for (int i = 0; i < 20; ++i) {
        if (b.tick_count() == 15) player_order(b, ids[2], move_to(-200.0f, 40.0f));
        b.tick();
    }
    CHECK(b.compute_sync_checksum() == a.compute_sync_checksum());
    // Its recording is the whole game, so it saves and loads again.
    CHECK(b.recorded_replay().checksums == a.recorded_replay().checksums);
    CHECK(b.recorded_replay().commands.size() == a.recorded_replay().commands.size());
}

TEST_CASE("A snapshot holds Moho's random stream where the game left it", "[savegame][sync]") {
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    a.set_seed(31);
    a.set_moho_random(true);
    const auto ids = setup(a);
    a.set_recording(true);
    for (int i = 0; i < 700; ++i) {
        a.random().next_u32();
    }
    const SavedGame save = osc::sim::save_game(a, "mt");
    REQUIRE_FALSE(save.snapshot.empty());

    LuaGuard gb;
    SimState b(gb.L, nullptr);
    b.set_seed(31);
    REQUIRE(setup(b) == ids);
    const std::string err = osc::sim::load_snapshot(b, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    CHECK(b.moho_random());
    CHECK(b.compute_sync_checksum() == a.compute_sync_checksum());
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(b.random().next_u32() == a.random().next_u32());
    }

    LuaGuard gc;
    SimState c(gc.L, nullptr);
    c.set_seed(31);
    setup(c);
    c.set_recording(true);
    const SavedGame plain = osc::sim::save_game(c, "splitmix");
    LuaGuard gd;
    SimState d(gd.L, nullptr);
    d.set_seed(31);
    d.set_moho_random(true);
    setup(d);
    REQUIRE(osc::sim::load_snapshot(d, plain.snapshot).empty());
    CHECK_FALSE(d.moho_random());
    CHECK(d.random().next_u64() == c.random().next_u64());
}

TEST_CASE("A restored game's history records the pathing it plays with", "[savegame]") {
    // A save from before replay format 16 doesn't say; its snapshot does,
    // and a save of the restored game must carry it.
    Replay old;
    old.version = 15;
    old.has_setup = true;
    old.setup.scenario = "/maps/x/x_scenario.lua";
    REQUIRE_FALSE(old.setup.moho_pathing);

    LuaGuard g;
    SimState s(g.L, nullptr);
    s.set_moho_pathing(true); // as load_snapshot leaves it
    s.adopt_history(old);
    CHECK(s.recorded_replay().setup.moho_pathing);

    Replay again;
    REQUIRE(Replay::deserialize(s.recorded_replay().serialize(), again));
    CHECK(again.version == Replay::kVersion);
    CHECK(again.setup.moho_pathing);
}

TEST_CASE("A damaged snapshot is refused before anything loads", "[savegame]") {
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    setup(a);
    for (int i = 0; i < 5; ++i) a.tick();
    std::vector<osc::u8> snap;
    REQUIRE(osc::sim::save_snapshot(a, snap).empty());
    for (size_t at : {size_t{3}, snap.size() / 3, snap.size() / 2, snap.size() - 9}) {
        auto bad = snap;
        bad[at] ^= 0x10;
        LuaGuard gb;
        SimState b(gb.L, nullptr);
        setup(b);
        CHECK_FALSE(osc::sim::load_snapshot(b, bad).empty());
        CHECK(b.tick_count() == 0); // untouched
    }
    LuaGuard gc;
    SimState c(gc.L, nullptr);
    setup(c);
    c.tick();
    CHECK_FALSE(osc::sim::load_snapshot(c, snap).empty()); // only into a sim that hasn't run
}

TEST_CASE("Only its own installation's snapshot is trusted", "[savegame]") {
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    setup(a);
    a.set_recording(true);
    a.tick();
    SavedGame save = osc::sim::save_game(a, "signed");
    REQUIRE_FALSE(save.snapshot.empty());
    osc::sim::SnapshotKey mine{}, theirs{};
    mine.fill(7);
    theirs.fill(9);
    osc::core::Sha256Digest binary{};
    binary.fill(3);
    CHECK_FALSE(osc::sim::snapshot_signed(save, mine)); // not signed yet
    osc::sim::sign_snapshot(save, mine, binary);
    SavedGame loaded;
    REQUIRE(SavedGame::deserialize(save.serialize(), loaded) == SaveLoadError::None);
    CHECK(loaded.snapshot_binary == binary); // the binary that took it, carried
    CHECK(osc::sim::snapshot_signed(loaded, mine));
    CHECK_FALSE(osc::sim::snapshot_signed(loaded, theirs));
    SavedGame relabelled = loaded;
    relabelled.snapshot_binary[0] ^= 1; // the signature covers the binary
    CHECK_FALSE(osc::sim::snapshot_signed(relabelled, mine));
    loaded.snapshot[loaded.snapshot.size() / 2] ^= 1; // changed after signing
    CHECK_FALSE(osc::sim::snapshot_signed(loaded, mine));
}

namespace {

struct PauseHeard {
    LuaGuard ui;
    explicit PauseHeard(SimState& sim) {
        osc::sim::set_sim(ui.L, &sim);
        const char* hooks = "pauses, resumes = 0, 0 "
                            "function OnPause() pauses = pauses + 1 end "
                            "function OnResume() resumes = resumes + 1 end";
        REQUIRE(luaL_loadbuffer(ui.L, hooks, std::strlen(hooks), "hooks") == 0);
        REQUIRE(lua_pcall(ui.L, 0, 0, 0) == 0);
    }
    int count(const char* name) const {
        lua_pushstring(ui.L, name);
        lua_rawget(ui.L, LUA_GLOBALSINDEX);
        const int n = static_cast<int>(lua_tonumber(ui.L, -1));
        lua_pop(ui.L, 1);
        return n;
    }
    bool session_paused() const {
        bool paused = true;
        REQUIRE(osc::lua::session_is_paused(ui.L, paused));
        return paused;
    }
    void frame() const { osc::lua::pump_pause_state(ui.L); }
};

} // namespace

TEST_CASE("A game saved on pause loads unpaused, as retail's, and replays as it played",
          "[savegame][pause]") {
    const osc::u32 source = GENERATE(0u, 1u);
    const bool from_snapshot = GENERATE(true, false);
    const auto boot = [](SimState& sim) {
        sim.set_seed(31);
        auto ids = setup(sim);
        sim.set_pause_timeouts(0, 3);
        sim.set_pause_timeouts(1, 3);
        return ids;
    };
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    const auto ids = boot(a);
    a.set_recording(true);
    for (int i = 0; i < 5; ++i) {
        a.tick();
    }
    osc::sim::SimCallbackEntry pause;
    pause.func_name = osc::sim::kRequestPauseCallback;
    a.schedule_callback(source, std::move(pause));
    for (int i = 0; i < 4; ++i) {
        a.tick();
    }
    REQUIRE(a.paused_by() == static_cast<osc::i32>(source));
    REQUIRE(a.tick_count() == 5);
    const SavedGame save = osc::sim::save_game(a, "paused");
    REQUIRE_FALSE(save.snapshot.empty());

    LuaGuard gb;
    SimState b(gb.L, nullptr);
    REQUIRE(boot(b) == ids);
    b.set_recording(true);
    if (from_snapshot) {
        REQUIRE(osc::sim::load_snapshot(b, save.snapshot).empty());
        b.adopt_history(save.game);
    } else {
        osc::sim::ReplayPlayback catch_up(save.game);
        catch_up.resume(b);
        while (b.resuming()) {
            b.tick();
            REQUIRE(catch_up.check(b));
        }
    }
    REQUIRE(b.beat_count() == a.beat_count());
    PauseHeard ui(b);
    b.schedule_post_load();
    CHECK_FALSE(b.paused());
    CHECK_FALSE(ui.session_paused());
    for (int i = 0; i < 5; ++i) {
        ui.frame();
        b.tick();
    }
    ui.frame();
    CHECK(b.tick_count() == 10);
    CHECK(ui.count("pauses") == 0);
    CHECK(ui.count("resumes") == 0);
    CHECK(b.pause_timeouts(source) == 2);

    const SavedGame again = osc::sim::save_game(b, "again");
    LuaGuard gc;
    SimState c(gc.L, nullptr);
    REQUIRE(boot(c) == ids);
    c.set_recording(true);
    osc::sim::ReplayPlayback replay(again.game);
    replay.resume(c);
    while (c.resuming()) {
        c.tick();
        REQUIRE(replay.check(c));
    }
    CHECK(c.tick_count() == 10);
    CHECK_FALSE(c.paused());
}

TEST_CASE("A save holds none of the Sync its user side hasn't had", "[savegame][sync]") {
    const auto run = [](lua_State* L, const char* code) {
        REQUIRE(luaL_loadbuffer(L, code, std::strlen(code), "code") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    };
    const auto boot = [&](lua_State* L, SimState& sim, osc::lua::SyncQueue& sync) {
        run(L, "function ResetSyncTable() Sync = {} end ResetSyncTable()");
        setup(sim);
        sim.set_beat_observer([&sync](const SimState& s) {
            lua_State* SL = s.lua_state();
            lua_pushstring(SL, "Sync");
            lua_rawget(SL, LUA_GLOBALSINDEX);
            lua_pushstring(SL, "Beat");
            lua_pushnumber(SL, s.beat_count());
            lua_rawset(SL, -3);
            lua_pop(SL, 1);
            sync.queue(SL, nullptr);
        });
    };
    const auto delivered = [&](osc::lua::SyncQueue& sync, lua_State* sim_L) {
        LuaGuard ui;
        run(ui.L, "seen = '' function OnSync() seen = seen .. Sync.Beat .. ',' end");
        while (sync.pending()) {
            sync.deliver(sim_L, ui.L);
        }
        lua_pushstring(ui.L, "seen");
        lua_rawget(ui.L, LUA_GLOBALSINDEX);
        std::string seen = lua_tostring(ui.L, -1);
        lua_pop(ui.L, 1);
        return seen;
    };

    LuaGuard ga;
    SimState a(ga.L, nullptr);
    osc::lua::SyncQueue sync_a;
    boot(ga.L, a, sync_a);
    a.set_recording(true);
    for (int i = 0; i < 3; ++i) {
        a.tick();
    }
    const SavedGame save = osc::sim::save_game(a, "three beats queued");
    REQUIRE_FALSE(save.snapshot.empty());
    CHECK(delivered(sync_a, ga.L) == "1,2,3,");

    LuaGuard gb;
    SimState b(gb.L, nullptr);
    osc::lua::SyncQueue sync_b;
    boot(gb.L, b, sync_b);
    REQUIRE(osc::sim::load_snapshot(b, save.snapshot).empty());
    b.tick();
    CHECK(delivered(sync_b, gb.L) == "4,");
}

TEST_CASE("SessionIsBeingRecorded answers true in a recorded new game", "[savegame][lua]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    setup(sim);
    CHECK_FALSE(ui_says_recorded(sim)); // not recording, as a replay plays
    sim.set_recording(true);
    sim.tick();
    CHECK_FALSE(sim.loaded_from_save());
    CHECK(ui_says_recorded(sim));
}

TEST_CASE("SessionIsBeingRecorded answers false in a game loaded from a save", "[savegame][lua]") {
    // Moho's CSavedGame::CreateSinglePlayerSessionInfo starts a loaded save's
    // session unrecorded. The engine's sim records it all the same (a later
    // save writes its history), so the answer is the session's, not the
    // recording's.
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    a.set_seed(31);
    const auto ids = setup(a);
    a.set_recording(true);
    a.schedule_command(0, {ids[0]}, move_to(300.0f, 0.0f), true);
    for (int i = 0; i < 10; ++i) a.tick();
    const SavedGame save = osc::sim::save_game(a, "mid-game");
    REQUIRE_FALSE(save.snapshot.empty());

    // Restored from its snapshot, as App::restore_save does
    LuaGuard gb;
    SimState b(gb.L, nullptr);
    b.set_seed(save.game.seed);
    setup(b);
    REQUIRE(osc::sim::load_snapshot(b, save.snapshot).empty());
    b.set_recording(true);
    b.adopt_history(save.game);
    CHECK(b.recording());
    CHECK(b.loaded_from_save());
    CHECK_FALSE(ui_says_recorded(b));

    // Caught up from its history instead, as the load falls back to
    LuaGuard gc;
    SimState c(gc.L, nullptr);
    c.set_seed(save.game.seed);
    setup(c);
    osc::sim::ReplayPlayback(save.game).resume(c);
    c.set_recording(true);
    CHECK(c.recording());
    CHECK(c.loaded_from_save());
    CHECK_FALSE(ui_says_recorded(c));
    while (c.resuming()) c.tick();
    CHECK_FALSE(ui_says_recorded(c)); // the player's again: still a loaded game
}

TEST_CASE("A game loaded from a save saves again with its whole history", "[savegame]") {
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    a.set_seed(31);
    const auto ids = setup(a);
    a.set_recording(true);
    a.schedule_command(0, {ids[0]}, move_to(300.0f, 0.0f), true);
    for (int i = 0; i < 10; ++i) a.tick();
    const SavedGame first = osc::sim::save_game(a, "first");
    for (int i = 0; i < 10; ++i) a.tick();

    LuaGuard gb;
    SimState b(gb.L, nullptr);
    b.set_seed(first.game.seed);
    setup(b);
    REQUIRE(osc::sim::load_snapshot(b, first.snapshot).empty());
    b.set_recording(true);
    b.adopt_history(first.game);
    for (int i = 0; i < 10; ++i) b.tick();
    REQUIRE(b.loaded_from_save()); // unrecorded, as the session reports it...
    const SavedGame again = osc::sim::save_game(b, "again");
    // ...and the save holds the game from its first tick
    CHECK(again.beat == 20);
    CHECK(again.game.checksums == a.recorded_replay().checksums);
    CHECK(again.game.commands.size() == a.recorded_replay().commands.size());
}
