# Integration tests. The test modes run in osc_integration: the engine's run
# (src/app) with the modes of runner/. The game's own features (AI games,
# replays, captures, binding coverage) run in `opensupcom`.

add_executable(osc_integration
    runner/main.cpp
    runner/test_modes.cpp
    runner/lan_modes.cpp
    runner/integration_tests.cpp
    runner/audio_data_test.cpp
    runner/map_parse_test.cpp
    runner/terrain_glow_test.cpp
    runner/campaign_test.cpp
    runner/persist_test.cpp
    runner/wave_test.cpp
    runner/movie_test.cpp
    runner/edit_text_test.cpp
    runner/keymap_test.cpp
    runner/camera_moves_test.cpp
    runner/window_test.cpp
    runner/options_test.cpp
    runner/lighting_test.cpp
    runner/render_probe.cpp
    runner/strata_test.cpp
    runner/meshless_test.cpp
    runner/camera_test.cpp
    runner/material_test.cpp
    runner/bloom_test.cpp
    runner/build_shader_test.cpp
    runner/effect_mesh_test.cpp
    runner/skinning_test.cpp
    runner/prop_material_test.cpp
    runner/clipped_shadow_test.cpp
    runner/shadow_map_test.cpp
    runner/unit_intel_test.cpp
    runner/intel_probe.cpp
    runner/strategic_icon_test.cpp
    runner/counter_intel_test.cpp
    runner/binding_tail_test.cpp
    runner/destroyed_handle_test.cpp
    runner/jammer_blip_test.cpp
    runner/intel_field_test.cpp
    runner/beam_render_test.cpp
    runner/selection_render_test.cpp
    runner/ghost_render_test.cpp
    runner/trail_render_test.cpp
    runner/particle_render_test.cpp
    runner/water_render_test.cpp
    runner/sky_test.cpp
    runner/shield_render_test.cpp
    runner/mesh_capacity_test.cpp
    runner/water_reflection_test.cpp
    runner/refract_render_test.cpp
    runner/decal_render_test.cpp
    runner/runtime_decal_test.cpp
    runner/terrain_normal_render_test.cpp
    runner/plate_fixtures.cpp
    runner/colors_test.cpp
    runner/script_order_test.cpp
    runner/patrol_test.cpp
)
target_include_directories(osc_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runner)
target_link_libraries(osc_integration PRIVATE osc::warnings osc::app)
# Beside opensupcom (on Windows with the same DLLs, which the cross-OS
# check's artifact carries); crash backtraces show function names.
set_target_properties(osc_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    ENABLE_EXPORTS ON)
#
# Labels:
#   mp          two processes over localhost TCP; need no game data (run in CI)
#   data        need Forged Alliance game data, located like `opensupcom
#               --print-install` does (OSC_FA_PATH, FAForever, or a Steam
#               library). Exit code 77 = no data = skipped.
#   gate        data tests that pass on retail FA 3599 today: the regression
#               gate to run before merging (`ctest -L gate`)
#   golden      render a fixed frame and compare with a recorded golden image
#   retail-gap  data tests that currently fail on retail data because of known
#               gaps (roadmap Phase B, M185/M186). They move to `gate` as the
#               gaps close; `ctest -L data` runs both groups.

find_package(Python3 COMPONENTS Interpreter)

# ── Two-process multiplayer (data-free) ────────────────────────────────────
if(Python3_Interpreter_FOUND)
    set(_run_pair ${CMAKE_CURRENT_SOURCE_DIR}/run_pair.py)
    # Distinct ports so the tests can run in parallel, below the ephemeral
    # ranges (Linux 32768+, Windows 49152+) an outgoing connection may hold.
    # Each pair hosts a lobby, launches, and plays in lockstep over its
    # connections (the network a lobby's game plays over).
    add_test(NAME mp.lockstep_sync
             COMMAND ${Python3_EXECUTABLE} ${_run_pair} $<TARGET_FILE:osc_integration> 29731)
    add_test(NAME mp.lockstep_desync_detected
             COMMAND ${Python3_EXECUTABLE} ${_run_pair} $<TARGET_FILE:osc_integration> 29732
                     -- --mp-desync)
    add_test(NAME mp.lockstep_peer_drop
             COMMAND ${Python3_EXECUTABLE} ${_run_pair} $<TARGET_FILE:osc_integration> 29733
                     --client-may-fail -- --mp-drop-at 20)
    # A joiner taking 200 ms a round (a slow machine) sets the game's pace:
    # the host, twice as fast, waits for it rather than dropping it (M218h)
    add_test(NAME mp.lockstep_slow_peer
             COMMAND ${Python3_EXECUTABLE} ${_run_pair} $<TARGET_FILE:osc_integration> 29734
                     -- --mp-slow 200 --mp-frames 80)
    set_tests_properties(mp.lockstep_sync mp.lockstep_desync_detected mp.lockstep_peer_drop
                         mp.lockstep_slow_peer PROPERTIES LABELS mp TIMEOUT 240)
else()
    message(STATUS "Python3 not found: two-process multiplayer tests not registered")
endif()

# ── Data-backed modes (need Forged Alliance) ───────────────────────────────
set(OSC_TEST_MAP "/maps/SCMP_009/SCMP_009_scenario.lua"
    CACHE STRING "Scenario used by data-backed integration tests")

include(${CMAKE_CURRENT_SOURCE_DIR}/data_tests.cmake)

function(osc_add_data_test mode group)
    if(ARGN STREQUAL "NO_MAP")
        add_test(NAME data.${mode} COMMAND osc_integration --${mode})
    else()
        add_test(NAME data.${mode}
                 COMMAND osc_integration --map ${OSC_TEST_MAP} --${mode})
    endif()
    set_tests_properties(data.${mode} PROPERTIES
        LABELS "data;${group}"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)
endfunction()

foreach(mode IN LISTS OSC_DATA_TESTS_GATE)
    osc_add_data_test(${mode} gate)
endforeach()
# M212f: the one TTerrainGlow map, Varga Pass
add_test(NAME data.terrain-glow-test
         COMMAND osc_integration --map /maps/SCMP_023/SCMP_023_scenario.lua --terrain-glow-test)
set_tests_properties(data.terrain-glow-test PROPERTIES
    LABELS "data;gate"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)
# M209: FA's campaign, its first operation
add_test(NAME data.campaign-test
         COMMAND osc_integration --map /maps/X1CA_001/X1CA_001_scenario.lua --campaign-test)
set_tests_properties(data.campaign-test PROPERTIES
    LABELS "data;gate"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)
foreach(mode IN LISTS OSC_DATA_TESTS_RETAIL_GAP)
    osc_add_data_test(${mode} retail-gap)
endforeach()
foreach(mode IN LISTS OSC_DATA_TESTS_NO_MAP_GATE)
    osc_add_data_test(${mode} gate NO_MAP)
endforeach()
foreach(mode IN LISTS OSC_DATA_TESTS_NO_MAP_RETAIL_GAP)
    osc_add_data_test(${mode} retail-gap NO_MAP)
endforeach()

# M196: the same game, played twice as two processes (every address
# different), stays identical tick for tick -- nothing depends on hash
# order, a shared or unseeded random stream, or object addresses.
if(Python3_Interpreter_FOUND)
    add_test(NAME data.determinism
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/determinism.py
                     $<TARGET_FILE:opensupcom> ${CMAKE_SOURCE_DIR}/tools/checksum_diff.py
                     -- --map ${OSC_TEST_MAP} --ai-skirmish --ai-armies 4 --ticks 1500
                     --seed 4242)
    set_tests_properties(data.determinism PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)

    # M199: a recorded game -- AI armies, and a player's orders and unit
    # settings -- plays back identically from the replay file alone.
    add_test(NAME data.replay_roundtrip
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/replay_roundtrip.py
                     $<TARGET_FILE:opensupcom> ${CMAKE_SOURCE_DIR}/tools/checksum_diff.py
                     -- --map ${OSC_TEST_MAP} --ai-skirmish --ai-armies 4 --ticks 1200
                     --seed 4242 --scripted-orders)
    set_tests_properties(data.replay_roundtrip PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)

    # M208a: a saved game loads and plays on as the game did, and a game
    # saved after it was loaded loads too -- each save holding an order the
    # player gave just before saving.
    add_test(NAME data.save_load
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/save_load.py
                     $<TARGET_FILE:opensupcom> ${CMAKE_SOURCE_DIR}/tools/checksum_diff.py
                     -- --map ${OSC_TEST_MAP} --ai-skirmish --ai-armies 4 --ticks 1200
                     --seed 4242 --scripted-orders)
    set_tests_properties(data.save_load PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 1200)
    # The same, each load catching up from the save's history (M208a): the
    # path a save without a snapshot takes, and the oracle a restore
    # matches.
    add_test(NAME data.save_load_replay
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/save_load.py --by-replay
                     $<TARGET_FILE:opensupcom> ${CMAKE_SOURCE_DIR}/tools/checksum_diff.py
                     -- --map ${OSC_TEST_MAP} --ai-skirmish --ai-armies 4 --ticks 1200
                     --seed 4242 --scripted-orders)
    set_tests_properties(data.save_load_replay PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 1200)
    # A campaign operation's (M209): blips, the playable area, its platoons
    # and objectives, restored with the rest.
    add_test(NAME data.save_load_campaign
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/save_load.py
                     $<TARGET_FILE:opensupcom> ${CMAKE_SOURCE_DIR}/tools/checksum_diff.py
                     -- --map /maps/X1CA_001/X1CA_001_scenario.lua --ticks 1200)
    set_tests_properties(data.save_load_campaign PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 1200)

    # M199b: retail's replay dialog path -- GetSpecialFiles lists the
    # recording, LaunchReplaySession opens it -- and the game plays it to
    # its end, offscreen, matching the recording at every tick.
    add_test(NAME data.replay_flow
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/replay_flow.py
                     $<TARGET_FILE:opensupcom>
                     -- --map ${OSC_TEST_MAP} --ai-skirmish --ai-armies 2 --ticks 300
                     --seed 9 --scripted-orders)
    set_tests_properties(data.replay_flow PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)

    # M208a, M191 step 4: retail's Load dialog path -- GetSpecialFiles lists
    # the save, LoadSavedGame loads it -- then the game catches up (never a
    # replay meanwhile), plays on, and InternalSaveGame saves it again. Then
    # that save loads from inside the game, the game returns to the lobby,
    # and the first loads again: each in a fresh UI state, offscreen.
    add_test(NAME data.load_flow
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/load_flow.py
                     $<TARGET_FILE:opensupcom>
                     -- --map ${OSC_TEST_MAP} --ai-skirmish --ai-armies 2 --ticks 250
                     --seed 9 --scripted-orders)
    set_tests_properties(data.load_flow PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)

    # The campaign through retail's own screens (M209b): the main menu, the
    # timeline movie skipped, operation select, the briefing, X1CA_001 to its
    # end, the score screen, and the next operation's briefing
    add_test(NAME data.campaign_flow COMMAND opensupcom --campaign-flow-test)
    set_tests_properties(data.campaign_flow PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)

    # M221b: a skirmish with the player's mods -- two test mods in a
    # temporary Documents folder's mods folder and retail's Resource Rich,
    # selected in its preferences -- launched from the front end as retail's
    # lobby launches one. The sim and the game's UI must have their mods
    # (hooks, blueprints, ModBlueprints), and its recording replay with them.
    add_test(NAME data.mods_flow
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mods_flow.py
                     $<TARGET_FILE:opensupcom>)
    set_tests_properties(data.mods_flow PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)

    # M218c: retail's LAN lobby (lobby.lua) from hosting to a game, by two
    # processes: one hosts, one joins, both ready, the host presses Launch,
    # and the game each launches plays in lockstep over the lobby's
    # connections, offscreen, each as its own slot's army. (Its ports are
    # below the OSes' ephemeral ranges, where another test's socket could
    # hold them.)
    add_test(NAME data.lan_game
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lan_game.py
                     $<TARGET_FILE:opensupcom> 29741)
    set_tests_properties(data.lan_game PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)
    # M218e: the joiner leaves at tick 120; the host drops it by the
    # lockstep's agreement, defeats its army, shows and closes retail's
    # disconnect dialog, and plays on alone.
    add_test(NAME data.lan_game_quit
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lan_game.py
                     $<TARGET_FILE:opensupcom> 29742 -- --lan-game-quit-at 120)
    set_tests_properties(data.lan_game_quit PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)
    # M220a: a matchmaking client (this script, standing in for FAF's)
    # launches the game over GPGNet: Idle, CreateLobby to a lobby, HostGame
    # hosting it on the next port, and the game ending with the link.
    add_test(NAME data.gpgnet_lobby
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gpgnet_lobby.py
                     $<TARGET_FILE:opensupcom> 29751)
    set_tests_properties(data.gpgnet_lobby PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 300)
    # M220b: the client drives two games through retail's auto-lobby, as
    # FAF's matchmaker does (uids, /players, /team, a faction), to a launch;
    # both play to tick 100 in lockstep. Ports 29753-29756.
    add_test(NAME data.gpgnet_game
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gpgnet_game.py
                     $<TARGET_FILE:opensupcom> 29753)
    set_tests_properties(data.gpgnet_game PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 600)
    # M220c: the same, each game reaching the other as through FAF's ICE
    # adapter: a UDP relay standing for each player, losing 5% of what it
    # carries. Ports 29771-29776.
    add_test(NAME data.gpgnet_game_relayed
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gpgnet_game.py
                     $<TARGET_FILE:opensupcom> 29771 --relay-loss 0.05)
    set_tests_properties(data.gpgnet_game_relayed PROPERTIES
        LABELS "data;gate"
        SKIP_RETURN_CODE 77
        TIMEOUT 600)

    # M223: the sim benchmark. A pinned four-AI game on Seton's Clutch, timed
    # tick by tick (--bench) and held to this machine's baseline in
    # the goldens' folder, bench/ (recorded on the first run): sim time, the mean
    # and p99 tick and peak memory within 15%, the same game (checksum).
    # Label `bench` alone (not `data`): timing wants a quiet machine, so they
    # run one at a time, and a Release build (`ctest -L bench`).
    foreach(_scenario early late)
        add_test(NAME bench.${_scenario}
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bench.py
                         check $<TARGET_FILE:opensupcom> --scenario ${_scenario})
        set_tests_properties(bench.${_scenario} PROPERTIES
            LABELS "bench"
            SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE
            TIMEOUT 3600)
    endforeach()
endif()

# ── Golden images (need Forged Alliance + a Vulkan display) ────────────────
# The golden PNGs contain game art, so they live outside the repository, in
# $OSC_GOLDEN_DIR (else the user state dir). A missing golden is "skipped";
# record one with: opensupcom <same args> --golden <name> --golden-update
# Frame 600 is after retail's loading fade: the capture holds the 3D scene
# and FA's game interface (economy, score, avatars, unit view). The camera is
# retail's own opening zoom to the commander. Run-to-run noise is ~0.015% of
# pixels; 0.1% still catches a UI text change (a word-spacing fix: 0.12%).
add_test(NAME data.golden.scmp009_game_ui
         COMMAND opensupcom --map ${OSC_TEST_MAP} --golden scmp009_game_ui
                 --screenshot-frame 600 --camera 672,346,60 --golden-tolerance 0.001)
set_tests_properties(data.golden.scmp009_game_ui PROPERTIES
    LABELS "data;gate;golden"
    SKIP_RETURN_CODE 77
    TIMEOUT 300)
# The same scene for a player whose profile shows the minimap window
# (retail's `stratview`): preferences load from Game.prefs, retail opens the
# window, and the engine draws the map into it.
add_test(NAME data.golden.scmp009_game_ui_minimap
         COMMAND opensupcom --map ${OSC_TEST_MAP} --golden scmp009_game_ui_minimap
                 --prefs ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/minimap.prefs
                 --screenshot-frame 600 --camera 672,346,60 --golden-tolerance 0.001)
set_tests_properties(data.golden.scmp009_game_ui_minimap PROPERTIES
    LABELS "data;gate;golden"
    SKIP_RETURN_CODE 77
    TIMEOUT 300)
# Retail's skirmish lobby, opened from the main menu (a capture's profile has
# already seen the tutorial prompt)
add_test(NAME data.golden.skirmish_lobby
         COMMAND opensupcom --click Skirmish --golden skirmish_lobby
                 --screenshot-frame 600 --golden-tolerance 0.001)
set_tests_properties(data.golden.skirmish_lobby PROPERTIES
    LABELS "data;gate;golden"
    SKIP_RETURN_CODE 77
    TIMEOUT 300)

# The lobby's Game Options dialog: its maps and map info are ItemLists, rows
# spaced as retail's
add_test(NAME data.golden.map_select
         COMMAND opensupcom --click Skirmish --click "Game Options" --golden map_select
                 --screenshot-frame 600 --golden-tolerance 0.001)
set_tests_properties(data.golden.map_select PROPERTIES
    LABELS "data;gate;golden"
    SKIP_RETURN_CODE 77
    TIMEOUT 300)

# The map list paged by a click on its scrollbar's track, below the thumb:
# a page, as far as its last rows
add_test(NAME data.golden.map_select_paged
         COMMAND opensupcom --click Skirmish --click "Game Options" --click-at 945,600
                 --golden map_select_paged --screenshot-frame 600 --golden-tolerance 0.001)
set_tests_properties(data.golden.map_select_paged PROPERTIES
    LABELS "data;gate;golden"
    SKIP_RETURN_CODE 77
    TIMEOUT 300)

# ── Engine API coverage ratchet (roadmap M184) ─────────────────────────────
# Fails if the game's scripts call an engine function/method that is neither
# bound nor listed in the baseline. Baseline is for retail FA 3599; a machine
# whose discovered install is FAForever will report FAF's own set instead.
add_test(NAME data.binding_coverage
         COMMAND opensupcom --map ${OSC_TEST_MAP}
                 --binding-coverage ${CMAKE_CURRENT_BINARY_DIR}/binding_coverage.txt
                 --binding-baseline ${CMAKE_CURRENT_SOURCE_DIR}/binding_baseline_retail.txt)
set_tests_properties(data.binding_coverage PROPERTIES
    LABELS "data;gate"
    SKIP_RETURN_CODE 77
    TIMEOUT 300)
