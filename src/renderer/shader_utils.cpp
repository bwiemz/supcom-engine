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
    float mapWidth, mapHeight;
    float _pad0, _pad1;   // explicit padding to match vec4 alignment
    float eyeX, eyeY, eyeZ;
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
    float _pad0, _pad1;   // explicit padding to match vec4 alignment
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

// Normal overlay from baked decal normal maps (binding 21)
layout(set = 0, binding = 21) uniform sampler2D normalOverlay;

// The upper stratum's albedo (binding 22): laid over the rest by its alpha
layout(set = 0, binding = 22) uniform sampler2D upperAlbedo;

// Each stratum's size in world units, for its albedo and its normal map
// (FA's StratumNAlbedoTile / NormalTile: the texture repeats every `size`).
layout(set = 0, binding = 23) uniform TerrainStrata {
    vec4 albedoSize0_3;
    vec4 albedoSize4_7;
    vec4 albedoSize8_upper;   // x: stratum 8, y: the upper stratum
    vec4 normalSize0_3;
    vec4 normalSize4_7;
    vec4 normalSize8;
} strata;

// Shadow map (set=1)
layout(set = 1, binding = 0) uniform sampler2DShadow shadowMap;
layout(set = 1, binding = 1) uniform LightUBO {
    mat4 lightViewProj;
    vec4 sunDirection;  // xyz toward the sun (the map's, M210a)
    vec4 sunColor;      // rgb; w: LightingMultiplier
    vec4 sunAmbience;   // rgb; w: 1 for the TTerrainXP terrain shader
    vec4 shadowFill;    // rgb: ShadowFillColor
    vec4 specularColor;
} lightUbo;

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

// A stratum normal map is an ordinary tangent-space normal in RGB, z up,
// whatever its compression (DXT1, 3 or 5): FA's TerrainNormalsPS reads it
// as tex * 2 - 1. Only decal normals are DXT5nm (green, alpha).
vec3 decodeNormal(sampler2D nmap, vec2 uv) {
    return texture(nmap, uv).rgb * 2.0 - 1.0;
}

void main() {
    // Blend map UV: world position normalized to [0,1] over map extents
    vec2 blendUV = fragWorldXZ / vec2(pc.mapWidth, pc.mapHeight);

    // Sample blend weights (RGBA = 4 strata weights each). TTerrain maps
    // (the original game's) blend four strata, from the first texture;
    // TTerrainXP maps all eight (FA's TerrainPS, TerrainAlbedoXP).
    bool xp = lightUbo.sunAmbience.w >= 0.5;
    vec4 b0 = texture(blendMap0, blendUV);
    vec4 b1 = xp ? texture(blendMap1, blendUV) : vec4(0.0);

    // Per-stratum UVs: the world over each texture's size
    vec2 uv0 = fragWorldXZ / strata.albedoSize0_3.x;
    vec2 uv1 = fragWorldXZ / strata.albedoSize0_3.y;
    vec2 uv2 = fragWorldXZ / strata.albedoSize0_3.z;
    vec2 uv3 = fragWorldXZ / strata.albedoSize0_3.w;
    vec2 uv4 = fragWorldXZ / strata.albedoSize4_7.x;
    vec2 uv5 = fragWorldXZ / strata.albedoSize4_7.y;
    vec2 uv6 = fragWorldXZ / strata.albedoSize4_7.z;
    vec2 uv7 = fragWorldXZ / strata.albedoSize4_7.w;
    vec2 uv8 = fragWorldXZ / strata.albedoSize8_upper.x;

    // Sample each stratum albedo
    vec4 s0 = texture(stratum0, uv0);
    vec4 s1 = texture(stratum1, uv1);
    vec4 s2 = texture(stratum2, uv2);
    vec4 s3 = texture(stratum3, uv3);
    vec4 s4 = texture(stratum4, uv4);
    vec4 s5 = texture(stratum5, uv5);
    vec4 s6 = texture(stratum6, uv6);
    vec4 s7 = texture(stratum7, uv7);
    vec4 s8 = texture(stratum8, uv8);

    // Each stratum replaces the layers below by its mask, sharpened as FA
    // sharpens it: saturate(mask * 2 - 1), so a mask under one half adds
    // nothing. The upper stratum then covers the rest by its own alpha.
    vec4 m0 = clamp(b0 * 2.0 - 1.0, 0.0, 1.0);
    vec4 m1 = clamp(b1 * 2.0 - 1.0, 0.0, 1.0);
    vec4 albedo = s0;
    albedo = mix(albedo, s1, m0.r);
    albedo = mix(albedo, s2, m0.g);
    albedo = mix(albedo, s3, m0.b);
    albedo = mix(albedo, s4, m0.a);
    albedo = mix(albedo, s5, m1.r);
    albedo = mix(albedo, s6, m1.g);
    albedo = mix(albedo, s7, m1.b);
    albedo = mix(albedo, s8, m1.a);
    vec4 upper = texture(upperAlbedo, fragWorldXZ / strata.albedoSize8_upper.y);
    albedo.rgb = mix(albedo.rgb, upper.rgb, upper.a);
    vec3 color = albedo.rgb;

    // Normal maps, each at its own size, blend by the raw masks
    // (TerrainNormalsPS reads the blend texture as it is).
    vec3 n0 = decodeNormal(normalMap0, fragWorldXZ / strata.normalSize0_3.x);
    vec3 n1 = decodeNormal(normalMap1, fragWorldXZ / strata.normalSize0_3.y);
    vec3 n2 = decodeNormal(normalMap2, fragWorldXZ / strata.normalSize0_3.z);
    vec3 n3 = decodeNormal(normalMap3, fragWorldXZ / strata.normalSize0_3.w);
    vec3 n4 = decodeNormal(normalMap4, fragWorldXZ / strata.normalSize4_7.x);
    vec3 n5 = decodeNormal(normalMap5, fragWorldXZ / strata.normalSize4_7.y);
    vec3 n6 = decodeNormal(normalMap6, fragWorldXZ / strata.normalSize4_7.z);
    vec3 n7 = decodeNormal(normalMap7, fragWorldXZ / strata.normalSize4_7.w);
    vec3 n8 = decodeNormal(normalMap8, fragWorldXZ / strata.normalSize8.x);

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

    // Apply baked normal overlay from decal normal maps
    {
        vec2 overlayUV = fragWorldXZ / vec2(pc.mapWidth, pc.mapHeight);
        vec2 overlayVal = texture(normalOverlay, overlayUV).rg;
        // Decode from [0,1] RGBA8 back to [-1,1] perturbation
        vec2 perturbation = overlayVal * 2.0 - 1.0;
        // Only apply if non-neutral (avoid perturbing where no decals exist)
        if (abs(perturbation.x) > 0.004 || abs(perturbation.y) > 0.004) {
            blendedTangentNormal.x += perturbation.x;
            blendedTangentNormal.y += perturbation.y;
            blendedTangentNormal = normalize(blendedTangentNormal);
        }
    }

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

    // FA's terrain lighting (terrain.fx), by the map's light (M210a).
    vec3 worldPos = vec3(fragWorldXZ.x, fragWorldY, fragWorldXZ.y);
    float shadow = calcShadow(worldPos);
    vec3 S = lightUbo.sunDirection.xyz;
    float SdotN = dot(S, worldNormal);
    vec3 V = normalize(worldPos - vec3(pc.eyeX, pc.eyeY, pc.eyeZ)); // eye to point
    float multiplier = lightUbo.sunColor.w;
    vec3 fill = lightUbo.shadowFill.rgb;
    vec3 lit;
    if (!xp) {
        // TTerrain (CalculateLighting): specular where the albedo's alpha
        // is low, added into the light.
        vec3 R = S - 2.0 * SdotN * worldNormal;
        float spec = pow(clamp(dot(R, V), 0.0, 1.0), 80.0) * lightUbo.specularColor.x * (1.0 - albedo.a);
        vec3 light = lightUbo.sunColor.rgb * clamp(SdotN, 0.0, 1.0) * shadow + lightUbo.sunAmbience.rgb + spec;
        light = multiplier * light + fill * (1.0 - light);
        lit = light * color;
    } else {
        // TTerrainXP (TerrainAlbedoXP): specular from the albedo's alpha.
        vec3 r = reflect(V, worldNormal);
        vec3 spec = pow(clamp(dot(r, S), 0.0, 1.0), 80.0) * albedo.a * lightUbo.specularColor.a * lightUbo.specularColor.rgb;
        vec3 light = lightUbo.sunColor.rgb * clamp(SdotN, 0.0, 1.0) * shadow + lightUbo.sunAmbience.rgb;
        light = multiplier * light + fill * (1.0 - light);
        lit = light * (color + spec);
    }

    // Fog of war: CPU-blurred texture, smooth transitions
    // FA shows unexplored at ~45% brightness with mild desaturation
    float fogVal = texture(fogMap, blendUV).r;
    float fogBright = mix(0.45, 1.0, fogVal);
    // Mild desaturation in unexplored areas
    float fogSat = mix(0.65, 1.0, fogVal);
    vec3 gray = vec3(dot(lit, vec3(0.299, 0.587, 0.114)));
    lit = mix(gray, lit, fogSat);
    lit *= fogBright;
    // No distance fog: FA's shaders have none (M210a).

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
layout(set = 0, binding = 1) uniform LightUBO {
    mat4 lightViewProj;
    vec4 sunDirection;  // xyz toward the sun (the map's, M210a)
    vec4 sunColor;      // rgb; w: LightingMultiplier
    vec4 sunAmbience;   // rgb; w: 1 for the TTerrainXP terrain shader
    vec4 shadowFill;    // rgb: ShadowFillColor
    vec4 specularColor;
} lightUbo;

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
    // FA's ComputeLight (mesh.fx), by the map's light (M210a).
    float NdotL = dot(lightUbo.sunDirection.xyz, normalize(fragNormal));
    float shadow = calcShadow(fragWorldPos);
    vec3 light = lightUbo.sunColor.rgb * clamp(NdotL, 0.0, 1.0) * shadow + lightUbo.sunAmbience.rgb;
    light = lightUbo.sunColor.w * light + (1.0 - light) * lightUbo.shadowFill.rgb;

    outColor = vec4(fragColor.rgb * light, fragColor.a);
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
    // Depth-based color: FA-style teal shallow to dark blue deep
    float d = clamp(fragDepth / 12.0, 0.0, 1.0);
    vec3 shallowColor = vec3(0.12, 0.38, 0.42);
    vec3 midColor     = vec3(0.06, 0.22, 0.38);
    vec3 deepColor    = vec3(0.02, 0.08, 0.22);
    // Two-stage depth gradient for richer transitions
    vec3 waterColor = d < 0.4
        ? mix(shallowColor, midColor, d / 0.4)
        : mix(midColor, deepColor, (d - 0.4) / 0.6);

    // Sun direction (same as terrain/mesh shaders)
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 N = normalize(fragNormal);

    // Diffuse shading on water surface
    float NdotL = max(dot(N, lightDir), 0.0);
    waterColor *= 0.75 + 0.25 * NdotL;

    // View-dependent effects
    vec3 eyePos = vec3(pc.eyeX, pc.eyeY, pc.eyeZ);
    vec3 viewDir = normalize(eyePos - fragWorldPos);
    vec3 halfDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(N, halfDir), 0.0);
    float NdotV = max(dot(viewDir, N), 0.0);

    // Sun specular: tight highlight + broader sun streak
    float specTight = pow(NdotH, 128.0) * 1.2;
    float specBroad = pow(NdotH, 16.0) * 0.15;
    float spec = specTight + specBroad;

    // Fresnel: Schlick approximation — more reflective at grazing angles
    float fresnel = 0.02 + 0.40 * pow(1.0 - NdotV, 4.0);

    // Sky reflection color (tinted blue, stronger at grazing angles)
    vec3 skyReflect = vec3(0.35, 0.45, 0.58) * fresnel;
    waterColor = mix(waterColor, skyReflect, fresnel);
    waterColor += vec3(1.0, 0.95, 0.85) * spec;

    // Animated caustic-like pattern on shallow water (subtle)
    float caustic1 = sin(fragWorldPos.x * 0.5 + pc.time * 0.8)
                   * cos(fragWorldPos.z * 0.6 - pc.time * 0.5);
    float caustic2 = sin(fragWorldPos.x * 0.3 - pc.time * 0.6)
                   * cos(fragWorldPos.z * 0.4 + pc.time * 0.7);
    float caustic = (caustic1 + caustic2) * 0.5;
    float causticMask = (1.0 - d) * 0.08; // only visible in shallow water
    waterColor += vec3(caustic * causticMask);

    // Shore foam: white band where depth is very shallow
    float foam = smoothstep(0.8, 0.0, fragDepth) * 0.35;
    // Animated foam sparkle
    float sparkle = sin(fragWorldPos.x * 2.0 + pc.time * 3.0)
                  * cos(fragWorldPos.z * 2.5 + pc.time * 2.0);
    foam *= 0.7 + 0.3 * sparkle;
    waterColor += vec3(foam);

    // Alpha: more opaque in deep water, semi-transparent at shore
    float alpha = mix(0.50, 0.88, d);

    // No distance fog: FA's shaders have none (M210a). The water's own
    // lighting is M213's.
    outColor = vec4(clamp(waterColor, 0.0, 1.0), alpha);
}
)glsl";

const char* mesh_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    uint boneBase;
    uint bonesPerInst;
    float eyeX, eyeY, eyeZ;
    uint technique; // MeshTechnique (M211b)
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
    // Blend-weight skeletal skinning: skip for unskinned meshes (bonesPerInst == 0)
    mat4 bone;
    if (pc.bonesPerInst > 0u) {
        uint base = pc.boneBase + uint(gl_InstanceIndex) * pc.bonesPerInst;
        bone = inBoneWeights[0] * boneSSBO.bones[base + inBoneIndices[0]]
             + inBoneWeights[1] * boneSSBO.bones[base + inBoneIndices[1]]
             + inBoneWeights[2] * boneSSBO.bones[base + inBoneIndices[2]]
             + inBoneWeights[3] * boneSSBO.bones[base + inBoneIndices[3]];
    } else {
        bone = mat4(1.0); // identity — no skinning for props/unskinned meshes
    }
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

// Full block declared for layout compatibility; boneBase/bonesPerInst unused here
layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    uint boneBase;
    uint bonesPerInst;
    float eyeX, eyeY, eyeZ;
    uint technique; // MeshTechnique: 0 Unit, 1 Aeon, 2 Insect, 3 Metal, 4 Seraphim (M211b)
} pc;

layout(set = 0, binding = 0) uniform sampler2D texAlbedo;
layout(set = 2, binding = 0) uniform sampler2D texSpecTeam;
layout(set = 3, binding = 0) uniform sampler2D texNormal;

// Shadow map (set=4)
layout(set = 4, binding = 0) uniform sampler2DShadow shadowMap;
layout(set = 4, binding = 1) uniform LightUBO {
    mat4 lightViewProj;
    vec4 sunDirection;  // xyz toward the sun (the map's, M210a)
    vec4 sunColor;      // rgb; w: LightingMultiplier
    vec4 sunAmbience;   // rgb; w: 1 for the TTerrainXP terrain shader
    vec4 shadowFill;    // rgb: ShadowFillColor
    vec4 specularColor;
} lightUbo;
// The map's environment cubes meshes reflect, by their technique's key
// (M211a/b), and FA's lookup textures
layout(set = 4, binding = 2) uniform samplerCube environmentMap;  // "<default>"
layout(set = 4, binding = 3) uniform samplerCube aeonEnvironment; // "<aeon>"
layout(set = 4, binding = 4) uniform samplerCube seraphimEnvironment; // "<seraphim>"
layout(set = 4, binding = 5) uniform sampler2D anisotropicLookup;
layout(set = 4, binding = 6) uniform sampler2D insectLookup;

layout(location = 0) in vec3 fragNormal;
// Army colour (RGB) + build alpha (A). A negative red marks a wreck, a
// negative green a prop (drawn without a team mask).
layout(location = 1) in vec4 fragColor;
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

// FA's viewDirection (mesh.fx): the point's normalised device position,
// turned into the world by the view's rotation. Moho's view is right-handed,
// so that depth (near 1) runs back toward the eye: V points from the point
// to the eye along the view axis, but its sideways part is mirrored (and
// steepened by the frustum's slope). The rows of viewProj hold the view's
// axes: x's the right, y's the up (both negated with the device's y, which
// runs down), w's the forward.
vec3 faViewDirection(vec3 worldPos) {
    vec4 clip = pc.viewProj * vec4(worldPos, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    mat4 m = pc.viewProj;
    vec3 right = normalize(vec3(m[0][0], m[1][0], m[2][0]));
    vec3 upDevice = normalize(vec3(m[0][1], m[1][1], m[2][1]));
    vec3 back = -normalize(vec3(m[0][3], m[1][3], m[2][3]));
    return normalize(ndc.x * right + ndc.y * upDevice + ndc.z * back);
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

    // FA's ComputeLight (mesh.fx), by the map's light (M210a).
    vec3 S = lightUbo.sunDirection.xyz;
    float NdotL = dot(worldNormal, S);
    float shadow = calcShadow(fragWorldPos);
    vec3 light = lightUbo.sunColor.rgb * clamp(NdotL, 0.0, 1.0) * shadow + lightUbo.sunAmbience.rgb;
    light = lightUbo.sunColor.w * light + (1.0 - light) * lightUbo.shadowFill.rgb;

    vec4 texColor = texture(texAlbedo, fragUV);
    // r: how much the environment is reflected, g: the highlight, b: glow,
    // a: the team colour's mask
    vec4 specTeam = texture(texSpecTeam, fragUV);

    vec3 lit;
    if (fragColor.r < 0.0) {
        // A wreck: burnt dark and grey, barely shining. (FA's WreckagePS,
        // with its own crunch texture, is M211c's.)
        float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        vec3 blended = mix(vec3(lum), texColor.rgb, 0.3) * 0.45;
        vec3 viewDir = normalize(vec3(pc.eyeX, pc.eyeY, pc.eyeZ) - fragWorldPos);
        vec3 halfDir = normalize(S + viewDir);
        float spec = pow(max(dot(worldNormal, halfDir), 0.0), 32.0) * specTeam.r * 0.25 * shadow;
        lit = blended * light + vec3(spec);
    } else {
        // FA's mesh.fx, by the mesh's technique. Props (NormalMappedAlpha)
        // tint by their colour; units mask the team's colour in.
        bool prop = fragColor.g < 0.0;
        vec3 albedo = prop ? texColor.rgb : mix(texColor.rgb, fragColor.rgb, specTeam.a);
        vec3 V = faViewDirection(fragWorldPos);
        vec3 R = reflect(-V, worldNormal);
        float phongAmount = clamp(dot(reflect(S, worldNormal), -V), 0.0, 1.0);
        float emissive = 2.0 * specTeam.b; // glowMultiplier
        if (pc.technique == 1u) {
            // AeonPS: its own cube and highlight, and the sun at 0.6.
            vec3 environment = texture(aeonEnvironment, R).rgb;
            vec3 phongAdditive = vec3(0.8, 0.85, 1.10) * pow(phongAmount, 3.0) * specTeam.g;
            vec3 aeonLight = lightUbo.sunColor.rgb * clamp(NdotL, 0.0, 1.0) * shadow +
                             lightUbo.sunAmbience.rgb;
            aeonLight = 0.6 * lightUbo.sunColor.w * aeonLight +
                        (1.0 - aeonLight) * lightUbo.shadowFill.rgb;
            lit = albedo * (emissive + aeonLight + specTeam.r * environment) + phongAdditive;
        } else if (pc.technique == 2u || pc.technique == 3u) {
            // NormalMappedInsectPS (Cybran) and NormalMappedMetalPS: an
            // anisotropic highlight from a lookup, half the environment.
            vec3 environment = texture(environmentMap, R).rgb;
            vec2 anisoAt = vec2(dot(reflect(S, worldNormal), -V), NdotL);
            vec3 aniso = pc.technique == 2u ? texture(insectLookup, anisoAt).rgb
                                            : texture(anisotropicLookup, anisoAt).rgb;
            vec3 phongAdditive = aniso * specTeam.g + 0.5 * specTeam.r * environment;
            vec3 meshLight = light;
            if (pc.technique == 2u) {
                // The insect's highlight stays off its team colour, and it
                // takes the sun twice over.
                phongAdditive *= 1.0 - specTeam.a;
                meshLight = 2.0 * lightUbo.sunColor.rgb * clamp(NdotL, 0.0, 1.0) * shadow +
                            lightUbo.sunAmbience.rgb;
                meshLight = lightUbo.sunColor.w * meshLight +
                            (1.0 - meshLight) * lightUbo.shadowFill.rgb;
            }
            lit = albedo * (emissive + meshLight) + phongAdditive;
        } else {
            // NormalMappedPS (the Unit technique; Seraphim's UnitFalloffPS
            // is M211c's).
            vec3 environment = texture(environmentMap, R).rgb;
            vec3 phongAdditive = vec3(0.6, 0.8, 0.9) * pow(phongAmount, 2.0) * specTeam.g;
            vec3 phongMultiplicative = 2.0 * environment * specTeam.r;
            lit = albedo * (emissive + light + phongMultiplicative) + phongAdditive;
        }
    }

    float finalAlpha = texColor.a * fragColor.a;
    if (finalAlpha < 0.1) discard;
    outColor = vec4(lit, finalAlpha);
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
    mat4 bone;
    if (pc.bonesPerInst > 0u) {
        uint base = pc.boneBase + uint(gl_InstanceIndex) * pc.bonesPerInst;
        bone = inBoneWeights[0] * boneSSBO.bones[base + inBoneIndices[0]]
             + inBoneWeights[1] * boneSSBO.bones[base + inBoneIndices[1]]
             + inBoneWeights[2] * boneSSBO.bones[base + inBoneIndices[2]]
             + inBoneWeights[3] * boneSSBO.bones[base + inBoneIndices[3]];
    } else {
        bone = mat4(1.0);
    }
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

// ---------------------------------------------------------------------------
// Particle billboard shaders
// ---------------------------------------------------------------------------

const char* particle_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec3 camRight;
    float _pad0;
    vec3 camUp;
    float _pad1;
} pc;

// Per-instance data (binding 0, instance rate)
layout(location = 0) in vec3 inPos;       // world position
layout(location = 1) in float inSize;     // billboard half-extent
layout(location = 2) in float inRotation; // rotation in radians
layout(location = 3) in float inAlpha;
layout(location = 4) in vec2 inUVOffset;  // texture frame offset
layout(location = 5) in vec2 inUVSize;    // texture frame size
layout(location = 6) in vec3 inColor;     // tint
layout(location = 7) in float inRampU;    // life fraction

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out float fragRampU;

void main() {
    // 6 vertices per quad (2 triangles)
    vec2 corner;
    int idx = gl_VertexIndex;
    if (idx == 0)      corner = vec2(-1, -1);
    else if (idx == 1) corner = vec2( 1, -1);
    else if (idx == 2) corner = vec2( 1,  1);
    else if (idx == 3) corner = vec2(-1, -1);
    else if (idx == 4) corner = vec2( 1,  1);
    else               corner = vec2(-1,  1);

    // Apply rotation
    float c = cos(inRotation);
    float s = sin(inRotation);
    vec2 rotated = vec2(
        corner.x * c - corner.y * s,
        corner.x * s + corner.y * c
    );

    // Billboard offset in world space
    vec3 worldPos = inPos
        + pc.camRight * rotated.x * inSize
        + pc.camUp    * rotated.y * inSize;

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);

    // UV: map corner [-1,1] to [0,1] then scale to frame
    vec2 uv01 = corner * 0.5 + 0.5;
    fragUV = inUVOffset + uv01 * inUVSize;

    fragColor = vec4(inColor, inAlpha);
    fragRampU = inRampU;
}
)glsl";

const char* particle_frag = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform sampler2D texSampler;
layout(set = 1, binding = 0) uniform sampler2D rampSampler; // colour over life

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in float fragRampU;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texColor = texture(texSampler, fragUV);
    vec4 ramp = texture(rampSampler, vec2(clamp(fragRampU, 0.0, 1.0), 0.5));
    outColor = texColor * fragColor * ramp;
    if (outColor.a < 0.01) discard;
}
)glsl";

const char* bloom_bright_vert = R"glsl(
#version 450

layout(location = 0) out vec2 fragUV;

void main() {
    fragUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(fragUV * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

const char* bloom_bright_frag = R"glsl(
#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;

layout(push_constant) uniform PC {
    float threshold;
    float intensity;
} pc;

void main() {
    vec3 color = texture(sceneTex, fragUV).rgb;
    vec3 bright = max(color - vec3(pc.threshold), vec3(0.0));
    outColor = vec4(bright * pc.intensity, 1.0);
}
)glsl";

const char* bloom_blur_frag = R"glsl(
#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D inputTex;

layout(push_constant) uniform PC {
    vec2 direction; // (1/w, 0) for horizontal, (0, 1/h) for vertical
} pc;

void main() {
    const float weights[5] = float[](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);
    vec3 result = texture(inputTex, fragUV).rgb * weights[0];
    for (int i = 1; i < 5; i++) {
        vec2 offset = pc.direction * float(i);
        result += texture(inputTex, fragUV + offset).rgb * weights[i];
        result += texture(inputTex, fragUV - offset).rgb * weights[i];
    }
    outColor = vec4(result, 1.0);
}
)glsl";

const char* bloom_composite_frag = R"glsl(
#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 1, binding = 0) uniform sampler2D bloomTex;

layout(push_constant) uniform PC {
    float bloomStrength;
} pc;

void main() {
    vec3 scene = texture(sceneTex, fragUV).rgb;
    vec3 bloom = texture(bloomTex, fragUV).rgb;
    outColor = vec4(scene + bloom * pc.bloomStrength, 1.0);
}
)glsl";

} // namespace shaders

} // namespace osc::renderer
