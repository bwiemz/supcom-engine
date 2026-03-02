add_library(osc_renderer STATIC
    renderer.cpp
    terrain_mesh.cpp
    unit_renderer.cpp
    water_renderer.cpp
    camera.cpp
    shader_utils.cpp
    pipeline_builder.cpp
    vk_utils.cpp
)
add_library(osc::renderer ALIAS osc_renderer)

target_include_directories(osc_renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(osc_renderer
    PUBLIC osc::core osc::map osc::sim
           Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator
    PRIVATE glfw vk-bootstrap::vk-bootstrap unofficial::shaderc::shaderc
            spdlog::spdlog
)
