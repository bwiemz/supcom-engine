add_library(osc_ui STATIC
    ui_control.cpp
    ui_dispatch.cpp
    font_metrics_provider.cpp
    wld_ui_provider.cpp
)
add_library(osc::ui ALIAS osc_ui)

target_include_directories(osc_ui
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(osc_ui
    PUBLIC osc::core
    PRIVATE glfw lua50 spdlog::spdlog osc::vfs
)
