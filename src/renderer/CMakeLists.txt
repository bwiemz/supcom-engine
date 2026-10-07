add_library(osc_renderer STATIC
    renderer.cpp
    terrain_mesh.cpp
    unit_renderer.cpp
    water_renderer.cpp
    sky_dome.cpp
    sky_renderer.cpp
    mesh_cache.cpp
    texture_cache.cpp
    dds_parser.cpp
    dds_decode.cpp
    camera.cpp
    frustum.cpp
    shadow_camera.cpp
    shader_utils.cpp
    shield_shaders.cpp
    pipeline_builder.cpp
    vk_utils.cpp
    ui_renderer.cpp
    movie_textures.cpp
    font_cache.cpp
    input_handler.cpp
    overlay_renderer.cpp
    minimap_renderer.cpp
    terrain_preview.cpp
    strategic_icon_renderer.cpp
    hud_renderer.cpp
    selection_info_renderer.cpp
    fog_renderer.cpp
    playable_rect.cpp
    recon_view.cpp
    decal_math.cpp
    terrain_normal_maps.cpp
    runtime_decals.cpp
    runtime_decal_renderer.cpp
    beam_blueprint.cpp
    beam_renderer.cpp
    command_feedback.cpp
    command_graph_renderer.cpp
    world_quad_batch.cpp
    selection_renderer.cpp
    range_overlays.cpp
    range_renderer.cpp
    effect_blueprint_file.cpp
    trail_blueprint.cpp
    trail_renderer.cpp
    emitter_blueprint.cpp
    particle_system.cpp
    wave_system.cpp
    particle_renderer.cpp
    profile_overlay.cpp
    vk_cmd.cpp
    gpu_queries.cpp
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
            spdlog::spdlog osc::vfs osc::blueprints osc::platform
)

target_link_libraries(osc_renderer PRIVATE osc::warnings)

# Debug builds point the Vulkan loader at the vcpkg-built validation layer
# (manifest feature "vulkan-validation"), so validation works without a
# system-wide SDK. Absent the feature, a system-installed layer is used.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_osc_vvl_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/vulkan/explicit_layer.d")
    if(EXISTS "${_osc_vvl_dir}/VkLayer_khronos_validation.json")
        target_compile_definitions(osc_renderer PRIVATE
            OSC_VULKAN_LAYER_PATH="${_osc_vvl_dir}")
    endif()
endif()
