add_library(osc_ui STATIC
    ui_control.cpp
    ui_dispatch.cpp
)
add_library(osc::ui ALIAS osc_ui)

target_include_directories(osc_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_ui
    PUBLIC osc::core
    PRIVATE glfw lua50 spdlog::spdlog
)
