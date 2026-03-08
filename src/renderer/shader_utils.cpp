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
layout(location = 1) out vec2 fragWorldXZ;
layout(location = 2) out float fragWorldY;

void main() {
    gl_Position = pc.viewProj * vec4(inPosition, 1.0);
    fragNormal = inNormal;
    fragWorldXZ = inPosition.xz;
    fragWorldY = inPosition.y;
}
)glsl";

const char* terrain_frag = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    float mapWidth, mapHeight;
    float scales[9];
    float eyeX, eyeY, eyeZ;
} pc;

// Blend maps
layout(set = 0, binding = 0) uniform sampler2D blendMap0;
layout(set = 0, binding = 1) uniform sampler2D blendMap1;

// Stratum albedo (bindings 2-10)
layout(set = 0, binding = 2) uniform sampler2D stratum0;
layout(set = 0, binding = 3) uniform sampler2D stratum1;
layout(set = 0, binding = 4) uniform sampler2D stratum2;
layout(set = 0, binding = 5) uniform sampler2D stratum3;
layout(set = 0, binding = 6) uniform sampler2D stratum4;
layout(set = 0, binding = 7) uniform sampler2D stratum5;
layout(set = 0, binding = 8) uniform sampler2D stratum6;
layout(set = 0, binding = 9) uniform sampler2D stratum7;
layout(set = 0, binding = 10) uniform sampler2D stratum8;

// Stratum normal maps (bindings 11-19)
layout(set = 0, binding = 11) uniform sampler2D normalMap0;
layout(set = 0, binding = 12) uniform sampler2D normalMap1;
layout(set = 0, binding = 13) uniform sampler2D normalMap2;
layout(set = 0, binding = 14) uniform sampler2D normalMap3;
layout(set = 0, binding = 15) uniform sampler2D normalMap4;
layout(set = 0, binding = 16) uniform sampler2D normalMap5;
layout(set = 0, binding = 17) uniform sampler2D normalMap6;
layout(set = 0, binding = 18) uniform sampler2D normalMap7;
layout(set = 0, binding = 19) uniform sampler2D normalMap8;

// Fog of war (binding 20)
layout(set = 0, binding = 20) uniform sampler2D fogMap;

// Shadow map (set=1)
layout(set = 1, binding = 0) uniform sampler2DShadow shadowMap;
layout(set = 1, binding = 1) uniform LightUBO { mat4 lightViewProj; } lightUbo;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragWorldXZ;
layout(location = 2) in float fragWorldY;

layout(location = 0) out vec4 outColor;

float calcShadow(vec3 worldPos) {
    vec4 lc = lightUbo.lightViewProj * vec4(worldPos, 1.0);
    vec3 pc2 = lc.xyz / lc.w;
    vec2 uv = pc2.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;
    // 4x4 PCF soft shadows
    float shadow = 0.0;
    float ts = 1.0 / 4096.0;
    for (int x = -2; x <= 1; x++) {
        for (int y = -2; y <= 1; y++) {
            vec2 off = vec2(float(x) + 0.5, float(y) + 0.5) * ts;
            shadow += texture(shadowMap, vec3(uv + off, pc2.z));
        }
    }
    shadow /= 16.0;
    // Smooth fade at shadow map frustum edges
    float fadeRange = 0.05;
    float edgeFade = smoothstep(0.0, fadeRange, uv.x)
                   * smoothstep(0.0, fadeRange, uv.y)
                   * smoothstep(0.0, fadeRange, 1.0 - uv.x)
                   * smoothstep(0.0, fadeRange, 1.0 - uv.y);
    return mix(1.0, shadow, edgeFade);
}

// Decode FA DXT5nm normal map: X=Green, Y=Alpha, Z=derived
vec3 decodeNormal(sampler2D nmap, vec2 uv) {
    vec4 s = texture(nmap, uv);
    vec3 n;
    n.x = s.g * 2.0 - 1.0;
    n.y = s.a * 2.0 - 1.0;
    n.z = sqrt(max(0.0, 1.0 - n.x*n.x - n.y*n.y));
    return n;
}

void main() {
    // Blend map UV: world position normalized to [0,1] over map extents
    vec2 blendUV = fragWorldXZ / vec2(pc.mapWidth, pc.mapHeight);

    // Sample blend weights (RGBA = 4 strata weights each)
    vec4 b0 = texture(blendMap0, blendUV);
    vec4 b1 = texture(blendMap1, blendUV);

    // Per-stratum UV (reuse albedo scale for normal maps)
    vec2 uv0 = fragWorldXZ / max(pc.scales[0], 1.0);
    vec2 uv1 = fragWorldXZ / max(pc.scales[1], 1.0);
    vec2 uv2 = fragWorldXZ / max(pc.scales[2], 1.0);
    vec2 uv3 = fragWorldXZ / max(pc.scales[3], 1.0);
    vec2 uv4 = fragWorldXZ / max(pc.scales[4], 1.0);
    vec2 uv5 = fragWorldXZ / max(pc.scales[5], 1.0);
    vec2 uv6 = fragWorldXZ / max(pc.scales[6], 1.0);
    vec2 uv7 = fragWorldXZ / max(pc.scales[7], 1.0);
    vec2 uv8 = fragWorldXZ / max(pc.scales[8], 1.0);

    // Sample each stratum albedo
    vec3 s0 = texture(stratum0, uv0).rgb;
    vec3 s1 = texture(stratum1, uv1).rgb;
    vec3 s2 = texture(stratum2, uv2).rgb;
    vec3 s3 = texture(stratum3, uv3).rgb;
    vec3 s4 = texture(stratum4, uv4).rgb;
    vec3 s5 = texture(stratum5, uv5).rgb;
    vec3 s6 = texture(stratum6, uv6).rgb;
    vec3 s7 = texture(stratum7, uv7).rgb;
    vec3 s8 = texture(stratum8, uv8).rgb;

    // Sequential alpha compositing: each stratum replaces a portion
    // of the layer below (FA blending — NOT additive)
    vec3 color = s0;
    color = mix(color, s1, b0.r);
    color = mix(color, s2, b0.g);
    color = mix(color, s3, b0.b);
    color = mix(color, s4, b0.a);
    color = mix(color, s5, b1.r);
    color = mix(color, s6, b1.g);
    color = mix(color, s7, b1.b);
    color = mix(color, s8, b1.a);

    // Decode and blend per-stratum normal maps (same UV as albedo)
    vec3 n0 = decodeNormal(normalMap0, uv0);
    vec3 n1 = decodeNormal(normalMap1, uv1);
    vec3 n2 = decodeNormal(normalMap2, uv2);
    vec3 n3 = decodeNormal(normalMap3, uv3);
    vec3 n4 = decodeNormal(normalMap4, uv4);
    vec3 n5 = decodeNormal(normalMap5, uv5);
    vec3 n6 = decodeNormal(normalMap6, uv6);
    vec3 n7 = decodeNormal(normalMap7, uv7);
    vec3 n8 = decodeNormal(normalMap8, uv8);

    // Sequential alpha compositing for normals (matching albedo blend)
    vec3 blendedTangentNormal = n0;
    blendedTangentNormal = mix(blendedTangentNormal, n1, b0.r);
    blendedTangentNormal = mix(blendedTangentNormal, n2, b0.g);
    blendedTangentNormal = mix(blendedTangentNormal, n3, b0.b);
    blendedTangentNormal = mix(blendedTangentNormal, n4, b0.a);
    blendedTangentNormal = mix(blendedTangentNormal, n5, b1.r);
    blendedTangentNormal = mix(blendedTangentNormal, n6, b1.g);
    blendedTangentNormal = mix(blendedTangentNormal, n7, b1.b);
    blendedTangentNormal = mix(blendedTangentNormal, n8, b1.a);
    blendedTangentNormal = normalize(blendedTangentNormal);

    // TBN: terrain UV is world-XZ-aligned, so T=(1,0,0), B=(0,0,1)
    // Gram-Schmidt orthogonalize against vertex normal for slopes
    // Falls back to Z-axis when N is nearly parallel to X (steep cliff faces)
    vec3 N = normalize(fragNormal);
    float d = dot(N, vec3(1.0, 0.0, 0.0));
    vec3 T;
    if (abs(d) > 0.999) {
        T = normalize(vec3(0.0, 0.0, 1.0) - N * dot(N, vec3(0.0, 0.0, 1.0)));
    } else {
        T = normalize(vec3(1.0, 0.0, 0.0) - N * d);
    }
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    vec3 worldNormal = normalize(TBN * blendedTangentNormal);

    // Lighting with perturbed normal + shadow
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float NdotL = max(dot(worldNormal, lightDir), 0.0);
    vec3 worldPos = vec3(fragWorldXZ.x, fragWorldY, fragWorldXZ.y);
    float shadow = calcShadow(worldPos);

    // Decode sRGB texture values to linear for correct lighting math
    // (sRGB swapchain will re-encode on output)
    color = pow(color, vec3(2.2));

    // Hemisphere ambient: warm sunlit sky from above, cool shadow from below
    vec3 skyColor = vec3(0.55, 0.52, 0.48);
    vec3 groundColor = vec3(0.25, 0.24, 0.22);
    float hemi = worldNormal.y * 0.5 + 0.5; // remap [-1,1] to [0,1]
    vec3 ambient = mix(groundColor, skyColor, hemi) * 0.45;

    // Diffuse
    vec3 diffuse = vec3(0.65) * NdotL * shadow;

    // Blinn-Phong specular on terrain (subtle)
    vec3 viewDir = normalize(vec3(pc.eyeX, pc.eyeY, pc.eyeZ) - worldPos);
    vec3 halfDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(worldNormal, halfDir), 0.0);
    float spec = pow(NdotH, 24.0) * 0.15 * shadow;

    vec3 lit = color * (ambient + diffuse) + vec3(spec);

    // Fog of war: CPU-blurred texture, smooth transitions
    // FA shows unexplored at ~45% brightness with mild desaturation
    float fogVal = texture(fogMap, blendUV).r;
    float fogBright = mix(0.45, 1.0, fogVal);
    // Mild desaturation in unexplored areas
    float fogSat = mix(0.65, 1.0, fogVal);
    vec3 gray = vec3(dot(lit, vec3(0.299, 0.587, 0.114)));
    lit = mix(gray, lit, fogSat);
    lit *= fogBright;

    // Atmospheric distance fog: fade to haze at distance
    float dist = length(vec3(pc.eyeX, pc.eyeY, pc.eyeZ) - worldPos);
    float fogDensity = 0.0005;
    float atmosFog = 1.0 - exp(-dist * fogDensity);
    vec3 hazeColor = vec3(0.55, 0.62, 0.72) * fogBright;
    lit = mix(lit, hazeColor, atmosFog);

    outColor = vec4(lit, 1.0);
}
)glsl";

const char* unit_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    float eyeX, eyeY, eyeZ;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 2) in vec3 inInstancePos;
layout(location = 3) in float inScale;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec3 fragWorldPos;

void main() {
    vec3 worldPos = inPosition * inScale + inInstancePos;
    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
    fragNormal = inNormal;
    fragColor = inColor;
    fragWorldPos = worldPos;
}
)glsl";

const char* unit_frag = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    float eyeX, eyeY, eyeZ;
} upc;

// Shadow map (set=0)
layout(set = 0, binding = 0) uniform sampler2DShadow shadowMap;
layout(set = 0, binding = 1) uniform LightUBO { mat4 lightViewProj; } lightUbo;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec3 fragWorldPos;

layout(location = 0) out vec4 outColor;

float calcShadow(vec3 worldPos) {
    vec4 lc = lightUbo.lightViewProj * vec4(worldPos, 1.0);
    vec3 pc = lc.xyz / lc.w;
    vec2 uv = pc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;
    // 4x4 PCF soft shadows
    float shadow = 0.0;
    float ts = 1.0 / 4096.0;
    for (int x = -2; x <= 1; x++) {
        for (int y = -2; y <= 1; y++) {
            vec2 off = vec2(float(x) + 0.5, float(y) + 0.5) * ts;
            shadow += texture(shadowMap, vec3(uv + off, pc.z));
        }
    }
    shadow /= 16.0;
    // Smooth fade at shadow map frustum edges
    float fadeRange = 0.05;
    float edgeFade = smoothstep(0.0, fadeRange, uv.x)
                   * smoothstep(0.0, fadeRange, uv.y)
                   * smoothstep(0.0, fadeRange, 1.0 - uv.x)
                   * smoothstep(0.0, fadeRange, 1.0 - uv.y);
    return mix(1.0, shadow, edgeFade);
}

void main() {
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float NdotL = max(dot(normalize(fragNormal), lightDir), 0.0);
    float shadow = calcShadow(fragWorldPos);
    float lighting = 0.4 + 0.6 * NdotL * shadow;

    vec3 lit = fragColor.rgb * lighting;

    // Atmospheric distance fog
    float dist = length(vec3(upc.eyeX, upc.eyeY, upc.eyeZ) - fragWorldPos);
    float fogDensity = 0.0005;
    float atmosFog = 1.0 - exp(-dist * fogDensity);
    vec3 hazeColor = vec3(0.55, 0.62, 0.72);
    lit = mix(lit, hazeColor, atmosFog);

    outColor = vec4(lit, fragColor.a);
}
)glsl";

const char* water_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    float time;
    float eyeX, eyeY, eyeZ;
    float waterElev;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in float inDepth;

layout(location = 0) out float fragDepth;
layout(location = 1) out vec3 fragWorldPos;
layout(location = 2) out vec3 fragNormal;

void main() {
    vec3 pos = inPosition;
    fragDepth = inDepth;

    // Wave displacement (3 overlapping sine waves)
    float t = pc.time;
    float wave1 = sin(pos.x * 0.08 + t * 1.2) * cos(pos.z * 0.06 + t * 0.9) * 0.15;
    float wave2 = sin(pos.x * 0.15 + pos.z * 0.12 + t * 1.8) * 0.08;
    float wave3 = sin(pos.z * 0.20 - t * 0.7) * cos(pos.x * 0.10 + t * 1.1) * 0.06;
    float wave = wave1 + wave2 + wave3;

    // Reduce waves near shore (shallow water)
    float shoreAtten = clamp(inDepth * 0.5, 0.0, 1.0);
    pos.y += wave * shoreAtten;

    // Approximate normal from wave derivatives
    float dx1 = cos(pos.x * 0.08 + t * 1.2) * 0.08 * cos(pos.z * 0.06 + t * 0.9) * 0.15;
    float dx2 = cos(pos.x * 0.15 + pos.z * 0.12 + t * 1.8) * 0.15 * 0.08;
    float dz1 = -sin(pos.x * 0.08 + t * 1.2) * sin(pos.z * 0.06 + t * 0.9) * 0.06 * 0.15;
    float dz3 = cos(pos.z * 0.20 - t * 0.7) * 0.20 * cos(pos.x * 0.10 + t * 1.1) * 0.06;
    float dydx = (dx1 + dx2) * shoreAtten;
    float dydz = (dz1 + dz3) * shoreAtten;
    fragNormal = normalize(vec3(-dydx, 1.0, -dydz));

    fragWorldPos = pos;
    gl_Position = pc.viewProj * vec4(pos, 1.0);
}
)glsl";

const char* water_frag = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    float time;
    float eyeX, eyeY, eyeZ;
    float waterElev;
} pc;

layout(location = 0) in float fragDepth;
layout(location = 1) in vec3 fragWorldPos;
layout(location = 2) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

void main() {
    // Depth-based color: shallow = light teal, deep = dark blue
    float d = clamp(fragDepth / 12.0, 0.0, 1.0);
    vec3 shallowColor = vec3(0.15, 0.45, 0.55);
    vec3 deepColor    = vec3(0.03, 0.10, 0.30);
    vec3 waterColor   = mix(shallowColor, deepColor, d);

    // Sun direction (same as terrain/mesh shaders)
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 N = normalize(fragNormal);

    // Diffuse shading on water surface
    float NdotL = max(dot(N, lightDir), 0.0);
    waterColor *= 0.7 + 0.3 * NdotL;

    // Fresnel-based specular (sun glint)
    vec3 eyePos = vec3(pc.eyeX, pc.eyeY, pc.eyeZ);
    vec3 viewDir = normalize(eyePos - fragWorldPos);
    vec3 halfDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(N, halfDir), 0.0);
    float spec = pow(NdotH, 64.0) * 0.8;

    // Fresnel: more reflection at grazing angles
    float fresnel = pow(1.0 - max(dot(viewDir, N), 0.0), 3.0) * 0.4;

    waterColor += vec3(spec + fresnel);

    // Shore foam: white band where depth is very shallow
    float foam = smoothstep(0.8, 0.0, fragDepth) * 0.4;
    // Animated foam sparkle
    float sparkle = sin(fragWorldPos.x * 2.0 + pc.time * 3.0)
                  * cos(fragWorldPos.z * 2.5 + pc.time * 2.0);
    foam *= 0.7 + 0.3 * sparkle;
    waterColor += vec3(foam);

    // Alpha: more opaque in deep water, semi-transparent at shore
    float alpha = mix(0.45, 0.85, d);

    // Atmospheric distance fog
    float dist = length(vec3(pc.eyeX, pc.eyeY, pc.eyeZ) - fragWorldPos);
    float fogDensity = 0.0005;
    float atmosFog = 1.0 - exp(-dist * fogDensity);
    vec3 hazeColor = vec3(0.55, 0.62, 0.72);
    waterColor = mix(clamp(waterColor, 0.0, 1.0), hazeColor, atmosFog);
    alpha = mix(alpha, 1.0, atmosFog); // fog is opaque at distance

    outColor = vec4(waterColor, alpha);
}
)glsl";

const char* mesh_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    uint boneBase;
    uint bonesPerInst;
    float eyeX, eyeY, eyeZ;
} pc;

// Per-vertex (binding 0): position + normal + UV + bone_indices + bone_weights + tangent
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 8) in uvec4 inBoneIndices;
layout(location = 9) in vec4 inBoneWeights;
layout(location = 10) in vec3 inTangent;

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
layout(location = 3) out vec3 fragTangent;
layout(location = 4) out vec3 fragBitangent;
layout(location = 5) out vec3 fragWorldPos;

void main() {
    // Blend-weight skeletal skinning: up to 4 bone influences per vertex
    uint base = pc.boneBase + uint(gl_InstanceIndex) * pc.bonesPerInst;
    mat4 bone = inBoneWeights[0] * boneSSBO.bones[base + inBoneIndices[0]]
              + inBoneWeights[1] * boneSSBO.bones[base + inBoneIndices[1]]
              + inBoneWeights[2] * boneSSBO.bones[base + inBoneIndices[2]]
              + inBoneWeights[3] * boneSSBO.bones[base + inBoneIndices[3]];
    vec4 skinnedPos = bone * vec4(inPosition, 1.0);
    vec4 worldPos = inModel * skinnedPos;
    gl_Position = pc.viewProj * worldPos;
    fragWorldPos = worldPos.xyz;
    // Transform TBN vectors through blended bone then model
    mat3 normalMat = mat3(inModel) * mat3(bone);
    fragNormal = normalMat * inNormal;
    fragTangent = normalMat * inTangent;
    fragBitangent = cross(fragNormal, fragTangent);
    fragColor = inColor;
    fragUV = inUV;
}
)glsl";

// Mesh fragment shader — normal mapping + Blinn-Phong specular + SpecTeam team color
const char* mesh_frag = R"glsl(
#version 450

// Full block declared for layout compatibility; only eyeX/Y/Z read in this stage
layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    uint boneBase;
    uint bonesPerInst;
    float eyeX, eyeY, eyeZ;
} pc;

layout(set = 0, binding = 0) uniform sampler2D texAlbedo;
layout(set = 2, binding = 0) uniform sampler2D texSpecTeam;
layout(set = 3, binding = 0) uniform sampler2D texNormal;

// Shadow map (set=4)
layout(set = 4, binding = 0) uniform sampler2DShadow shadowMap;
layout(set = 4, binding = 1) uniform LightUBO { mat4 lightViewProj; } lightUbo;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragColor;  // army color (RGB) + build alpha (A)
layout(location = 2) in vec2 fragUV;
layout(location = 3) in vec3 fragTangent;
layout(location = 4) in vec3 fragBitangent;
layout(location = 5) in vec3 fragWorldPos;

layout(location = 0) out vec4 outColor;

float calcShadow(vec3 worldPos) {
    vec4 lc = lightUbo.lightViewProj * vec4(worldPos, 1.0);
    vec3 pc2 = lc.xyz / lc.w;
    vec2 uv = pc2.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;
    // 4x4 PCF soft shadows
    float shadow = 0.0;
    float ts = 1.0 / 4096.0;
    for (int x = -2; x <= 1; x++) {
        for (int y = -2; y <= 1; y++) {
            vec2 off = vec2(float(x) + 0.5, float(y) + 0.5) * ts;
            shadow += texture(shadowMap, vec3(uv + off, pc2.z));
        }
    }
    shadow /= 16.0;
    // Smooth fade at shadow map frustum edges
    float fadeRange = 0.05;
    float edgeFade = smoothstep(0.0, fadeRange, uv.x)
                   * smoothstep(0.0, fadeRange, uv.y)
                   * smoothstep(0.0, fadeRange, 1.0 - uv.x)
                   * smoothstep(0.0, fadeRange, 1.0 - uv.y);
    return mix(1.0, shadow, edgeFade);
}

void main() {
    // Decode normal from GA channels (FA DXT5nm encoding: X=Green, Y=Alpha)
    vec4 nmap = texture(texNormal, fragUV);
    vec3 tangentNormal;
    tangentNormal.x = nmap.g * 2.0 - 1.0;
    tangentNormal.y = nmap.a * 2.0 - 1.0;
    tangentNormal.z = sqrt(max(0.0, 1.0 - tangentNormal.x*tangentNormal.x
                                         - tangentNormal.y*tangentNormal.y));

    // TBN matrix: transform tangent-space normal to world space
    vec3 N = normalize(fragNormal);
    vec3 T = normalize(fragTangent);
    vec3 B = normalize(fragBitangent);
    mat3 TBN = mat3(T, B, N);
    vec3 worldNormal = normalize(TBN * tangentNormal);

    // Diffuse lighting (Lambertian) + shadow
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float NdotL = max(dot(worldNormal, lightDir), 0.0);
    float shadow = calcShadow(fragWorldPos);
    float lighting = 0.4 + 0.6 * NdotL * shadow;

    vec4 texColor = texture(texAlbedo, fragUV);
    vec4 specTeam = texture(texSpecTeam, fragUV);

    // Team color mask from SpecTeam alpha channel
    float teamMask = specTeam.a;
    vec3 blended = mix(texColor.rgb, fragColor.rgb, teamMask);

    // Specular lighting (Blinn-Phong)
    vec3 viewDir = normalize(vec3(pc.eyeX, pc.eyeY, pc.eyeZ) - fragWorldPos);
    vec3 halfDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(worldNormal, halfDir), 0.0);
    float specIntensity = specTeam.r;
    float spec = pow(NdotH, 32.0) * specIntensity * shadow;

    vec3 lit = blended * lighting + vec3(spec);

    // Atmospheric distance fog
    float dist = length(vec3(pc.eyeX, pc.eyeY, pc.eyeZ) - fragWorldPos);
    float fogDensity = 0.0005;
    float atmosFog = 1.0 - exp(-dist * fogDensity);
    vec3 hazeColor = vec3(0.55, 0.62, 0.72);
    lit = mix(lit, hazeColor, atmosFog);

    outColor = vec4(lit, texColor.a * fragColor.a);
}
)glsl";

const char* decal_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

// Per-vertex (binding 0): position + UV
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;

// Per-instance (binding 1): model matrix (4 vec4 columns at locations 2-5)
layout(location = 2) in mat4 inModel;

layout(location = 0) out vec2 fragUV;

void main() {
    vec4 worldPos = inModel * vec4(inPosition, 1.0);
    gl_Position = pc.viewProj * worldPos;
    fragUV = inUV;
}
)glsl";

const char* decal_frag = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform sampler2D texAlbedo;

layout(location = 0) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 color = texture(texAlbedo, fragUV);
    if (color.a < 0.01) discard;
    outColor = color;
}
)glsl";

// --- Shadow depth-only shaders ---

const char* shadow_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 lightViewProj;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;  // unused but must match terrain vertex layout

void main() {
    gl_Position = pc.lightViewProj * vec4(inPosition, 1.0);
}
)glsl";

const char* shadow_mesh_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 lightViewProj;
    uint boneBase;
    uint bonesPerInst;
} pc;

// Per-vertex (binding 0): position + normal + UV + bone_indices + bone_weights + tangent
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 8) in uvec4 inBoneIndices;
layout(location = 9) in vec4 inBoneWeights;
layout(location = 10) in vec3 inTangent;

// Per-instance (binding 1) — mat4 uses locations 3-6 (4 vec4 columns)
layout(location = 3) in mat4 inModel;
layout(location = 7) in vec4 inColor;

// Bone SSBO (set=0, binding=0)
layout(std430, set = 0, binding = 0) readonly buffer BoneBuffer {
    mat4 bones[];
} boneSSBO;

void main() {
    uint base = pc.boneBase + uint(gl_InstanceIndex) * pc.bonesPerInst;
    mat4 bone = inBoneWeights[0] * boneSSBO.bones[base + inBoneIndices[0]]
              + inBoneWeights[1] * boneSSBO.bones[base + inBoneIndices[1]]
              + inBoneWeights[2] * boneSSBO.bones[base + inBoneIndices[2]]
              + inBoneWeights[3] * boneSSBO.bones[base + inBoneIndices[3]];
    vec4 skinnedPos = bone * vec4(inPosition, 1.0);
    vec4 worldPos = inModel * skinnedPos;
    gl_Position = pc.lightViewProj * worldPos;
}
)glsl";

const char* shadow_unit_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 lightViewProj;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;  // unused but must match cube vertex layout

layout(location = 2) in vec3 inInstancePos;
layout(location = 3) in float inScale;
layout(location = 4) in vec4 inColor;   // unused

void main() {
    vec3 worldPos = inPosition * inScale + inInstancePos;
    gl_Position = pc.lightViewProj * vec4(worldPos, 1.0);
}
)glsl";

const char* shadow_frag = R"glsl(
#version 450
void main() {}
)glsl";

// --- 2D UI shaders ---

const char* ui_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    float viewportWidth;
    float viewportHeight;
} pc;

// Per-instance data (binding 0, instance rate)
layout(location = 0) in vec4 inRect;    // x, y, w, h in pixels
layout(location = 1) in vec4 inUVRect;  // u0, v0, u1, v1
layout(location = 2) in vec4 inColor;   // r, g, b, a

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main() {
    // Generate unit quad from gl_VertexIndex (6 verts = 2 triangles)
    vec2 pos;
    int idx = gl_VertexIndex;
    if (idx == 0)      pos = vec2(0, 0);
    else if (idx == 1) pos = vec2(1, 0);
    else if (idx == 2) pos = vec2(1, 1);
    else if (idx == 3) pos = vec2(0, 0);
    else if (idx == 4) pos = vec2(1, 1);
    else               pos = vec2(0, 1);

    // Scale to pixel rect
    vec2 pixel = inRect.xy + pos * inRect.zw;

    // Convert to NDC [-1, 1], Vulkan Y-down
    gl_Position = vec4(
        pixel.x / pc.viewportWidth * 2.0 - 1.0,
        pixel.y / pc.viewportHeight * 2.0 - 1.0,
        0.0, 1.0
    );

    // Interpolate UV within the UV rect
    fragUV = mix(inUVRect.xy, inUVRect.zw, pos);
    fragColor = inColor;
}
)glsl";

const char* ui_frag = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform sampler2D texSampler;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texColor = texture(texSampler, fragUV);
    outColor = texColor * fragColor;
    if (outColor.a < 0.01) discard;
}
)glsl";

} // namespace shaders

} // namespace osc::renderer
