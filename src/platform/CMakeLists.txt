# OS abstraction layer: the only place allowed to include platform headers
# (<windows.h>, <unistd.h>, ...). Depends on nothing else in the engine.
add_library(osc_platform STATIC
    browser.cpp
    crash_handler.cpp
    game_install.cpp
    paths.cpp
    steam_library.cpp
)
add_library(osc::platform ALIAS osc_platform)

target_include_directories(osc_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# open_url reaps the URL handler it starts on a thread of its own.
find_package(Threads REQUIRED)
target_link_libraries(osc_platform PRIVATE osc::warnings Threads::Threads)

if(WIN32)
    target_link_libraries(osc_platform PRIVATE shell32 ole32 advapi32)
endif()
