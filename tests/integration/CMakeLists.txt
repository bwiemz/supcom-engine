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
foreach(mode IN LISTS OSC_DATA_TESTS_NO_MAP_RETAIL_GAP)
    osc_add_data_test(${mode} retail-gap NO_MAP)
endforeach()

# ── Golden images (need Forged Alliance + a Vulkan display) ────────────────
# The golden PNGs contain game art, so they live outside the repository, in
# $OSC_GOLDEN_DIR (else the user state dir). A missing golden is "skipped";
# record one with: opensupcom <same args> --golden <name> --golden-update
add_test(NAME data.golden.scmp009_acu
         COMMAND opensupcom --map ${OSC_TEST_MAP} --golden scmp009_acu
                 --screenshot-frame 120 --camera 672,346,60)
set_tests_properties(data.golden.scmp009_acu PROPERTIES
    LABELS "data;gate;golden"
    SKIP_RETURN_CODE 77
    TIMEOUT 300)
