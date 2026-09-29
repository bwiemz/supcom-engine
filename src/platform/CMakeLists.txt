# OS abstraction layer: the only place allowed to include platform headers
# (<windows.h>, <unistd.h>, ...). Depends on nothing else in the engine.
add_library(osc_platform STATIC
    browser.cpp
    crash_handler.cpp
    engine_settings.cpp
    first_run.cpp
    game_install.cpp
    native_prompter.cpp
    paths.cpp
    steam_library.cpp
)
add_library(osc::platform ALIAS osc_platform)

target_include_directories(osc_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# open_url reaps the URL handler it starts on a thread of its own.
find_package(Threads REQUIRED)
target_link_libraries(osc_platform PRIVATE osc::warnings Threads::Threads
                                           nlohmann_json::nlohmann_json)

# The first run's native dialogs (M228a): portable-file-dialogs, one header
# (a system include: its warnings aren't the engine's).
find_path(OSC_PFD_INCLUDE_DIR portable-file-dialogs.h REQUIRED)
target_include_directories(osc_platform SYSTEM PRIVATE ${OSC_PFD_INCLUDE_DIR})

if(WIN32)
    target_link_libraries(osc_platform PRIVATE shell32 ole32 advapi32 comdlg32 user32)
endif()
