#include "renderer/shader_utils.hpp"

#include <shaderc/shaderc.hpp>
#include <spdlog/spdlog.h>

namespace osc::renderer {

VkShaderModule compile_glsl(VkDevice device, const char* source,
                            const char* name, bool is_vertex) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan,
                                shaderc_env_version_vulkan_1_0);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    auto kind = is_vertex ? shaderc_vertex_shader : shaderc_fragment_shader;
    auto result = compiler.CompileGlslToSpv(source, kind, name, options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        spdlog::error("Shader compile error ({}): {}", name,
                      result.GetErrorMessage());
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (result.end() - result.begin()) * sizeof(uint32_t);
    ci.pCode = result.begin();

    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS) {
        spdlog::error("Failed to create shader module: {}", name);
        return VK_NULL_HANDLE;
    }

    spdlog::debug("Compiled shader: {}", name);
    return mod;
}

// Embedded GLSL sources
namespace shaders {

const char* terrain_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out float fragHeight;

void main() {
    gl_Position = pc.viewProj * vec4(inPosition, 1.0);
    fragNormal = inNormal;
    fragHeight = inPosition.y;
}
)glsl";

const char* terrain_frag = R"glsl(
#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in float fragHeight;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 green = vec3(0.2, 0.5, 0.15);
    vec3 brown = vec3(0.45, 0.3, 0.15);
    vec3 white = vec3(0.9, 0.9, 0.85);

    float t = clamp(fragHeight / 40.0, 0.0, 1.0);
    vec3 baseColor;
    if (t < 0.5) {
        baseColor = mix(green, brown, t * 2.0);
    } else {
        baseColor = mix(brown, white, (t - 0.5) * 2.0);
    }

    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float NdotL = max(dot(normalize(fragNormal), lightDir), 0.0);
    float lighting = 0.3 + 0.7 * NdotL;

    outColor = vec4(baseColor * lighting, 1.0);
}
)glsl";

const char* unit_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 2) in vec3 inInstancePos;
layout(location = 3) in float inScale;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec4 fragColor;

void main() {
    vec3 worldPos = inPosition * inScale + inInstancePos;
    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
    fragNormal = inNormal;
    fragColor = inColor;
}
)glsl";

const char* unit_frag = R"glsl(
#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float NdotL = max(dot(normalize(fragNormal), lightDir), 0.0);
    float lighting = 0.4 + 0.6 * NdotL;

    outColor = vec4(fragColor.rgb * lighting, fragColor.a);
}
)glsl";

const char* water_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = pc.viewProj * vec4(inPosition, 1.0);
}
)glsl";

const char* water_frag = R"glsl(
#version 450

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.1, 0.3, 0.6, 0.5);
}
)glsl";

const char* mesh_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    uint boneBase;
    uint bonesPerInst;
} pc;

// Per-vertex (binding 0): position + normal + UV + bone_index
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 8) in int inBoneIndex;

// Per-instance (binding 1) — mat4 uses locations 3-6 (4 vec4 columns)
layout(location = 3) in mat4 inModel;
layout(location = 7) in vec4 inColor;

// Bone SSBO (set=1, binding=0)
layout(std430, set = 1, binding = 0) readonly buffer BoneBuffer {
    mat4 bones[];
} boneSSBO;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec2 fragUV;

void main() {
    // Apply skeletal skinning: bone transform before model matrix
    uint boneIdx = pc.boneBase + uint(gl_InstanceIndex) * pc.bonesPerInst
                   + uint(inBoneIndex);
    mat4 bone = boneSSBO.bones[boneIdx];
    vec4 skinnedPos = bone * vec4(inPosition, 1.0);
    vec4 worldPos = inModel * skinnedPos;
    gl_Position = pc.viewProj * worldPos;
    // Transform normal through bone then model
    fragNormal = mat3(inModel) * (mat3(bone) * inNormal);
    fragColor = inColor;
    fragUV = inUV;
}
)glsl";

// Mesh fragment shader — samples albedo texture, multiplies with army color
const char* mesh_frag = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform sampler2D texAlbedo;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float NdotL = max(dot(normalize(fragNormal), lightDir), 0.0);
    float lighting = 0.4 + 0.6 * NdotL;

    vec4 texColor = texture(texAlbedo, fragUV);
    outColor = vec4(texColor.rgb * fragColor.rgb * lighting, texColor.a * fragColor.a);
}
)glsl";

} // namespace shaders

} // namespace osc::renderer
