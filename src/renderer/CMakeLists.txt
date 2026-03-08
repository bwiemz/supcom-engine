add_library(osc_renderer STATIC
    renderer.cpp
    terrain_mesh.cpp
    unit_renderer.cpp
    water_renderer.cpp
    mesh_cache.cpp
    texture_cache.cpp
    dds_parser.cpp
    camera.cpp
    shader_utils.cpp
    pipeline_builder.cpp
    vk_utils.cpp
    ui_renderer.cpp
    font_cache.cpp
)
add_library(osc::renderer ALIAS osc_renderer)

target_include_directories(osc_renderer
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(osc_renderer
    PUBLIC osc::core osc::map osc::sim osc::ui
           Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator
    PRIVATE glfw vk-bootstrap::vk-bootstrap unofficial::shaderc::shaderc
            spdlog::spdlog osc::vfs osc::blueprints
)
