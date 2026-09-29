// --load-flow-test's frame (M208a, M191 step 4): App::Window.
//  0. the listed save loads from the front end, catches up (never a
//     replay meanwhile), plays on, and is saved again;
//  1. that save loads from inside the game, as the game menu's Load
//     dialog loads it, into a fresh UI state;
//  2. back to the front end (ReturnToLobby), a fresh state again;
//  3. the first save loads from there once more.

#include "app/window_loop.hpp"

namespace osc::app {

void App::Window::load_flow_globals() {
    lua_State* uL = ui_lua_state.raw();
    for (const auto& [name, value] : {std::pair{"__osc_load_profile", &load_flow_profile},
                                      std::pair{"__osc_load_file", &load_flow_file}}) {
        lua_pushstring(uL, name);
        lua_pushstring(uL, value->c_str());
        lua_rawset(uL, LUA_GLOBALSINDEX);
    }
}

void App::Window::load_flow_frame() {
    // --load-flow-test: the saved game catches up -- never a replay
    // meanwhile -- then plays on a little and is saved again.
    if (opt.load_flow_test && !load_flow_done) {
        ++load_flow_frames;
        if (sim_state && sim_state->resuming() && !load_flow_saw_catch_up) {
            load_flow_saw_catch_up = true;
            auto r = ui_lua_state.do_string(
                "if SessionIsReplay() then error('SessionIsReplay() is true while a "
                "saved game catches up') end");
            if (!r) osc::test_status::fail("[FAIL] load-flow: {}", r.error().message);
        }
        if (sim_state && load_flow_saw_catch_up && !catch_up && !load_flow_resumed_at) {
            load_flow_resumed_at = sim_state->tick_count();
            load_flow_resumes.push_back(*load_flow_resumed_at);
        }
        const bool played_on = sim_state && load_flow_resumed_at &&
                               sim_state->tick_count() >= *load_flow_resumed_at + 50;
        const auto next_load = [&] {
            load_flow_saw_catch_up = false;
            load_flow_resumed_at.reset();
        };
        const auto run = [&](const char* code) {
            load_flow_globals();
            if (auto r = ui_lua_state.do_string(code); !r)
                osc::test_status::fail("[FAIL] load-flow: {}", r.error().message);
        };
        // Each load draws its units' meshes, none a cube (every game
        // started from the front end once drew cubes: its caches had
        // no mesh cache). Retail's units all have meshes.
        if (played_on &&
            (renderer.mesh_instance_count() == 0 || renderer.cube_instance_count() != 0))
            osc::test_status::fail("[FAIL] load-flow: load {} drew {} meshes and {} cubes",
                                   load_flow_phase + 1, renderer.mesh_instance_count(),
                                   renderer.cube_instance_count());
        if (load_flow_phase == 0 && played_on) {
            save_again_in_load_flow();
            // The game menu's Load dialog: load, then destroy the
            // control it was opened over, GetFrame(0) in a game.
            run(R"(
                local worked, err = LoadSavedGame(__osc_again_file)
                if not worked then error('LoadSavedGame in a game: ' .. tostring(err)) end
                GetFrame(0):Destroy()
            )");
            next_load();
            load_flow_phase = 1;
        } else if (load_flow_phase == 1 && played_on) {
            run("ReturnToLobby()");
            load_flow_front_end_frames = 0;
            load_flow_phase = 2;
        } else if (load_flow_phase == 2 && !sim_state && ++load_flow_front_end_frames > 20) {
            run(R"(
                local worked, err = LoadSavedGame(__osc_load_file)
                if not worked then error('LoadSavedGame from the front end: ' ..
                                         tostring(err)) end
            )");
            next_load();
            load_flow_phase = 3;
        } else if (load_flow_phase == 3 && played_on) {
            load_flow_done = true;
        } else if (load_flow_frames > 60000) {
            load_flow_done = true; // stuck: reported below
        }
    }
}

void App::Window::save_again_in_load_flow() {
    load_flow_globals();
    auto saved = ui_lua_state.do_string(R"(
        if SessionIsReplay() then error('a loaded game is a replay') end
        local data = GetSpecialFiles('SaveGame')
        local folder = data.directory .. __osc_load_profile .. '/'
        __osc_again_file = folder .. 'again.' .. data.extension
        local result
        InternalSaveGame(__osc_again_file, 'again', function(worked, errmsg)
            result = {worked = worked, errmsg = errmsg}
        end)
        if not result then error('InternalSaveGame never called back') end
        if not result.worked then
            error('InternalSaveGame failed: ' .. tostring(result.errmsg))
        end
        if not GetSpecialFileInfo(__osc_load_profile, 'again', 'SaveGame') then
            error('the new save is not listed')
        end
        local refused
        InternalSaveGame(folder .. '../../outside.' .. data.extension, 'outside',
                         function(worked) refused = not worked end)
        if not refused then error('InternalSaveGame saved outside its folder') end
    )");
    if (!saved) {
        osc::test_status::fail("[FAIL] load-flow: {}", saved.error().message);
        return;
    }
    // The new save holds the whole game, as it stands.
    lua_State* uL = ui_lua_state.raw();
    lua_pushstring(uL, "__osc_again_file");
    lua_rawget(uL, LUA_GLOBALSINDEX);
    const std::string again = lua_type(uL, -1) == LUA_TSTRING ? lua_tostring(uL, -1) : "";
    lua_pop(uL, 1);
    osc::sim::SavedGame save;
    const osc::u32 tick = sim_state->tick_count();
    if (osc::lua::read_saved_game(again, save) != osc::sim::SaveLoadError::None) {
        osc::test_status::fail("[FAIL] load-flow: the new save {} does not load", again);
    } else if (save.tick != tick || save.game.checksums.size() != tick) {
        osc::test_status::fail("[FAIL] load-flow: the new save holds tick {} and {} "
                               "checksums, not the game's {}",
                               save.tick, save.game.checksums.size(), tick);
    }
    if (osc::fs::exists(special_files->root() / "outside.oscsave"))
        osc::test_status::fail("[FAIL] load-flow: a save was written outside its folder");
}

int App::Window::finish_load_flow() {
    // The first save resumes at its tick, the one saved in the game
    // 50 ticks later, then the first again.
    const bool all = load_flow_resumes.size() == 3 &&
                     load_flow_resumes[1] == load_flow_resumes[0] + 50 &&
                     load_flow_resumes[2] == load_flow_resumes[0];
    if (!all) {
        std::string got;
        for (const osc::u32 t : load_flow_resumes) got += fmt::format(" {}", t);
        osc::test_status::fail("[FAIL] load-flow: stopped in phase {}; the loads "
                               "resumed at:{}",
                               load_flow_phase, got.empty() ? " none" : got);
    } else if (osc::test_status::failure_count() == 0) {
        spdlog::info("[PASS] load-flow: loaded from the front end (tick {}), saved "
                     "again, loaded that from the game (tick {}), back to the lobby, "
                     "and loaded again (tick {}), each in a fresh UI state",
                     load_flow_resumes[0], load_flow_resumes[1], load_flow_resumes[2]);
    }
    return finish_test_run("load-flow-test");
}

} // namespace osc::app
