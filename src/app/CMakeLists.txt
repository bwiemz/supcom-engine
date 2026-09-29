# The engine as an application (M192): the boot, the front end, a game's
# session, the windowed loop and the headless run. The game (opensupcom)
# runs it as it is; the integration runner adds its test modes.
add_library(osc_app STATIC
    app.cpp
    boot.cpp
    cli.cpp
    frames.cpp
    headless.cpp
    lan_game_test.cpp
    log_bundle.cpp
    mods_flow_test.cpp
    reload.cpp
    session.cpp
    ui_globals.cpp
    window.cpp
    window_commands.cpp
    window_mode.cpp
)
add_library(osc::app ALIAS osc_app)

target_include_directories(osc_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_app
    PUBLIC osc::platform osc::core osc::vfs osc::sim osc::map osc::audio osc::video osc::ui
           osc::renderer osc::lua osc::lua_user osc::blueprints
    PRIVATE osc::warnings unofficial::minizip::minizip
)
