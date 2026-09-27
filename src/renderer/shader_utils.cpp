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
    // The frame's glow (M211e): TTerrain's specular, a little; XP's none.
    float glow = 0.0;
    if (!xp) {
        // TTerrain (CalculateLighting): specular where the albedo's alpha
        // is low, added into the light.
        vec3 R = S - 2.0 * SdotN * worldNormal;
        float spec = pow(clamp(dot(R, V), 0.0, 1.0), 80.0) * lightUbo.specularColor.x * (1.0 - albedo.a);
        vec3 light = lightUbo.sunColor.rgb * clamp(SdotN, 0.0, 1.0) * shadow + lightUbo.sunAmbience.rgb + spec;
        light = multiplier * light + fill * (1.0 - light);
        lit = light * color;
        glow = 0.01 + spec * lightUbo.specularColor.w;
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

    outColor = vec4(lit, glow);
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
    uint pass;      // a build technique's pass: 0, or 1 for its overlay (M211f)
    float time;     // FA's time: the newest tick plus the interpolant (M211f)
} pc;

// Per-vertex (binding 0): position + normal + UV + bone_indices + bone_weights + tangent
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 8) in uvec4 inBoneIndices;
layout(location = 9) in vec4 inBoneWeights;
layout(location = 10) in vec3 inTangent;
layout(location = 11) in vec3 inBinormal;
// Per-instance: the row of the mesh's lookup texture for its army (M211c)
layout(location = 12) in float inColorLookup;
// Per-instance: the tick its mesh instance was made (FA's material.x, M211d)
layout(location = 13) in float inShaderTime;
// Per-instance: the fraction complete (FA's material.y for the build techniques, M211f)
layout(location = 14) in float inParameter;

// Per-instance (binding 1) — mat4 uses locations 3-6 (4 vec4 columns)
layout(location = 3) in mat4 inModel;
layout(location = 7) in vec4 inColor;

// Bone SSBO (set=1, binding=0)
layout(std430, set = 1, binding = 0) readonly buffer BoneBuffer {
    mat4 bones[];
} boneSSBO;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) flat out vec4 fragColor; // per instance: exact, not interpolated
layout(location = 2) out vec2 fragUV;
layout(location = 3) out vec3 fragTangent;
layout(location = 4) out vec3 fragBitangent;
layout(location = 5) out vec3 fragWorldPos;
layout(location = 6) flat out float fragColorLookup;
layout(location = 7) flat out float fragShaderTime;
layout(location = 8) flat out float fragParameter;

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
    // AeonBuildVS and SeraphimBuildVS grow the mesh as it's built.
    float grow = 1.0;
    if (pc.technique == 6u) grow = max(inParameter, 0.75);
    else if (pc.technique == 8u) grow = 0.25 + inParameter * 0.75;
    vec4 skinnedPos = bone * vec4(inPosition * grow, 1.0);
    vec4 worldPos = inModel * skinnedPos;
    if (pc.technique == 17u) {
        // UndulatingNormalMappedVS: swaying in FA's wind (mesh.fx's
        // windDirection, which Moho never sets), by the vertex's height in
        // its mesh, out of step by the instance's place (M211i).
        const vec3 wind = vec3(0.707, 0.0, 0.707);
        float sway = sin(0.05 * pc.time - dot(wind, inModel[3].xyz));
        worldPos.xyz += 0.003 * inPosition.y * sway * sway * wind;
    }
    if (inColor.r < 0.0) {
        // A wreck (WreckageVS, mesh.fx) crumples: each vertex shifts by up to
        // 0.15, by its place in the world and the instance's.
        vec3 nvert = normalize(worldPos.xyz);
        float s = nvert.x * 0.15; // FA's float s = nvert * 0.15 keeps x
        float r = length(worldPos.xyz);
        float phi = fract(0.01 * length(inModel[3].xyz));
        worldPos.x += sin(14.5 * r * nvert.z + phi) * s;
        worldPos.y += cos(10.8 * r * nvert.x + phi) * s;
        worldPos.z += sin(20.5 * r * nvert.y + phi) * s;
    }
    gl_Position = pc.viewProj * worldPos;
    fragWorldPos = worldPos.xyz;
    // Transform TBN vectors through blended bone then model
    mat3 normalMat = mat3(inModel) * mat3(bone);
    fragNormal = normalMat * inNormal;
    fragTangent = normalMat * inTangent;
    fragBitangent = normalMat * inBinormal;
    fragColor = inColor;
    fragColorLookup = inColorLookup;
    fragShaderTime = inShaderTime;
    fragParameter = inParameter;
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
    // MeshTechnique: 0 Unit, 1 Aeon, 2 Insect, 3 Metal, 4 Seraphim (M211b);
    // 5 UEFBuild, 6 AeonBuild, 7 CybranBuild, 8 SeraphimBuild (M211f);
    // 9 NormalMappedAlpha, 10 NormalMappedGlow, 11 AlphaFade, 12 UEFBuildCube,
    // 13 AeonBuildPuddle, 14 BlackenedNormalMappedAlpha (M211g); 15 VertexNormal,
    // 16 NormalMappedTerrain, 17 UndulatingNormalMappedAlpha (M211i)
    uint technique;
    uint pass;  // a build technique's pass: 0, or 1 for its overlay
    float time; // FA's time: the newest tick plus the interpolant, wrapped
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
// The mesh's own lookup, its LOD's LookupName (Seraphim's falloff, M211c)
layout(set = 5, binding = 0) uniform sampler2D texLookup;
// The mesh's SecondaryName: its faction's build overlay (M211f)
layout(set = 6, binding = 0) uniform sampler2D texSecondary;

layout(location = 0) in vec3 fragNormal;
// Army colour (RGB) + build alpha (A). A negative red marks a wreck, a
// negative green a prop (drawn without a team mask).
layout(location = 1) flat in vec4 fragColor;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in vec3 fragTangent;
layout(location = 4) in vec3 fragBitangent;
layout(location = 5) in vec3 fragWorldPos;
layout(location = 6) flat in float fragColorLookup; // the army's row of texLookup
layout(location = 7) flat in float fragShaderTime;  // the tick its mesh instance was made
layout(location = 8) flat in float fragParameter;   // the fraction complete

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

// FA's ComputeNormal (mesh.fx): the map's green runs along the binormal,
// its alpha along the tangent (DXT5nm's y and x), and z is what's left.
vec3 computeNormal(vec2 uv) {
    vec4 nmap = texture(texNormal, uv);
    vec3 n = vec3(nmap.g * 2.0 - 1.0, nmap.a * 2.0 - 1.0, 0.0);
    n.z = sqrt(max(0.0, 1.0 - n.x * n.x - n.y * n.y));
    return normalize(mat3(fragBitangent, fragTangent, fragNormal) * n);
}

// FA's ComputeLight (mesh.fx), by the map's light (M210a): the sun scaled by
// `sun` (the insect's twice over), the lit part by `scale` (Aeon's 0.6).
vec3 computeLight(float NdotL, float shadow, float sun, float scale) {
    vec3 light = sun * lightUbo.sunColor.rgb * clamp(NdotL, 0.0, 1.0) * shadow +
                 lightUbo.sunAmbience.rgb;
    return scale * lightUbo.sunColor.w * light + (1.0 - light) * lightUbo.shadowFill.rgb;
}

// FA's falloffSampler (point, clamped): the mesh's lookup, across by how
// squarely the surface faces the eye, down at the army's row.
vec4 fallOffAt(float across) {
    ivec2 size = textureSize(texLookup, 0);
    ivec2 at = clamp(ivec2(vec2(across, fragColorLookup) * vec2(size)), ivec2(0), size - 1);
    return texelFetch(texLookup, at, 0);
}
)glsl" // split in two: MSVC caps one string literal at 16380 bytes (C2026)
                        R"glsl(
// FA's build techniques (mesh.fx, M211f), for an instance f built, `age`
// ticks after its mesh instance was made. `alpha` is what the pass blends
// by. The overlays' texture coordinates are FA's vertex shaders' (scaled
// and shifted per vertex, so the same at each pixel).
vec3 buildColor(vec4 texColor, vec4 specTeam, vec3 N, vec3 V, float shadow, out float alpha) {
    vec3 S = lightUbo.sunDirection.xyz;
    float NdotL = dot(N, S);
    float f = fragParameter;
    float age = pc.time - fragShaderTime;
    // The overlays fade out over the last 5%.
    float fade = f >= 0.95 ? 1.0 - (f - 0.95) * 20.0 : 1.0;
    // The team's colour fades in over the last tenth, under the SpecTeam's mask.
    vec3 team = fragColor.rgb * (f >= 0.90 ? (f - 0.9) * 10.0 : 0.0);
    vec3 albedo = mix(team, texColor.rgb, 1.0 - specTeam.a);
    vec3 environment = texture(environmentMap, reflect(-V, N)).rgb; // no "environment" named
    float phongAmount = clamp(dot(reflect(S, N), -V), 0.0, 1.0);
    float emissive = 2.0 * specTeam.b; // glowMultiplier
    if (pc.technique == 5u && pc.pass == 0u) {
        // UEFBuildHiFiPS: pulsing toward blue (by FA's time, every 50
        // ticks), less so as it's built, under a scrolling secondary.
        vec4 secondary = texture(texSecondary, vec2(fragUV.x, fragUV.y + age * 0.0124) * 50.0);
        vec3 color = albedo * (emissive + computeLight(NdotL, shadow, 1.0, 1.0) +
                               2.0 * environment * specTeam.r) +
                     pow(phongAmount, 8.0) * specTeam.g;
        float t = min(max(fract(0.02 * pc.time), 0.35), 0.7);
        alpha = max(f, 0.5);
        return mix(mix(color + secondary.rgb, vec3(0.0, 0.0, 1.0), t), color, f);
    }
    if (pc.technique == 5u) {
        // UEFBuildOverlayHiFiPS, by EffectVertexNormalHiFiVS(16, 8, 0.0192,
        // 0.0176, -0.0122, -0.0122), which adds both of the second read's
        // shifts to both its coordinates.
        vec4 x = texture(texSecondary, fragUV * 16.0 + age * vec2(0.0192, 0.0176));
        vec4 y = texture(texSecondary, fragUV * 8.0 + age * (-0.0122 - 0.0122));
        alpha = max((x.a + y.a) * (1.0 - f), 0.25) * fade;
        return x.rgb + y.rgb;
    }
    if (pc.technique == 6u && pc.pass == 0u) {
        // AeonBuildPS: AeonPS's light, opaque (its glow isn't written).
        alpha = 1.0;
        return albedo * (emissive + computeLight(NdotL, shadow, 1.0, 0.6) +
                         specTeam.r * environment) +
               pow(phongAmount, 8.0) * specTeam.g;
    }
    if (pc.technique == 6u) {
        // AeonBuildOverlayPS: two reads of the secondary, scrolling apart,
        // make a grey sheen, which bends the normal it's lit by.
        vec4 mask1 = texture(texSecondary, (fragUV + vec2(-0.001, 0.00162) * age) * 2.0);
        vec4 mask2 = texture(texSecondary, (fragUV - vec2(0.0, 0.00162) * age) * 2.0);
        vec3 sheen = vec3(mask1.r - mask2.g + mask1.g * mask2.r);
        sheen = mix(sheen, vec3(0.5), 0.75);
        vec3 n = mix(texture(texNormal, fragUV).gaa, texture(texSecondary, fragUV * 7.0).baa, 0.5);
        n = 2.0 * mix(n, sheen, 0.5) - 1.0;
        n.z = sqrt(abs(1.0 - n.x * n.x - n.y * n.y)); // ps_2_0's sqrt takes |x|
        n = normalize(mat3(fragBitangent, fragTangent, fragNormal) * n);
        float lit = clamp(dot(S, n), 0.0, 1.0);
        vec3 reflection = normalize(2.0 * lit * n - normalize(S));
        vec3 color = sheen * lit + pow(clamp(dot(reflection, V), 0.0, 1.0), 8.0);
        alpha = clamp(fade * color.r * 2.0, 0.0, 1.0);
        return color;
    }
    if (pc.technique == 7u && pc.pass == 0u) {
        // CybranBuildPS: NormalMappedInsectPS, 40% opaque until 70% built.
        vec2 anisoAt = vec2(dot(reflect(S, N), -V), NdotL);
        vec3 phongAdditive = (texture(insectLookup, anisoAt).rgb * specTeam.g +
                              0.5 * specTeam.r * environment) *
                             (1.0 - specTeam.a);
        alpha = f >= 0.7 ? 0.4 + 0.6 * ((f - 0.7) * 3.33) : 0.4;
        return albedo * (emissive + computeLight(NdotL, shadow, 2.0, 1.0)) + phongAdditive;
    }
    if (pc.technique == 7u) {
        // CybranBuildOverlayPS, by EffectVertexNormalLoFiVS(14, 4, 0, 0,
        // -0.008, 0.008): red lines, masked by a second, scrolling read.
        vec4 secondary = texture(texSecondary, fragUV * 14.0);
        vec4 mask = texture(texSecondary, fragUV * 4.0 + age * vec2(-0.008, 0.008));
        alpha = mask.r * secondary.a * fade;
        return vec3(secondary.a * 0.75, 0.0, 0.0);
    }
    // SeraphimBuildPS: the secondary, scrolling, shifts every read until
    // it's 90% built (ten times over at the start), then UnitFalloffPS's
    // colour, from the default environment.
    vec4 uvaddress = texture(texSecondary, vec2(fragUV.x, fragUV.y + age * 0.005) * 0.5) * 0.03;
    vec2 uv = fragUV + mix(uvaddress.rb, vec2(0.0), (f - 0.9) * 10.0);
    vec3 normal = computeNormal(uv);
    vec4 fallOff = fallOffAt(pow(1.0 - clamp(dot(V, normal), 0.0, 1.0), 0.6));
    vec4 diffuse = texture(texAlbedo, uv);
    vec4 specular = texture(texSpecTeam, uv);
    vec3 fill = lightUbo.sunAmbience.rgb +
                (1.0 - lightUbo.sunAmbience.rgb) * lightUbo.shadowFill.rgb;
    alpha = max(f, 0.25);
    return diffuse.rgb * fill +
           texture(environmentMap, reflect(-V, normal)).rgb * specular.r * fallOff.a +
           fallOff.rgb * diffuse.a;
}

// The build effects' own techniques (mesh.fx, M211g), for an instance f
// built, `age` ticks after its mesh instance was made.
vec3 effectColor(vec3 V, float shadow, out float alpha) {
    vec3 S = lightUbo.sunDirection.xyz;
    float f = fragParameter;
    float age = pc.time - fragShaderTime;
    if (pc.technique == 11u) {
        // AlphaFadePS(2.0, 0.145): lit by the vertex's normal (VertexNormalVS),
        // fading out from two ticks old.
        vec4 color = texture(texAlbedo, fragUV);
        alpha = color.a * f * clamp(1.0 - (age - 2.0) * 0.145, 0.0, 1.0);
        return color.rgb * computeLight(dot(S, normalize(fragNormal)), shadow, 1.0, 1.0);
    }
    if (pc.technique == 12u) {
        // UEFBuildCubePS: unlit, scrolling, pulsing toward blue (unclamped
        // above, unlike UEFBuild's) until built.
        vec2 uv = fragUV + age * vec2(0.012, 0.062);
        vec4 albedo = texture(texAlbedo, uv * 0.025);
        vec4 secondary = texture(texSecondary, (uv + vec2(0.0, age * 0.062)) * 50.0);
        vec3 current =
            mix(albedo.rgb + secondary.rgb, vec3(0.0, 0.0, 1.0), max(fract(0.05 * pc.time), 0.35));
        alpha = max(f, 0.5) * albedo.a;
        return mix(current, albedo.rgb, f);
    }
    // AeonBuildPuddlePS: AeonBuildPS's light and terms, every read scrolling,
    // no team colour; it glows.
    vec2 uv = fragUV + age * vec2(-0.002, 0.0042);
    vec3 N = computeNormal(uv);
    vec4 albedo = texture(texAlbedo, uv);
    vec4 specular = texture(texSpecTeam, uv);
    vec3 environment = texture(environmentMap, reflect(-V, N)).rgb;
    float phongAmount = clamp(dot(reflect(S, N), -V), 0.0, 1.0);
    alpha = specular.b + 0.01; // glowMinimum
    return albedo.rgb * (2.0 * specular.b + computeLight(dot(S, N), shadow, 1.0, 0.6) +
                         specular.r * environment) +
           pow(phongAmount, 8.0) * specular.g;
}

void main() {
    vec3 worldNormal = computeNormal(fragUV);
    vec3 S = lightUbo.sunDirection.xyz;
    float NdotL = dot(worldNormal, S);
    float shadow = calcShadow(fragWorldPos);
    vec3 light = computeLight(NdotL, shadow, 1.0, 1.0);

    vec4 texColor = texture(texAlbedo, fragUV);
    // r: how much the environment is reflected, g: the highlight, b: glow,
    // a: the team colour's mask
    vec4 specTeam = texture(texSpecTeam, fragUV);

    bool prop = fragColor.g < 0.0;
    // NormalMappedPS without the team's mask (props, NormalMappedAlpha,
    // NormalMappedGlow, the burnt trees'): the albedo tinted by the
    // instance's colour, white but a unit's. The alpha-tested ones write
    // colour only.
    // The props' own techniques (M211i) set their alpha below.
    bool vertexNormal = pc.technique == 15u;
    bool terrainProp = pc.technique == 16u;
    bool unmasked = prop || pc.technique == 9u || pc.technique == 10u || pc.technique == 14u ||
                    vertexNormal || terrainProp || pc.technique == 17u;
    bool alphaTested = (prop && !vertexNormal && !terrainProp) || pc.technique == 9u ||
                       pc.technique == 14u || pc.technique == 17u;
    vec3 tint = prop ? vec3(1.0) : fragColor.rgb;
    // Alpha: the build ghost's fade, which its pipeline blends by, leaving
    // the frame's alpha; a build technique's or effect's own (M211f/g); else
    // the glow FA's techniques write there (M211e): a unit's SpecTeam blue
    // plus glowMinimum, a wreck's glowMinimum, the colour-only techniques'
    // none (FA writes them no alpha, and what's beneath is at most 0.01 and
    // terrain specular).
    const float glowMinimum = 0.01;
    float alpha =
        fragColor.a < 1.0
            ? fragColor.a
            : (alphaTested ? 0.0 : (fragColor.r < 0.0 ? glowMinimum : specTeam.b + glowMinimum));
    vec3 lit;
    if (fragColor.r < 0.0) {
        // WreckagePS (mesh.fx): a wreck's "specular" is a crunch noise
        // (wreckage_noise.dds), tiled 5.15 times and offset by when its mesh
        // instance was made; the sun lights it unshadowed ("the random
        // crunchiness makes for bad artifacts").
        float offset = fract(0.01 * fragShaderTime);
        vec4 crunch = texture(texSpecTeam, (fragUV + vec2(offset, -offset)) * 5.15);
        lit = texColor.rgb * computeLight(NdotL, 1.0, 1.0, 1.0);
        if (crunch.g < 0.22)
            lit *= (texColor.rgb + crunch.r + crunch.a) * crunch.b * 2.5;
        else
            lit *= crunch.b * 2.0;
    } else if (pc.technique >= 5u && pc.technique <= 8u) {
        lit = buildColor(texColor, specTeam, worldNormal, faViewDirection(fragWorldPos), shadow,
                         alpha);
    } else if (pc.technique >= 11u && pc.technique <= 13u) {
        lit = effectColor(faViewDirection(fragWorldPos), shadow, alpha);
    } else if (vertexNormal || terrainProp) {
        // VertexNormalPS: lit by the vertex's normal, shadowed, and nothing
        // else, blended by f * albedo.a; NormalMappedTerrainPS: by the
        // normal map, unshadowed, alpha glowMinimum (M211i).
        vec3 albedo = texColor.rgb * tint;
        lit = vertexNormal
                  ? albedo * computeLight(dot(S, normalize(fragNormal)), shadow, 1.0, 1.0)
                  : albedo * computeLight(NdotL, 1.0, 1.0, 1.0);
        alpha = vertexNormal ? fragParameter * texColor.a : glowMinimum;
    } else {
        // FA's mesh.fx, by the mesh's technique. The unmasked tint by their
        // colour; units mask the team's colour in. The burnt trees'
        // (BlackenedNormalMappedPS) grey the albedo first.
        vec3 albedo = unmasked ? texColor.rgb * tint : mix(texColor.rgb, fragColor.rgb, specTeam.a);
        if (pc.technique == 14u) albedo = vec3(dot(texColor.rgb, vec3(0.1))) * tint;
        vec3 V = faViewDirection(fragWorldPos);
        vec3 R = reflect(-V, worldNormal);
        float phongAmount = clamp(dot(reflect(S, worldNormal), -V), 0.0, 1.0);
        float emissive = 2.0 * specTeam.b; // glowMultiplier
        if (pc.technique == 1u) {
            // AeonPS: its own cube and highlight, and the sun at 0.6.
            vec3 environment = texture(aeonEnvironment, R).rgb;
            vec3 phongAdditive = vec3(0.8, 0.85, 1.10) * pow(phongAmount, 3.0) * specTeam.g;
            lit = albedo * (emissive + computeLight(NdotL, shadow, 1.0, 0.6) +
                            specTeam.r * environment) +
                  phongAdditive;
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
                meshLight = computeLight(NdotL, shadow, 2.0, 1.0);
            }
            lit = albedo * (emissive + meshLight) + phongAdditive;
        } else if (pc.technique == 4u) {
            // UnitFalloffPS (Seraphim): the albedo untinted (the army's
            // colour is the lookup's row), no sun (FA leaves its shadow at
            // 0), and a falloff read by how squarely the surface faces the
            // eye, point-sampled from the mesh's lookup. Its alpha weighs
            // the environment; the albedo's alpha masks its colour in.
            vec3 environment = texture(seraphimEnvironment, R).rgb;
            vec4 fallOff = fallOffAt(pow(1.0 - clamp(dot(V, worldNormal), 0.0, 1.0), 0.6));
            vec3 phongAdditive = vec3(0.5, 0.6, 0.7) * pow(phongAmount, 9.0) * specTeam.g;
            vec3 seraphimLight = lightUbo.sunAmbience.rgb;
            seraphimLight = seraphimLight + (1.0 - seraphimLight) * lightUbo.shadowFill.rgb;
            lit = texColor.rgb * seraphimLight + environment * specTeam.r * fallOff.a +
                  phongAdditive + fallOff.rgb * texColor.a;
        } else {
            // NormalMappedPS (Unit, NormalMappedAlpha, NormalMappedGlow);
            // BlackenedNormalMappedPS's highlight is its own.
            vec3 environment = texture(environmentMap, R).rgb;
            vec3 phongAdditive = pc.technique == 14u
                                     ? vec3(pow(phongAmount, 8.0) * specTeam.g)
                                     : vec3(0.6, 0.8, 0.9) * pow(phongAmount, 2.0) * specTeam.g;
            vec3 phongMultiplicative = 2.0 * environment * specTeam.r;
            lit = albedo * (emissive + light + phongMultiplicative) + phongAdditive;
        }
    }

    // FA alpha-tests the alpha-tested techniques (NormalMappedAlpha and its
    // kin: the fraction complete times the albedo's alpha, over 0x80;
    // AlphaFade and VertexNormal: their alpha over 0x23); a unit's albedo
    // alpha is a mask its technique reads (Seraphim's glow).
    if (alphaTested && fragParameter * texColor.a <= 128.0 / 255.0) discard;
    if ((pc.technique == 11u || vertexNormal) && alpha <= 35.0 / 255.0) discard;
    outColor = vec4(lit, alpha);
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
    uint technique; // MeshTechnique (M211f)
    float time;     // FA's time, for the swaying trees (M211j)
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
layout(location = 14) in float inParameter; // the fraction complete

layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out float fragProp; // 1 for a prop (its colour's negative green)

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
    // SeraphimBuildDepthVS: a Seraphim unit's shadow grows as it's built.
    float grow = pc.technique == 8u ? 0.25 + inParameter * 0.75 : 1.0;
    vec4 skinnedPos = bone * vec4(inPosition * grow, 1.0);
    vec4 worldPos = inModel * skinnedPos;
    if (pc.technique == 17u) {
        // UndulatingDepthVS: the tree's shadow sways as it does (M211j).
        const vec3 wind = vec3(0.707, 0.0, 0.707);
        float sway = sin(0.05 * pc.time - dot(wind, inModel[3].xyz));
        worldPos.xyz += 0.003 * inPosition.y * sway * sway * wind;
    }
    gl_Position = pc.lightViewProj * worldPos;
    fragUV = inUV;
    fragProp = inColor.g < 0.0 ? 1.0 : 0.0;
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

// Mesh shadows: FA's DepthClip and UndulatingDepthClip (DepthPS(clipTest)) cut
// an alpha-tested mesh's shadow where its albedo's alpha is under a half
// (M211j): NormalMappedAlpha, BlackenedNormalMappedAlpha, VertexNormal, the
// swaying trees, and props the engine draws as NormalMappedAlpha.
const char* shadow_mesh_frag = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 lightViewProj;
    uint boneBase;
    uint bonesPerInst;
    uint technique;
    float time;
} pc;

layout(set = 1, binding = 0) uniform sampler2D texAlbedo;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in float fragProp;

void main() {
    bool clipped = fragProp > 0.5 || pc.technique == 9u || pc.technique == 14u ||
                   pc.technique == 15u || pc.technique == 17u;
    if (clipped && texture(texAlbedo, fragUV).a < 0.5) discard;
}
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
    float glowCopyScale; // ren_BloomGlowCopyScale
    float glowCopyAdd;   // the map's bloom
} pc;

// FA's CopyGlowingPS (frame.fx): what glows is what has alpha. MinimumGlow
// is 0.02; the frame is read at half size, linearly, as Moho stretches it.
void main() {
    vec4 c = texture(sceneTex, fragUV);
    float a = clamp((c.a - 0.02) * pc.glowCopyScale + pc.glowCopyAdd, 0.0, 1.0);
    outColor = c * a;
}
)glsl";

const char* bloom_blur_frag = R"glsl(
#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D inputTex;

layout(push_constant) uniform PC {
    vec2 direction;  // (1/w, 0) for horizontal, (0, 1/h) for vertical
    float blurScale; // ren_BloomBlurKernelScale
} pc;

// FA's BlurHorizontalPS / BlurVerticalPS (frame.fx): seven taps a texel
// apart, then scaled up.
void main() {
    const float weights[7] = float[](0.102734, 0.120985, 0.176033, 0.199471, 0.176033,
                                     0.120985, 0.102734);
    vec4 color = vec4(0.0);
    for (int i = 0; i < 7; i++)
        color += weights[i] * texture(inputTex, fragUV + pc.direction * float(i - 3));
    outColor = color * pc.blurScale;
}
)glsl";

const char* bloom_composite_frag = R"glsl(
#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 1, binding = 0) uniform sampler2D bloomTex;

layout(push_constant) uniform PC {
    float bloomStrength; // 1 with bloom (FA's TFrameAdd adds one to one), 0 without
} pc;

void main() {
    vec3 scene = texture(sceneTex, fragUV).rgb;
    vec3 bloom = texture(bloomTex, fragUV).rgb;
    outColor = vec4(scene + bloom * pc.bloomStrength, 1.0);
}
)glsl";

} // namespace shaders

} // namespace osc::renderer
