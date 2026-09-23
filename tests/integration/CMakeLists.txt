# Integration tests that drive the real `opensupcom` executable.
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
    # Distinct ports so the tests can run in parallel.
    add_test(NAME mp.lockstep_sync
             COMMAND ${Python3_EXECUTABLE} ${_run_pair} $<TARGET_FILE:opensupcom> mp 47711)
    add_test(NAME mp.lockstep_desync_detected
             COMMAND ${Python3_EXECUTABLE} ${_run_pair} $<TARGET_FILE:opensupcom> mp 47712
                     -- --mp-desync)
    add_test(NAME mp.lan_lobby_launch
             COMMAND ${Python3_EXECUTABLE} ${_run_pair} $<TARGET_FILE:opensupcom> lan 47713)
    add_test(NAME mp.lan_peer_drop
             COMMAND ${Python3_EXECUTABLE} ${_run_pair} $<TARGET_FILE:opensupcom> lan 47714
                     --client-may-fail -- --mp-drop-at 20)
    set_tests_properties(mp.lockstep_sync mp.lockstep_desync_detected
                         mp.lan_lobby_launch mp.lan_peer_drop
                         PROPERTIES LABELS mp TIMEOUT 240)
else()
    message(STATUS "Python3 not found: two-process multiplayer tests not registered")
endif()

add_test(NAME mp.lan_ui_globals COMMAND opensupcom --lan-ui-test)
set_tests_properties(mp.lan_ui_globals PROPERTIES LABELS mp TIMEOUT 60)

# ── Data-backed modes (need Forged Alliance) ───────────────────────────────
set(OSC_TEST_MAP "/maps/SCMP_009/SCMP_009_scenario.lua"
    CACHE STRING "Scenario used by data-backed integration tests")

include(${CMAKE_CURRENT_SOURCE_DIR}/data_tests.cmake)

function(osc_add_data_test mode group)
    if(ARGN STREQUAL "NO_MAP")
        add_test(NAME data.${mode} COMMAND opensupcom --${mode})
    else()
        add_test(NAME data.${mode}
                 COMMAND opensupcom --map ${OSC_TEST_MAP} --${mode})
    endif()
    set_tests_properties(data.${mode} PROPERTIES
        LABELS "data;${group}"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)
endfunction()

foreach(mode IN LISTS OSC_DATA_TESTS_GATE)
    osc_add_data_test(${mode} gate)
endforeach()
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
