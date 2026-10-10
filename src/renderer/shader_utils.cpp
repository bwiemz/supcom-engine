#include "renderer/shader_utils.hpp"

#include <shaderc/shaderc.hpp>
#include <spdlog/spdlog.h>

#include <string>

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
    float terrainTime, _pad1; // TTerrainGlow's Time (M212f); padding to vec4
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

const char* terrain_skirt_frag = R"glsl(
#version 450

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.1, 0.1, 0.1, 0.0);
}
)glsl";

namespace {

// The terrain's surface (M212b): its bindings, its normal, and FA's
// terrain.fx lighting, water tint and the fog of war, shared by the
// terrain's fragment shader and the lit decals', which follow the terrain's
// normal and light as FA's DecalsPS does. It follows each shader's #version
// and push block.
const char* kTerrainSurface = R"glsl(
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

// The map's normal maps (binding 21, M212e): its tiles side by side, one
// texel a world unit; x in alpha, z in green.
layout(set = 0, binding = 21) uniform sampler2D normalMaps;
// The normal target (binding 26, M212e): the strata's tangent normal in RG
// and the map's normal in BA, drawn before the scene (read, not drawn, here).
layout(set = 0, binding = 26) uniform sampler2D terrainNormals;

// The upper stratum's albedo (binding 22): laid over the rest by its alpha
layout(set = 0, binding = 22) uniform sampler2D upperAlbedo;
// Under the water (terrain.fx's ApplyWaterColor, M213a): the map's water
// ramp, read by the water map's depth (its G).
layout(set = 0, binding = 24) uniform sampler2D waterRamp;
layout(set = 0, binding = 25) uniform sampler2D waterMap;

// Each stratum's size in world units, for its albedo and its normal map
// (FA's StratumNAlbedoTile / NormalTile: the texture repeats every `size`).
layout(set = 0, binding = 23) uniform TerrainStrata {
    vec4 albedoSize0_3;
    vec4 albedoSize4_7;
    vec4 albedoSize8_upper;   // x: stratum 8, y: the upper stratum
    vec4 normalSize0_3;
    vec4 normalSize4_7;
    vec4 normalSize8;         // x: stratum 8; yz: a normal-map tile's size (M212e)
} strata;

// Shadow map (set=1): Moho's terrain mask (binding 8, M210c)
layout(set = 1, binding = 8) uniform sampler2D shadowMask;
layout(set = 1, binding = 1) uniform LightUBO {
    mat4 lightViewProj;
    vec4 sunDirection;  // xyz toward the sun (the map's, M210a)
    vec4 sunColor;      // rgb; w: LightingMultiplier
    vec4 sunAmbience;   // rgb; w: 1 for the TTerrainXP terrain shader
    vec4 shadowFill;    // rgb: ShadowFillColor
    vec4 specularColor;
    vec4 shadow;        // x: a light camera this frame (M210c)
} lightUbo;

// terrain.fx's ComputeShadow: the blurred mask's G, bilinear, white off the
// map: where a mesh is nearest the sun. No depth comparison, so the
// terrain never shadows itself; lit with no light camera (M210c).
float calcShadow(vec3 worldPos) {
    if (lightUbo.shadow.x < 0.5) return 1.0;
    vec4 lc = lightUbo.lightViewProj * vec4(worldPos, 1.0);
    return texture(shadowMask, lc.xy / lc.w * 0.5 + 0.5).g;
}

// A stratum normal map is an ordinary tangent-space normal in RGB, z up,
// whatever its compression (DXT1, 3 or 5): FA's TerrainNormalsPS reads it
// as tex * 2 - 1. Only decal normals are DXT5nm (green, alpha).
vec3 decodeNormal(sampler2D nmap, vec2 uv) {
    return texture(nmap, uv).rgb * 2.0 - 1.0;
}

// TTerrain maps (the original game's) blend four strata, from the first
// blend texture; TTerrainXP maps all eight (FA's TerrainPS, TerrainAlbedoXP).
bool terrainXP() {
    return lightUbo.sunAmbience.w >= 0.5 && lightUbo.sunAmbience.w < 1.5;
}

// TTerrainGlow (M212f): TTerrain whose stratum 1 scrolls and glows.
bool terrainGlow() {
    return lightUbo.sunAmbience.w >= 1.5;
}

// The strata's blend weights at `blendUV` (RGBA = 4 strata each).
void terrainMasks(vec2 blendUV, out vec4 b0, out vec4 b1) {
    b0 = texture(blendMap0, blendUV);
    b1 = terrainXP() ? texture(blendMap1, blendUV) : vec4(0.0);
}

// The strata's normal at `worldXZ`, in the terrain's tangent space
// (TerrainNormalsPS, TerrainNormalsXP; M212e): each stratum's normal map at
// its own size, blended by the raw masks (a TTerrain map's b1 is 0: its
// lower stratum and the next four).
vec3 strataNormal(vec2 worldXZ, vec4 b0, vec4 b1) {
    vec3 n = decodeNormal(normalMap0, worldXZ / strata.normalSize0_3.x);
    n = mix(n, decodeNormal(normalMap1, worldXZ / strata.normalSize0_3.y), b0.r);
    n = mix(n, decodeNormal(normalMap2, worldXZ / strata.normalSize0_3.z), b0.g);
    n = mix(n, decodeNormal(normalMap3, worldXZ / strata.normalSize0_3.w), b0.b);
    n = mix(n, decodeNormal(normalMap4, worldXZ / strata.normalSize4_7.x), b0.a);
    n = mix(n, decodeNormal(normalMap5, worldXZ / strata.normalSize4_7.y), b1.r);
    n = mix(n, decodeNormal(normalMap6, worldXZ / strata.normalSize4_7.z), b1.g);
    n = mix(n, decodeNormal(normalMap7, worldXZ / strata.normalSize4_7.w), b1.b);
    n = mix(n, decodeNormal(normalMap8, worldXZ / strata.normalSize8.x), b1.a);
    return normalize(n);
}

// The world's normal at this pixel (M212e): the normal target's texel, as
// frame.fx's BasisPS composes it. The strata's normal (RG: x and z) turned
// into the basis of the map's normal (BA: x and z), y up in both.
vec3 screenNormal() {
    vec4 raw = texelFetch(terrainNormals, ivec2(gl_FragCoord.xy), 0) * 2.0 - 1.0;
    vec3 s = vec3(raw.x, sqrt(max(1.0 - raw.x * raw.x - raw.y * raw.y, 0.0)), raw.y);
    vec3 base = vec3(raw.z, sqrt(max(1.0 - raw.z * raw.z - raw.w * raw.w, 0.0)), raw.w);
    vec3 h = normalize(base + vec3(0.0, 1.0, 0.0));
    vec3 xaxis = h.x * h * vec3(-2.0, 2.0, -2.0) + vec3(1.0, 0.0, 0.0);
    vec3 zaxis = h.z * h * vec3(-2.0, 2.0, -2.0) + vec3(0.0, 0.0, 1.0);
    return vec3(dot(s, xaxis), dot(s, base), dot(s, zaxis));
}

// terrain.fx's CalculateLighting (TTerrain), by the map's light (M210a):
// specular added into the light by `specAmount` (the terrain's 1 - albedo
// alpha; a decal's specular red), which `spec` returns for the glow.
vec3 lightTTerrain(vec3 color, float specAmount, vec3 worldNormal, vec3 worldPos, vec3 eye,
                   float shadow, out float spec) {
    vec3 S = lightUbo.sunDirection.xyz;
    float SdotN = dot(S, worldNormal);
    vec3 V = normalize(worldPos - eye); // eye to point
    float multiplier = lightUbo.sunColor.w;
    vec3 fill = lightUbo.shadowFill.rgb;
    vec3 R = S - 2.0 * SdotN * worldNormal;
    spec = pow(clamp(dot(R, V), 0.0, 1.0), 80.0) * lightUbo.specularColor.x * specAmount;
    vec3 light = lightUbo.sunColor.rgb * clamp(SdotN, 0.0, 1.0) * shadow + lightUbo.sunAmbience.rgb + spec;
    light = multiplier * light + fill * (1.0 - light);
    return light * color;
}

// TTerrainXP's light (TerrainAlbedoXP, DecalAlbedoXP): specular by
// `specAmount` (the terrain's albedo alpha; a decal's specular alpha).
vec3 lightXP(vec3 color, float specAmount, vec3 worldNormal, vec3 worldPos, vec3 eye,
             float shadow) {
    vec3 S = lightUbo.sunDirection.xyz;
    float SdotN = dot(S, worldNormal);
    vec3 V = normalize(worldPos - eye); // eye to point
    float multiplier = lightUbo.sunColor.w;
    vec3 fill = lightUbo.shadowFill.rgb;
    vec3 r = reflect(V, worldNormal);
    vec3 spec = pow(clamp(dot(r, S), 0.0, 1.0), 80.0) * specAmount * lightUbo.specularColor.a * lightUbo.specularColor.rgb;
    vec3 light = lightUbo.sunColor.rgb * clamp(SdotN, 0.0, 1.0) * shadow + lightUbo.sunAmbience.rgb;
    light = multiplier * light + fill * (1.0 - light);
    return light * (color + spec);
}

// Under the water, lerped to the water ramp by depth (ApplyWaterColor).
vec3 applyWaterColor(vec3 lit, vec2 blendUV) {
    float waterDepth = texture(waterMap, blendUV).g;
    vec4 waterTint = texture(waterRamp, vec2(waterDepth, 0.5));
    return mix(lit, waterTint.rgb, waterTint.a);
}

// Fog of war: what the army doesn't see is darkened as FA's frame.fx
// Vision pass darkens it, black at alpha 0.33 over the colour (M215g).
vec3 applyFogOfWar(vec3 lit, vec2 blendUV) {
    float fogVal = texture(fogMap, blendUV).r;
    return lit * mix(0.67, 1.0, fogVal);
}
)glsl";

const char* kTerrainPush = R"glsl(#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    float mapWidth, mapHeight;
    float terrainTime, _pad1; // TTerrainGlow's Time (M212f); padding to vec4
    float eyeX, eyeY, eyeZ;
} pc;
)glsl";

const char* kTerrainFragMain = R"glsl(
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragWorldXZ;
layout(location = 2) in float fragWorldY;

layout(location = 0) out vec4 outColor;

void main() {
    // Blend map UV: world position normalized to [0,1] over map extents
    vec2 mapSize = vec2(pc.mapWidth, pc.mapHeight);
    vec2 blendUV = fragWorldXZ / mapSize;
    bool xp = terrainXP();
    vec4 b0;
    vec4 b1;
    terrainMasks(blendUV, b0, b1);

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
    // TTerrainGlow: FA's stratum 1 scrolls by 0.01 (sin, cos) of Time / 8
    // (TerrainGlowVS); its alpha at the offset swapped is the glow, and its
    // own alpha goes (TerrainGlowPS). FA numbers the strata above its lower
    // albedo from 0, the engine from 1: FA's stratum 1 (mask.y) is s2 here,
    // masked by m0.g (Varga Pass's lav_lava02).
    bool glowing = terrainGlow();
    vec2 offset = glowing ? vec2(sin(pc.terrainTime * 0.125), cos(pc.terrainTime * 0.125)) * 0.01
                          : vec2(0.0);
    vec4 s2 = texture(stratum2, uv2 + offset);
    float lava = 0.0;
    if (glowing) {
        lava = texture(stratum2, uv2 + offset.yx).a;
        s2.a = 0.0;
    }
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

    // The normal the normal pass left at this pixel (M212e).
    vec3 worldNormal = screenNormal();

    // FA's terrain lighting (terrain.fx), by the map's light (M210a).
    vec3 worldPos = vec3(fragWorldXZ.x, fragWorldY, fragWorldXZ.y);
    float shadow = calcShadow(worldPos);
    vec3 eye = vec3(pc.eyeX, pc.eyeY, pc.eyeZ);
    vec3 lit;
    // The frame's glow (M211e): TTerrain's specular, a little; XP's none.
    float glow = 0.0;
    if (!xp) {
        // TTerrain (CalculateLighting): specular where the albedo's alpha
        // is low, added into the light.
        float spec;
        lit = lightTTerrain(color, 1.0 - albedo.a, worldNormal, worldPos, eye, shadow, spec);
        // TTerrainGlow writes its stratum's glow by its mask instead
        glow = glowing ? lava * m0.g + 0.01 : 0.01 + spec * lightUbo.specularColor.w;
    } else {
        // TTerrainXP (TerrainAlbedoXP): specular from the albedo's alpha.
        lit = lightXP(color, albedo.a, worldNormal, worldPos, eye, shadow);
    }

    // Under the water (both techniques), then the fog of war. No distance
    // fog: FA's shaders have none (M210a).
    lit = applyWaterColor(lit, blendUV);
    lit = applyFogOfWar(lit, blendUV);

    outColor = vec4(lit, glow);
}
)glsl";

// The map's decals, projected and lit (M212b): terrain.fx's DecalsVS, with
// FA's decalDepthOffset as a depth bias, and DecalsPS / DecalAlbedoXP.
const char* kDecalPush = R"glsl(#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 decalU;   // the decal matrix's u column: u = dot(decalU, world, 1)
    vec4 decalV;   // its v (world z) column
    vec4 mapAlpha; // x, y: the map's size; z: DecalAlpha; w: 1 for AlbedoXP
    vec4 eye;      // xyz: the camera
} pc;
)glsl";

const char* kDecalFragMain = R"glsl(
layout(set = 2, binding = 0) uniform sampler2D decalAlbedo; // DecalAlbedoSampler: clamps
layout(set = 3, binding = 0) uniform sampler2D decalSpec;   // DecalSpecSampler: clamps
layout(set = 4, binding = 0) uniform sampler2D decalMask;   // DecalMaskSampler: clamps

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragWorldXZ;
layout(location = 2) in float fragWorldY;
layout(location = 3) in vec2 fragDecalUV;

layout(location = 0) out vec4 outColor;

// The cache's sampler repeats: clamp as FA's decal samplers do, keeping the
// lookup within the edge texels' centres.
vec4 clamped(sampler2D tex, vec2 uv) {
    vec2 edge = 0.5 / vec2(textureSize(tex, 0));
    return texture(tex, clamp(uv, edge, 1.0 - edge));
}

void main() {
    vec2 mapSize = pc.mapAlpha.xy;
    vec2 blendUV = fragWorldXZ / mapSize;
    vec4 albedo = clamped(decalAlbedo, fragDecalUV);
    vec4 specular = clamped(decalSpec, fragDecalUV);
    vec4 mask = clamped(decalMask, fragDecalUV);

    // The terrain's normal under it, from the normal target (M212e).
    vec3 worldNormal = screenNormal();
    vec3 worldPos = vec3(fragWorldXZ.x, fragWorldY, fragWorldXZ.y);
    float shadow = calcShadow(worldPos);

    vec3 lit;
    float alpha;
    if (pc.mapAlpha.w < 0.5) {
        // DecalsPS: CalculateLighting, the specular's red its amount; the
        // mask's red (.xxxx).
        float spec;
        lit = lightTTerrain(albedo.rgb, specular.r, worldNormal, worldPos, pc.eye.xyz, shadow, spec);
        alpha = albedo.a * mask.r * pc.mapAlpha.z;
    } else {
        // DecalAlbedoXP: the XP light, the specular's alpha its amount; the
        // mask's alpha.
        lit = lightXP(albedo.rgb, specular.a, worldNormal, worldPos, pc.eye.xyz, shadow);
        alpha = albedo.a * mask.a * pc.mapAlpha.z;
    }
    lit = applyWaterColor(lit, blendUV);
    lit = applyFogOfWar(lit, blendUV);
    outColor = vec4(lit, alpha);
}
)glsl";

// The low fidelity terrain (M212h): terrain.fx's LowFidelityTerrainPS, the
// lower stratum and strata 0-3 by the first mask (sharpened), unlit, then
// LowFidelityLightingPS multiplied in (its pass blends Zero / SrcColor): the
// map's normal map at the point, bilinear within its tile (x from alpha, z
// from green), the sun by it and the shadow, the ambience, the multiplier
// and the shadow fill. No upper stratum, strata 4-7, specular, screen
// normals or water tint; no glow.
const char* kTerrainLowFragMain = R"glsl(
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragWorldXZ;
layout(location = 2) in float fragWorldY;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 mapSize = vec2(pc.mapWidth, pc.mapHeight);
    vec2 blendUV = fragWorldXZ / mapSize;
    vec4 b0;
    vec4 b1;
    terrainMasks(blendUV, b0, b1);
    vec4 mask = clamp(b0 * 2.0 - 1.0, 0.0, 1.0);
    vec3 albedo = texture(stratum0, fragWorldXZ / strata.albedoSize0_3.x).rgb;
    albedo = mix(albedo, texture(stratum1, fragWorldXZ / strata.albedoSize0_3.y).rgb, mask.x);
    albedo = mix(albedo, texture(stratum2, fragWorldXZ / strata.albedoSize0_3.z).rgb, mask.y);
    albedo = mix(albedo, texture(stratum3, fragWorldXZ / strata.albedoSize0_3.w).rgb, mask.z);
    albedo = mix(albedo, texture(stratum4, fragWorldXZ / strata.albedoSize4_7.x).rgb, mask.w);

    // The map's normal map tile under the point (UtilitySamplerA, rebound
    // per tile), bilinear, kept within the tile.
    vec2 size = vec2(textureSize(normalMaps, 0));
    vec2 tile = min(strata.normalSize8.yz, size);
    vec2 origin = clamp(floor(fragWorldXZ / tile) * tile, vec2(0.0), size - tile);
    vec2 local = clamp(fragWorldXZ - origin, vec2(0.5), tile - 0.5);
    vec4 nm = texture(normalMaps, (origin + local) / size);
    vec3 normal = vec3(nm.a * 2.0 - 1.0, 0.0, nm.g * 2.0 - 1.0);
    normal.y = sqrt(max(0.0, 1.0 - normal.x * normal.x - normal.z * normal.z));

    vec3 worldPos = vec3(fragWorldXZ.x, fragWorldY, fragWorldXZ.y);
    float shadow = calcShadow(worldPos);
    vec3 light = lightUbo.sunColor.rgb *
                     clamp(dot(lightUbo.sunDirection.xyz, normal), 0.0, 1.0) * shadow +
                 lightUbo.sunAmbience.rgb;
    light = lightUbo.sunColor.w * light + lightUbo.shadowFill.rgb * (1.0 - light);
    vec3 lit = applyFogOfWar(albedo * light, blendUV);
    outColor = vec4(lit, 0.0);
}
)glsl";

// The terrain in the normal pass (M212e): its strata's normal into RG
// (TerrainNormalsPS / TerrainNormalsXP) and the map's normal into BA
// (TerrainBasisPSBiCubic), in one draw: the normal decals after it write RG
// alone, as Moho's two passes leave them.
const char* kTerrainNormalFragMain = R"glsl(
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragWorldXZ;
layout(location = 2) in float fragWorldY;

layout(location = 0) out vec4 outColor;

// The map's normal maps at `worldXZ` (TTerrainBasisBiCubic, ren_bicubicnormals
// on): a cubic B-spline from four bilinear taps (GPU Gems 2, ch. 20; Moho's
// weight table holds the B-spline's), within the tile the point is on, as
// each tile's sampler clamps. A tile's UV is (world - origin) / its size.
vec4 basisSample(vec2 worldXZ) {
    vec2 size = vec2(textureSize(normalMaps, 0));
    // No larger than the texture: its 1x1 fallback, should the maps fail to
    // load, is one tile (and the clamps below stay ordered).
    vec2 tile = min(strata.normalSize8.yz, size);
    vec2 origin = clamp(floor(worldXZ / tile) * tile, vec2(0.0), size - tile);
    vec2 coord = worldXZ - origin - 0.5; // texel centres at +0.5
    vec2 i = floor(coord);
    vec2 f = coord - i;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 g0 = w0 + w1;
    vec2 g1 = w2 + w3;
    // The two taps each way, in the tile's texels, kept within it: a tap
    // clamped to the edge texel's centre reads what CLAMP would.
    vec2 lo = vec2(0.5);
    vec2 hi = tile - 0.5;
    vec2 p0 = clamp(i - 0.5 + w1 / g0, lo, hi);
    vec2 p1 = clamp(i + 1.5 + w3 / g1, lo, hi);
    vec2 uv0 = (origin + p0) / size;
    vec2 uv1 = (origin + p1) / size;
    vec4 t00 = texture(normalMaps, vec2(uv0.x, uv0.y));
    vec4 t10 = texture(normalMaps, vec2(uv1.x, uv0.y));
    vec4 t01 = texture(normalMaps, vec2(uv0.x, uv1.y));
    vec4 t11 = texture(normalMaps, vec2(uv1.x, uv1.y));
    return g0.y * (g0.x * t00 + g1.x * t10) + g1.y * (g0.x * t01 + g1.x * t11);
}

void main() {
    vec2 mapSize = vec2(pc.mapWidth, pc.mapHeight);
    vec4 b0;
    vec4 b1;
    terrainMasks(fragWorldXZ / mapSize, b0, b1);
    vec3 n = strataNormal(fragWorldXZ, b0, b1) * 0.5 + 0.5;
    vec4 basis = basisSample(fragWorldXZ);
    outColor = vec4(n.x, n.y, basis.a, basis.g); // TerrainBasisPS's .xxwy into BA
}
)glsl";

// The normal decals in the normal pass (M212e): terrain.fx's DecalsNormalsPS
// (TDecalsNormals and TDecalsNormalsAlpha alike). The decal's normal, x and
// z from its texture's alpha and green, turned into the world by its turn
// (TangentMatrix, RotationY, as mul(M, v)), blended into RG by its
// texture's red times the mask's alpha, faded.
const char* kDecalNormalFragMain = R"glsl(
layout(set = 2, binding = 0) uniform sampler2D decalNormals; // DecalNormalSampler: clamps
layout(set = 4, binding = 0) uniform sampler2D decalMask;    // DecalMaskSampler: clamps

layout(location = 3) in vec2 fragDecalUV;

layout(location = 0) out vec4 outColor;

vec4 clamped(sampler2D tex, vec2 uv) {
    vec2 edge = 0.5 / vec2(textureSize(tex, 0));
    return texture(tex, clamp(uv, edge, 1.0 - edge));
}

void main() {
    vec4 mask = clamped(decalMask, fragDecalUV);
    vec4 raw = clamped(decalNormals, fragDecalUV);
    vec2 xz = raw.ag * 2.0 - 1.0;
    vec3 n = vec3(xz.x, sqrt(max(1.0 - dot(xz, xz), 0.0)), xz.y);
    float c = pc.turn.x;
    float s = pc.turn.y;
    n = normalize(vec3(c * n.x - s * n.z, n.y, s * n.x + c * n.z));
    n = n * 0.5 + 0.5;
    outColor = vec4(n.x, n.z, n.y, raw.r * mask.a * pc.mapAlpha.z);
}
)glsl";

// The normal decals' push block: the decals', its last vec4 the decal's
// turn (cos, sin) where the lit decals keep the eye.
const char* kDecalNormalPush = R"glsl(#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 decalU;
    vec4 decalV;
    vec4 mapAlpha; // z: DecalAlpha
    vec4 turn;     // xy: the decal's cos, sin
} pc;
)glsl";

// Glowing decals (M212d): terrain.fx's DecalsPSGlow, added into the frame's
// alpha, what glows (TDecalsGlow: One/One, alpha only): the albedo's alpha,
// times a quarter of the mask's red, faded.
const char* kDecalGlowFragMain = R"glsl(
layout(set = 2, binding = 0) uniform sampler2D decalAlbedo; // DecalAlbedoSampler: clamps
layout(set = 4, binding = 0) uniform sampler2D decalMask;   // DecalMaskSampler: clamps

layout(location = 3) in vec2 fragDecalUV;

layout(location = 0) out vec4 outColor;

vec4 clamped(sampler2D tex, vec2 uv) {
    vec2 edge = 0.5 / vec2(textureSize(tex, 0));
    return texture(tex, clamp(uv, edge, 1.0 - edge));
}

void main() {
    float glow = clamped(decalAlbedo, fragDecalUV).a;
    float mask = clamped(decalMask, fragDecalUV).r * 0.25;
    outColor = vec4(glow * mask * pc.mapAlpha.z);
}
)glsl";

// Glow-mask decals (M212d): terrain.fx's DecalsGlowMaskPS, TDecalGlowMask. Lit
// as DecalsPS without shadows, kept only where the albedo's alpha times the
// mask's (faded) is at least 0.9, and there the frame's glow set to 0.01: it
// masks what the terrain below would have glowed. No blending.
const char* kDecalGlowMaskFragMain = R"glsl(
layout(set = 2, binding = 0) uniform sampler2D decalAlbedo; // DecalAlbedoSampler: clamps
layout(set = 3, binding = 0) uniform sampler2D decalSpec;   // DecalSpecSampler: clamps
layout(set = 4, binding = 0) uniform sampler2D decalMask;   // DecalMaskSampler: clamps

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragWorldXZ;
layout(location = 2) in float fragWorldY;
layout(location = 3) in vec2 fragDecalUV;

layout(location = 0) out vec4 outColor;

vec4 clamped(sampler2D tex, vec2 uv) {
    vec2 edge = 0.5 / vec2(textureSize(tex, 0));
    return texture(tex, clamp(uv, edge, 1.0 - edge));
}

void main() {
    vec4 albedo = clamped(decalAlbedo, fragDecalUV);
    float a = clamp(albedo.a * clamped(decalMask, fragDecalUV).r * pc.mapAlpha.z, 0.0, 1.0);
    if (a < 0.9) discard; // clip(a - 0.90)
    vec2 mapSize = pc.mapAlpha.xy;
    vec2 blendUV = fragWorldXZ / mapSize;
    vec3 worldNormal = screenNormal(); // the normal target's (M212e)
    vec3 worldPos = vec3(fragWorldXZ.x, fragWorldY, fragWorldXZ.y);
    float spec;
    // DecalsGlowMaskPS(false): CalculateLighting without shadows.
    vec3 lit = lightTTerrain(albedo.rgb, clamped(decalSpec, fragDecalUV).r, worldNormal, worldPos,
                             pc.eye.xyz, 1.0, spec);
    lit = applyWaterColor(lit, blendUV);
    lit = applyFogOfWar(lit, blendUV);
    outColor = vec4(lit, 0.01);
}
)glsl";

// Runtime splats (M212c): terrain.fx's SplatsPS. CalculateLighting with no
// specular, by the terrain's normal; alpha the albedo's times the splat's
// (its LOD fade and removal fade). No mask.
const char* kSplatFragMain = R"glsl(
layout(set = 2, binding = 0) uniform sampler2D splatAlbedo; // DecalAlbedoSampler: clamps

layout(location = 1) in vec2 fragWorldXZ;
layout(location = 2) in float fragWorldY;
layout(location = 3) in vec2 fragSplatUV;
layout(location = 4) in float fragAlpha;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 mapSize = pc.mapAlpha.xy;
    vec2 blendUV = fragWorldXZ / mapSize;
    // The cache's sampler repeats: clamp as FA's decal sampler does.
    vec2 edge = 0.5 / vec2(textureSize(splatAlbedo, 0));
    vec4 albedo = texture(splatAlbedo, clamp(fragSplatUV, edge, 1.0 - edge));
    // LowFidelitySplat (M212h, eye.w set): the albedo, unlit
    if (pc.eye.w > 0.5) {
        outColor = vec4(applyFogOfWar(albedo.rgb, blendUV), albedo.a * fragAlpha);
        return;
    }

    // The terrain's normal under it, from the normal target (M212e).
    vec3 worldNormal = screenNormal();
    vec3 worldPos = vec3(fragWorldXZ.x, fragWorldY, fragWorldXZ.y);
    float shadow = calcShadow(worldPos);
    float spec;
    vec3 lit = lightTTerrain(albedo.rgb, 0.0, worldNormal, worldPos, pc.eye.xyz, shadow, spec);
    lit = applyWaterColor(lit, blendUV);
    lit = applyFogOfWar(lit, blendUV);
    outColor = vec4(lit, albedo.a * fragAlpha);
}
)glsl";

} // namespace

const char* terrain_normal_frag() {
    static const std::string source =
        std::string(kTerrainPush) + kTerrainSurface + kTerrainNormalFragMain;
    return source.c_str();
}

const char* decal_normal_frag() {
    static const std::string source = std::string(kDecalNormalPush) + kDecalNormalFragMain;
    return source.c_str();
}

const char* decal_glow_frag() {
    static const std::string source = std::string(kDecalPush) + kDecalGlowFragMain;
    return source.c_str();
}

const char* decal_glow_mask_frag() {
    static const std::string source =
        std::string(kDecalPush) + kTerrainSurface + kDecalGlowMaskFragMain;
    return source.c_str();
}

const char* splat_frag() {
    static const std::string source = std::string(kDecalPush) + kTerrainSurface + kSplatFragMain;
    return source.c_str();
}

const char* splat_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 decalU;
    vec4 decalV;
    vec4 mapAlpha;
    vec4 eye;
} pc;

// SplatsVS: a quad's corner on the terrain, its UV and its alpha. Its
// normal is the normal target's at each pixel (M212e).
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in float inAlpha;

layout(location = 1) out vec2 fragWorldXZ;
layout(location = 2) out float fragWorldY;
layout(location = 3) out vec2 fragSplatUV;
layout(location = 4) out float fragAlpha;

void main() {
    gl_Position = pc.viewProj * vec4(inPosition, 1.0);
    // Rasterizer_Cull_None_Bias_Neg001: a depth bias of -0.001.
    gl_Position.z += -0.001 * gl_Position.w;
    fragWorldXZ = inPosition.xz;
    fragWorldY = inPosition.y;
    fragSplatUV = inUV;
    fragAlpha = inAlpha;
}
)glsl";

const char* terrain_frag() {
    static const std::string source =
        std::string(kTerrainPush) + kTerrainSurface + kTerrainFragMain;
    return source.c_str();
}

const char* terrain_low_frag() {
    static const std::string source =
        std::string(kTerrainPush) + kTerrainSurface + kTerrainLowFragMain;
    return source.c_str();
}

const char* decal_lit_frag() {
    static const std::string source = std::string(kDecalPush) + kTerrainSurface + kDecalFragMain;
    return source.c_str();
}

const char* decal_lit_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 decalU;
    vec4 decalV;
    vec4 mapAlpha;
    vec4 eye;
} pc;

// The terrain's own vertices (TerrainVertex).
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragWorldXZ;
layout(location = 2) out float fragWorldY;
layout(location = 3) out vec2 fragDecalUV;

void main() {
    vec4 world = vec4(inPosition, 1.0);
    gl_Position = pc.viewProj * world;
    // Rasterizer_Bias_Decal: decalDepthOffset (-0.00001) on the depth.
    gl_Position.z += -0.00001 * gl_Position.w;
    fragNormal = inNormal;
    fragWorldXZ = inPosition.xz;
    fragWorldY = inPosition.y;
    // DecalsVS: mul(position, DecalMatrix).xz
    fragDecalUV = vec2(dot(pc.decalU, world), dot(pc.decalV, world));
}
)glsl";

// Water Albedo decals (M212g): terrain.fx's DecalsVSWaterAlbedo, the
// terrain's vertices under the water's surface lifted onto it (to 0.01
// above), with DecalsVS's bias and matrix. The surface's height in eye.w.
const char* decal_water_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 decalU;
    vec4 decalV;
    vec4 mapAlpha;
    vec4 eye; // w: the water's surface
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 3) out vec2 fragDecalUV;

void main() {
    vec4 world = vec4(inPosition, 1.0);
    if (world.y < pc.eye.w) world.y = pc.eye.w + 0.01;
    gl_Position = pc.viewProj * world;
    // Rasterizer_Bias_Decal: decalDepthOffset (-0.00001) on the depth.
    gl_Position.z += -0.00001 * gl_Position.w;
    fragDecalUV = vec2(dot(pc.decalU, world), dot(pc.decalV, world));
}
)glsl";

// DecalsPSWaterAlbedo: the albedo times LightingMultiplier, unlit and
// unshadowed, its alpha the albedo's times the mask's red, faded
// (TDecalsWaterAlbedo: SrcAlpha / InvSrcAlpha, RGB).
const char* decal_water_frag = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 decalU;
    vec4 decalV;
    vec4 mapAlpha; // z: DecalAlpha
    vec4 eye;
} pc;

layout(set = 1, binding = 1) uniform LightUBO {
    mat4 lightViewProj;
    vec4 sunDirection;
    vec4 sunColor; // w: LightingMultiplier
    vec4 sunAmbience;
    vec4 shadowFill;
    vec4 specularColor;
} lightUbo;
layout(set = 2, binding = 0) uniform sampler2D decalAlbedo; // DecalAlbedoSampler: clamps
layout(set = 4, binding = 0) uniform sampler2D decalMask;   // DecalMaskSampler: clamps

layout(location = 3) in vec2 fragDecalUV;

layout(location = 0) out vec4 outColor;

vec4 clamped(sampler2D tex, vec2 uv) {
    vec2 edge = 0.5 / vec2(textureSize(tex, 0));
    return texture(tex, clamp(uv, edge, 1.0 - edge));
}

void main() {
    vec4 albedo = clamped(decalAlbedo, fragDecalUV);
    float mask = clamped(decalMask, fragDecalUV).r;
    outColor = vec4(lightUbo.sunColor.w * albedo.rgb, albedo.a * mask * pc.mapAlpha.z);
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

// Shadow map (set=0): Moho's, point-sampled (M210c)
layout(set = 0, binding = 0) uniform sampler2D shadowMap;
layout(set = 0, binding = 1) uniform LightUBO {
    mat4 lightViewProj;
    vec4 sunDirection;  // xyz toward the sun (the map's, M210a)
    vec4 sunColor;      // rgb; w: LightingMultiplier
    vec4 sunAmbience;   // rgb; w: 1 for the TTerrainXP terrain shader
    vec4 shadowFill;    // rgb: ShadowFillColor
    vec4 specularColor;
    vec4 shadow;        // x: a light camera this frame, y: the bias (M210c)
} lightUbo;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec3 fragWorldPos;

layout(location = 0) out vec4 outColor;

// The placeholder cubes (units without meshes, the engine's own) read
// Moho's map as a Medium-lane mesh does: one point tap, the bias (M210c).
float calcShadow(vec3 worldPos) {
    if (lightUbo.shadow.x < 0.5) return 1.0;
    vec4 lc = lightUbo.lightViewProj * vec4(worldPos, 1.0);
    vec3 p = lc.xyz / lc.w;
    float depth = texture(shadowMap, p.xy * 0.5 + 0.5).r;
    return p.z > depth + lightUbo.shadow.y ? 0.0 : 1.0;
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

// FA's water (M213a): water2.fx's WaterVS, HighFidelityPS and
// TWaterLayAlphaMask, over one quad that covers the map.
const char* water_vert = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform Water {
    mat4 viewProj;
    vec4 viewPos;    // xyz the eye, w the water's elevation
    vec4 params;     // time, refraction scale, unit reflection, sky reflection
    vec4 waterColor; // rgb, a sun shininess
    vec4 lerpRange;
    vec4 repeatRate;
    vec4 move01;
    vec4 move23;
    vec4 sunDir;
    vec4 sunColor;
} u;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec2 fragLayer0;
layout(location = 2) out vec2 fragLayer1;
layout(location = 3) out vec2 fragLayer2;
layout(location = 4) out vec2 fragLayer3;
layout(location = 5) out vec3 fragViewVec;
layout(location = 6) out vec4 fragScreenPos;

void main() {
    vec3 pos = vec3(inPos.x, u.viewPos.w, inPos.z);
    gl_Position = u.viewProj * vec4(pos, 1.0);
    fragUV = inUV;
    fragScreenPos = gl_Position;
    // Four wave layers, each scrolling at its own rate (in ticks).
    float t = u.params.x;
    fragLayer0 = (pos.xz + u.move01.xy * t) * u.repeatRate.x;
    fragLayer1 = (pos.xz + u.move01.zw * t) * u.repeatRate.y;
    fragLayer2 = (pos.xz + u.move23.xy * t) * u.repeatRate.z;
    fragLayer3 = (pos.xz + u.move23.zw * t) * u.repeatRate.w;
    fragViewVec = pos - u.viewPos.xyz;
}
)glsl";

const char* water_frag = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform Water {
    mat4 viewProj;
    vec4 viewPos;
    vec4 params;
    vec4 waterColor;
    vec4 lerpRange;
    vec4 repeatRate;
    vec4 move01;
    vec4 move23;
    vec4 sunDir;
    vec4 sunColor;
} u;
layout(set = 0, binding = 1) uniform samplerCube skyMap;
layout(set = 0, binding = 2) uniform sampler2D normalMap0;
layout(set = 0, binding = 3) uniform sampler2D normalMap1;
layout(set = 0, binding = 4) uniform sampler2D normalMap2;
layout(set = 0, binding = 5) uniform sampler2D normalMap3;
layout(set = 0, binding = 6) uniform sampler2D refractionMap;
layout(set = 0, binding = 7) uniform sampler2D reflectionMap;
layout(set = 0, binding = 8) uniform sampler2D fresnelLookup;
layout(set = 0, binding = 9) uniform sampler2D waterMap; // R flatness, G depth, B land, A foam

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec2 fragLayer0;
layout(location = 2) in vec2 fragLayer1;
layout(location = 3) in vec2 fragLayer2;
layout(location = 4) in vec2 fragLayer3;
layout(location = 5) in vec3 fragViewVec;
layout(location = 6) in vec4 fragScreenPos;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 waterTexture = texture(waterMap, fragUV);
    float waterDepth = waterTexture.g;
    vec3 viewVector = normalize(fragViewVec);

    // The frame behind, at this pixel.
    float oneOverW = 1.0 / fragScreenPos.w;
    vec2 screenPos = fragScreenPos.xy * oneOverW * 0.5 + 0.5;
    vec4 background = texture(refractionMap, screenPos);
    float mask = clamp(background.a * 255.0, 0.0, 1.0);
    // Not water (its alpha test): drawn over by nothing.
    if (1.0 - mask == 0.0) discard;

    // The surface's normal: four layers summed, flattened by the map.
    vec4 sum = texture(normalMap0, fragLayer0) + texture(normalMap1, fragLayer1) +
               texture(normalMap2, fragLayer2) + texture(normalMap3, fragLayer3);
    float waveCrest = clamp(sum.a - 1.0, 0.0, 1.0);
    vec3 N = normalize((2.0 * sum.xyz - 4.0).xzy);
    N = mix(vec3(0.0, 1.0, 0.0), N, waterTexture.r);

    vec3 R = reflect(viewVector, N);
    vec4 skyReflection = texture(skyMap, R);

    // Refraction: the frame, displaced by the normal; where the displaced
    // pixel isn't water, the one behind.
    vec2 refractionPos = screenPos - u.params.y * N.xz * oneOverW;
    vec4 refracted = texture(refractionMap, refractionPos);
    refracted.rgb = mix(refracted, background, clamp(refracted.a * 255.0, 0.0, 1.0)).rgb;
    vec4 reflected = texture(reflectionMap, refractionPos);

    float NdotL = clamp(dot(-viewVector, N), 0.0, 1.0);
    float fresnel = texture(fresnelLookup, vec2(waterDepth, NdotL)).r;
    vec3 sunReflection = pow(clamp(dot(-R, u.sunDir.xyz), 0.0, 1.0), u.waterColor.a) * u.sunColor.rgb;

    reflected = mix(skyReflection, reflected, clamp(u.params.z * reflected.a, 0.0, 1.0));
    float waterLerp = clamp(waterDepth, u.lerpRange.x, u.lerpRange.y);
    refracted.rgb = mix(refracted.rgb, u.waterColor.rgb, waterLerp);
    float skyReflectionAmount = u.params.w * clamp(waterDepth * 10.0, 0.0, 1.0);
    refracted = mix(refracted, reflected, clamp(skyReflectionAmount * fresnel, 0.0, 1.0));
    refracted.rgb += sunReflection * fresnel;
    // Wave crests, where the map lets foam show.
    refracted.rgb = mix(refracted.rgb, vec3(1.0), (1.0 - waterTexture.a) * waveCrest);
    outColor = vec4(refracted.rgb, 1.0 - mask);
}
)glsl";

// Water_LowFidelity (M213d), water2.fx's at fidelity 0 and 1 (it has no
// medium technique): no refraction, reflection or sun. Pass 0
// (LowFidelityPS0): waterColorLowFi, by the water map's depth up to 0.3.
// Pass 1 (LowFidelityPS1): white where the four wave layers' alphas sum past
// waveCrestThreshold. Both SrcAlpha / InvSrcAlpha into RGB. Moho sets neither
// colour nor the threshold: water2.fx's own values.
const char* water_low_frag0 = R"glsl(
#version 450

layout(set = 0, binding = 9) uniform sampler2D waterMap; // UtilitySamplerC: G the depth

layout(location = 0) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    float alpha = clamp(texture(waterMap, fragUV).g, 0.0, 0.3);
    outColor = vec4(0.7647, 0.8784, 0.9647, alpha); // waterColorLowFi
}
)glsl";

const char* water_low_frag1 = R"glsl(
#version 450

layout(set = 0, binding = 2) uniform sampler2D normalMap0;
layout(set = 0, binding = 3) uniform sampler2D normalMap1;
layout(set = 0, binding = 4) uniform sampler2D normalMap2;
layout(set = 0, binding = 5) uniform sampler2D normalMap3;

layout(location = 1) in vec2 fragLayer0;
layout(location = 2) in vec2 fragLayer1;
layout(location = 3) in vec2 fragLayer2;
layout(location = 4) in vec2 fragLayer3;

layout(location = 0) out vec4 outColor;

void main() {
    float w = texture(normalMap0, fragLayer0).a + texture(normalMap1, fragLayer1).a +
              texture(normalMap2, fragLayer2).a + texture(normalMap3, fragLayer3).a;
    float crest = clamp(w - 1.0, 0.0, 1.0); // waveCrestThreshold 1
    outColor = vec4(1.0, 1.0, 1.0, crest);  // waveCrestColor
}
)glsl";

const char* water_mask_frag = R"glsl(
#version 450

layout(set = 0, binding = 10) uniform sampler2D waterMask; // the water map, point-sampled

layout(location = 0) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    // WaterLayAlphaMaskPS with its alpha test: alpha 0 where the water map
    // has no land.
    if (texture(waterMask, fragUV).b != 0.0) discard;
    outColor = vec4(0.0);
}
)glsl";

namespace {

// The sky's parameters (M210b), shared by its shaders.
const char* kSkyUniforms = R"glsl(#version 450
layout(set = 0, binding = 0) uniform Sky {
    mat4 viewProj;
    vec4 viewRight;      // xyz; w the time, in ticks with the frame's interpolant
    vec4 viewUp;         // xyz; w the decals' glow multiplier
    vec4 horizon;        // x where it begins, y where it ends; z the cirrus multiplier
    vec4 horizonColor;
    vec4 skyColor;
    vec4 cirrusColor;
    vec4 cirrusLayer[4]; // each layer's frequency (xy) and direction (zw)
    vec4 cirrusSpeed;    // each layer's speed
} u;
)glsl";

const char* kSkyDomeVertMain = R"glsl(
layout(location = 0) in vec3 inPos;
layout(location = 1) in float inTheta;

layout(location = 0) out float fragElevation;
layout(location = 1) out float fragTheta;
layout(location = 2) out vec4 fragCirrus01;
layout(location = 3) out vec4 fragCirrus23;

// computeCirrusCoord: the position turned into the layer's direction, and
// moved along it with time.
vec2 cirrusCoord(float time, vec2 position, int i) {
    vec2 dir = normalize(u.cirrusLayer[i].zw);
    vec2 p = vec2(dot(position, dir), position.x * dir.y - position.y * dir.x);
    return u.cirrusLayer[i].xy * (p - time * u.cirrusSpeed[i] * dir);
}

void main() {
    float time = u.viewRight.w;
    fragCirrus01 = vec4(cirrusCoord(time, inPos.xz, 0), cirrusCoord(time, inPos.xz, 1));
    fragCirrus23 = vec4(cirrusCoord(time, inPos.xz, 2), cirrusCoord(time, inPos.xz, 3));
    fragElevation = inPos.y;
    fragTheta = inTheta;
    gl_Position = u.viewProj * vec4(inPos, 1.0);
    // Never clipped by the far plane, as Moho's dome isn't (no depth test)
    gl_Position.z = 0.5 * gl_Position.w;
}
)glsl";

const char* kSkyAtmosphereFragMain = R"glsl(
layout(set = 0, binding = 1) uniform sampler2D horizonLookup; // point, clamped

layout(location = 0) in float fragElevation;
layout(location = 1) in float fragTheta;

layout(location = 0) out vec4 outColor;

const float TWO_PI = 6.283185;
const float INV_TWO_PI = 0.159155;

void main() {
    // AtmospherePS: the lookup by azimuth, times the lookup by the height
    // through the horizon, blends the horizon's colour into the sky's. Every
    // sky pass saturates what it writes, as Moho's 8-bit target stores it
    // (the scene here is half floats; a map's colours may pass 1).
    float th = INV_TWO_PI * clamp(fragTheta, 0.0, TWO_PI);
    float tv = clamp((fragElevation - u.horizon.x) / (u.horizon.y - u.horizon.x), 0.0, 1.0);
    float t = texture(horizonLookup, vec2(th, 0.25)).a * texture(horizonLookup, vec2(tv, 0.75)).a;
    outColor = vec4(clamp(mix(u.horizonColor.rgb, u.skyColor.rgb, 1.0 - t), 0.0, 1.0), 0.0);
}
)glsl";

const char* kSkyCirrusFragMain = R"glsl(
layout(set = 0, binding = 2) uniform sampler2D cirrusTexture;

layout(location = 2) in vec4 fragCirrus01;
layout(location = 3) in vec4 fragCirrus23;

layout(location = 0) out vec4 outColor;

void main() {
    // CirrusPS: each layer a channel of the texture; their product the cloud
    float c0 = texture(cirrusTexture, fragCirrus01.xy).r;
    float c1 = texture(cirrusTexture, fragCirrus01.zw).g;
    float c2 = texture(cirrusTexture, fragCirrus23.xy).b;
    float c3 = texture(cirrusTexture, fragCirrus23.zw).a;
    outColor = clamp(vec4(u.cirrusColor.rgb, u.horizon.z * c0 * c1 * c2 * c3), 0.0, 1.0);
}
)glsl";

const char* kSkyDecalVertMain = R"glsl(
layout(location = 0) in vec2 inCorner;   // the quad's
layout(location = 1) in vec4 inPosition; // the decal's: xyz, w its rotation
layout(location = 2) in vec2 inSize;
layout(location = 3) in vec4 inTexcoord; // its rectangle: u, v, width, height

layout(location = 0) out vec2 fragUV;

void main() {
    // DecalVS: a billboard, turned by the decal's rotation
    float s = sin(inPosition.w);
    float c = cos(inPosition.w);
    fragUV = inTexcoord.xy + 0.5 * inTexcoord.zw * (inCorner + vec2(1.0));
    fragUV.y = 1.0 - fragUV.y;
    vec2 corner = inSize * inCorner;
    vec2 r = vec2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    vec3 pos = inPosition.xyz + r.x * u.viewRight.xyz + r.y * u.viewUp.xyz;
    gl_Position = u.viewProj * vec4(pos, 1.0);
    gl_Position.z = 0.5 * gl_Position.w;
}
)glsl";

const char* kSkyDecalGlowFragMain = R"glsl(
layout(set = 0, binding = 4) uniform sampler2D decalGlow;

layout(location = 0) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    // DecalGlowPS: into the frame's glow alone
    outColor = vec4(0.0, 0.0, 0.0, clamp(u.viewUp.w * texture(decalGlow, fragUV).a, 0.0, 1.0));
}
)glsl";

} // namespace

const char* sky_dome_vert() {
    static const std::string source = std::string(kSkyUniforms) + kSkyDomeVertMain;
    return source.c_str();
}

const char* sky_atmosphere_frag() {
    static const std::string source = std::string(kSkyUniforms) + kSkyAtmosphereFragMain;
    return source.c_str();
}

const char* sky_cirrus_frag() {
    static const std::string source = std::string(kSkyUniforms) + kSkyCirrusFragMain;
    return source.c_str();
}

const char* sky_decal_vert() {
    static const std::string source = std::string(kSkyUniforms) + kSkyDecalVertMain;
    return source.c_str();
}

const char* sky_decal_glow_frag() {
    static const std::string source = std::string(kSkyUniforms) + kSkyDecalGlowFragMain;
    return source.c_str();
}

const char* sky_decal_albedo_frag = R"glsl(
#version 450
layout(set = 0, binding = 3) uniform sampler2D decalAlbedo;

layout(location = 0) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(decalAlbedo, fragUV); // DecalAlbedoPS
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
    uint mirrored;  // drawn into the water's reflection (M213b)
    float surface;  // the water's elevation (M213b)
    uint lane;       // mesh.fx's lane by graphics fidelity: 0 Low, 1 Medium, 2 High (M211m)
    uint shadowMode; // 0 none, 1 one tap, 2 FA's five-tap PCF (M211m)
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
layout(location = 15) in vec2 inScroll;

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
// VertexNormalVS's normal, normalised per vertex (the Low lanes', M211m)
layout(location = 9) out vec3 fragVertexNormal;

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
    vec3 local = inPosition * grow;
    if (inColor.r < 0.0 && pc.lane == 0u) {
        // WreckageVS_LowFidelity (M211m): the wreck's model-space position
        // crumpled by a random from when its mesh instance was made, its
        // uniform scale factored out, and squashed down to 0.69 of it.
        float s = length(inModel[1].xyz);
        float rdm = fract(0.01 * inShaderTime);
        local += (0.05 / s) * (cos(15.0 * rdm * local.x * s) + sin(20.0 * rdm * local.z * s));
        local.y *= mix(0.69, 1.0, rdm);
    }
    if (pc.technique == 27u) {
        // PositionNormalOffsetVS(0.05)
        local += inNormal * (0.05 / length(inModel[1].xyz));
    }
    if (pc.technique == 28u || pc.technique == 29u || pc.technique == 30u) {
        // CommandFeedbackVS(0.7 or 1.1): to that much of its size over its
        // lifetime, material.y ticks from material.x (its distance scale is
        // in the instance's matrix).
        float age = pc.time - inShaderTime;
        if (age < 0.0) age += 36000.0; // the times wrap at 36000
        float t = clamp(age / max(inParameter, 1e-4), 0.0, 1.0);
        local *= mix(1.0, pc.technique == 29u ? 1.1 : 0.7, t);
    }
    vec4 skinnedPos = bone * vec4(local, 1.0);
    vec4 worldPos = inModel * skinnedPos;
    // At Low the undulating trees take VertexNormalVS: no sway (M211m)
    if (pc.technique == 17u && pc.lane > 0u) {
        // UndulatingNormalMappedVS: swaying in FA's wind (mesh.fx's
        // windDirection, which Moho never sets), by the vertex's height in
        // its mesh, out of step by the instance's place (M211i).
        const vec3 wind = vec3(0.707, 0.0, 0.707);
        float sway = sin(0.05 * pc.time - dot(wind, inModel[3].xyz));
        worldPos.xyz += 0.003 * inPosition.y * sway * sway * wind;
    }
    if (inColor.r < 0.0 && pc.lane > 0u) {
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
    // (Unnormalised, fragNormal carries the blueprint's UniformScale.)
    fragVertexNormal = normalize(fragNormal);
    fragTangent = normalMat * inTangent;
    fragBitangent = normalMat * inBinormal;
    fragColor = inColor;
    fragColorLookup = inColorLookup;
    fragShaderTime = inShaderTime;
    fragParameter = inParameter;
    // ComputeScrolledTexcoord
    fragUV = inUV;
    if (inUV.y > 0.95) {
        fragUV.x += inScroll.x;
    } else if (inUV.y > 0.90) {
        fragUV.x += inScroll.y;
    }
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
    uint mirrored; // drawn into the water's reflection (M213b)
    float surface; // the water's elevation (M213b)
    uint lane;       // mesh.fx's lane by graphics fidelity: 0 Low, 1 Medium, 2 High (M211m)
    uint shadowMode; // 0 none, 1 one tap, 2 FA's five-tap PCF (M211m)
} pc;

layout(set = 0, binding = 0) uniform sampler2D texAlbedo;
layout(set = 2, binding = 0) uniform sampler2D texSpecTeam;
layout(set = 3, binding = 0) uniform sampler2D texNormal;

// Shadow map (set=4)
// Moho's shadow map (M210c): point-sampled and clamped (binding 0), and
// bilinear with a white border (binding 7, the PCF's)
layout(set = 4, binding = 0) uniform sampler2D shadowMap;
layout(set = 4, binding = 7) uniform sampler2D shadowPcf;
layout(set = 4, binding = 1) uniform LightUBO {
    mat4 lightViewProj;
    vec4 sunDirection;  // xyz toward the sun (the map's, M210a)
    vec4 sunColor;      // rgb; w: LightingMultiplier
    vec4 sunAmbience;   // rgb; w: 1 for the TTerrainXP terrain shader
    vec4 shadowFill;    // rgb: ShadowFillColor
    vec4 specularColor;
    vec4 shadow;        // x: a light camera this frame, y: the bias, z: the map's size (M210c)
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
layout(location = 9) in vec3 fragVertexNormal;      // normalised per vertex (the Low lanes')

layout(location = 0) out vec4 outColor;

// The sun, negated in the water's reflection, as Moho's ConfigureShader
// sets it when mirrored (M213b).
vec3 sunDirection() {
    return pc.mirrored != 0u ? -lightUbo.sunDirection.xyz : lightUbo.sunDirection.xyz;
}

float calcShadow(vec3 worldPos) {
    // The reflection is drawn with no shadow bound (M213b); the Low lane and
    // shadow fidelity 0 and 1 take none either (M211m).
    if (pc.mirrored != 0u || pc.shadowMode == 0u || lightUbo.shadow.x < 0.5) return 1.0;
    // Moho's map's R, the depth of what is nearest the sun (the terrain's
    // about 0: a mesh where the terrain is nearest is shadowed), by the lane
    // and shadow fidelity (M211m, M210c): mesh.fx's ComputeShadowStandard,
    // one point tap, shadowed past it plus the bias (the Medium lane, or High
    // without the blur); or its ComputeShadowPCF, five bilinear taps of the
    // depth, white outside, each lit while the point is nearer than it plus
    // the bias and 0.001, averaged (High at shadow fidelity 3 with
    // ren_ShadowBlur).
    vec4 lc = lightUbo.lightViewProj * vec4(worldPos, 1.0);
    vec3 p = lc.xyz / lc.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    float bias = lightUbo.shadow.y;
    if (pc.shadowMode == 1u) return p.z > texture(shadowMap, uv).r + bias ? 0.0 : 1.0;
    float ts = 1.0 / lightUbo.shadow.z;
    float d[5];
    d[0] = texture(shadowPcf, uv + vec2(-0.5 * ts, 0.0)).r;
    d[1] = texture(shadowPcf, uv + vec2(0.0, -0.5 * ts)).r;
    d[2] = texture(shadowPcf, uv + vec2(-ts, 0.0)).r;
    d[3] = texture(shadowPcf, uv + vec2(ts, 0.0)).r;
    d[4] = texture(shadowPcf, uv + vec2(0.0, ts)).r;
    float lit = 0.0;
    for (int i = 0; i < 5; ++i)
        if (d[i] + bias > p.z - 0.001) lit += 1.0;
    return lit / 5.0;
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
    vec3 S = sunDirection();
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
    if (pc.lane == 0u) {
        // The builds' Low lanes (M211n), lit by the vertex's normal, no
        // shadow, normal map or environment.
        vec3 lowLight = computeLight(dot(S, fragVertexNormal), 1.0, 1.0, 1.0);
        if (pc.technique == 5u && pc.pass == 0u) {
            // UEFBuildLoFiPS writes `float4 color = (lit colour + blue,
            // max(f, 0.5))`: HLSL's comma operator keeps the last, so FA's
            // colour is that alpha in all four channels, a flat grey.
            float s = max(f, 0.5);
            alpha = s;
            return vec3(s);
        }
        if (pc.technique == 5u) {
            // UEFBuildOverlayLoFiPS, by EffectVertexNormalLoFiVS(16, 8,
            // 0.0192, 0.0176, -0.05, -0.05): no fade at the end
            vec4 x = texture(texSecondary, fragUV * 16.0 + age * vec2(0.0192, 0.0176));
            vec4 y = texture(texSecondary, fragUV * 8.0 + age * vec2(-0.05, -0.05));
            alpha = max((x.a + y.a) * clamp(1.0 - f, 0.0, 1.0), 0.25);
            return x.rgb + y.rgb;
        }
        if (pc.technique == 6u) {
            // AeonBuild_LowFidelity: one pass of ColorMaskPS_LowFidelity
            // (AeonBuildLoFiVS(1, 1, 0, 0, 0, 0)), blended by f
            alpha = f;
            return 2.0 * lowLight * lowLight *
                   mix(fragColor.rgb, texColor.rgb, 1.0 - clamp(specTeam.a, 0.0, 1.0));
        }
        if ((pc.technique == 7u || pc.technique == 8u) && pc.pass == 0u) {
            // CybranBuildLoFiPS (SeraphimBuild_LowFidelity's too): the army's
            // colour, half and by f, under the mask; 40% opaque until 70%
            alpha = f >= 0.7 ? 0.4 + 0.6 * ((f - 0.7) * 3.33) : 0.4;
            return 2.0 * lowLight * lowLight *
                   mix(fragColor.rgb * f * 0.5, texColor.rgb, 1.0 - clamp(specTeam.a, 0.0, 1.0));
        }
        // CybranBuild's overlay is CybranBuildOverlayPS at every fidelity
    }
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
    vec3 S = sunDirection();
    float f = fragParameter;
    float age = pc.time - fragShaderTime;
    if (pc.lane == 0u && pc.technique == 12u) {
        // UEFBuildCubeLoFiPS, by EffectVertexNormalLoFiVS(0.025, 50, 0.0012,
        // 0.0062, -0.25, -0.0062) (M211n)
        vec4 albedo = texture(texAlbedo, fragUV * 0.025 + age * vec2(0.0012, 0.0062));
        vec4 secondary = texture(texSecondary, fragUV * 50.0 + age * vec2(-0.25, -0.0062));
        vec3 current = mix(albedo.rgb + secondary.rgb, vec3(0.0, 0.0, 1.0), 0.65);
        alpha = max(f, 0.5) * albedo.a;
        return mix(current, albedo.rgb, clamp(f, 0.0, 1.0));
    }
    if (pc.lane == 0u && pc.technique == 13u) {
        // AeonBuildPuddleLoFiPS, by EffectVertexNormalLoFiVS(1, 1, -0.002,
        // 0.0042, 0, 0): the albedo and a little of the sun's reflection in
        // the default cube; no glow (M211n)
        vec4 albedo = texture(texAlbedo, fragUV + age * vec2(-0.002, 0.0042));
        vec3 environment = texture(environmentMap, reflect(-S, fragVertexNormal)).rgb;
        alpha = 0.0;
        return albedo.rgb + environment * 0.15;
    }
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
)glsl" // split again: MSVC caps one literal at 16380 bytes (C2026)
                        R"glsl(

void main() {
    if (pc.technique == 28u || pc.technique == 29u || pc.technique == 30u) {
        // CommandFeedbackPS0(fade): the albedo, its alpha fading over the
        // lifetime (not RallyPoint's), tested over 0x23.
        float age = pc.time - fragShaderTime;
        if (age < 0.0) age += 36000.0;
        float t = pc.technique == 30u ? 0.0 : clamp(age / max(fragParameter, 1e-4), 0.0, 1.0);
        vec4 feedback = texture(texAlbedo, fragUV);
        float alpha = clamp(feedback.a * (1.0 - t), 0.0, 1.0);
        if (alpha <= 35.0 / 255.0) discard;
        outColor = vec4(feedback.rgb, alpha);
        return;
    }
    vec3 worldNormal = computeNormal(fragUV);
    vec3 S = sunDirection();
    if (pc.technique == 27u) {
        outColor = vec4(clamp(fragColor.rgb * computeLight(dot(S, fragVertexNormal), 1.0, 1.0, 1.0),
                              0.0, 1.0),
                        0.2);
        return;
    }
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
    // The blended techniques (the builds, AlphaFade, UEFBuildCube,
    // VertexNormal) and the build ghost's fade: FA drew them into an 8-bit
    // target, which clamps what a shader writes before it blends (the scene
    // here is half floats).
    bool blended = (pc.technique >= 5u && pc.technique <= 8u) || pc.technique == 11u ||
                   pc.technique == 12u || vertexNormal || fragColor.a < 1.0;
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
    // The Low lane (graphics fidelity 0, M211m): mesh.fx's low fidelity
    // shaders, lit by the vertex's normal (normalised per vertex, interpolated
    // and not renormalised, as FA's are), with no shadow, normal map,
    // environment or glow. The build techniques (5-8, 12, 13) take theirs in
    // buildColor and effectColor (M211n), and NormalMappedTerrain has no
    // lanes.
    bool lowLane = pc.lane == 0u && !(pc.technique >= 5u && pc.technique <= 8u) &&
                   pc.technique != 12u && pc.technique != 13u && !terrainProp;
    if (lowLane) {
        vec3 lowLight = computeLight(dot(S, fragVertexNormal), 1.0, 1.0, 1.0);
        vec3 lowLit;
        float lowAlpha = 0.0; // written RGB only: no glow
        if (fragColor.r < 0.0) {
            // WreckagePS_LowFidelity: the crunch tiled 10 times. Its alpha
            // (f) is left out: Wreckage_LowFidelity sets no alpha state, so
            // whether FA writes it depends on the draw before (a Low unit's
            // leaves alpha unwritten); no glow, as the other Low lanes.
            vec4 crunch = texture(texSpecTeam, fragUV * 10.0);
            lowLit = texColor.rgb * lowLight * (texColor.rgb + crunch.r + crunch.a) * crunch.b * 5.5;
        } else if (pc.technique == 4u) {
            // LowFiUnitFalloffPS (Seraphim): its colour from the specular's
            // green and red, the army's by the green, a highlight, and
            // ComputeLight's attenuation the sun direction's x (FA passes the
            // float3 where a float goes)
            vec3 diffuse = specTeam.ggg * specTeam.rrr * (1.0 - texColor.rgb);
            diffuse = mix(tint, diffuse, specTeam.g);
            vec3 n = normalize(fragNormal);
            vec3 reflected = reflect(-faViewDirection(fragWorldPos), n);
            float highlight = pow(clamp(dot(reflected, S), 0.0, 1.0), 5.0);
            lowLit = diffuse * computeLight(dot(S, n), S.x, 1.0, 1.0) + vec3(highlight);
        } else if (pc.technique <= 3u) {
            // ColorMaskPS_LowFidelity (Unit, Aeon, Insect, Metal): the army's
            // colour by the specular's alpha
            vec3 albedo = mix(tint, texColor.rgb, 1.0 - clamp(specTeam.a, 0.0, 1.0));
            lowLit = 2.0 * lowLight * lowLight * albedo;
        } else if (pc.technique == 14u) {
            // BlackenedLoFiPS
            lowLit = vec3(dot(texColor.rgb, vec3(0.1))) * lowLight;
        } else if (pc.technique == 11u) {
            // AlphaFadeLoFiPS
            float age = pc.time - fragShaderTime;
            lowLit = texColor.rgb * lowLight;
            lowAlpha = texColor.a * fragParameter * clamp(1.0 - (age - 2.0) * 0.145, 0.0, 1.0);
        } else {
            // VertexNormalPS_LowFidelity (NormalMappedAlpha, NormalMappedGlow,
            // VertexNormal, UndulatingNormalMappedAlpha): untinted; the glow's
            // and VertexNormal's alpha f times the albedo's
            lowLit = 2.0 * lowLight * lowLight * texColor.rgb;
            if (pc.technique == 10u || vertexNormal) lowAlpha = texColor.a * fragParameter;
        }
        if (alphaTested && fragParameter * texColor.a <= 128.0 / 255.0) discard;
        if ((pc.technique == 11u || vertexNormal) && lowAlpha <= 35.0 / 255.0) discard;
        if (fragColor.a < 1.0) lowAlpha = fragColor.a; // the build ghost's fade
        outColor = vec4(blended ? clamp(lowLit, 0.0, 1.0) : lowLit, lowAlpha);
        return;
    }

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
            // Aeon_Med names no environment: the "<default>" cube (M211m)
            vec3 environment = pc.lane >= 2u ? texture(aeonEnvironment, R).rgb
                                             : texture(environmentMap, R).rgb;
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
    // The reflection (mesh.fx when mirrored, M213b): nothing under the
    // water, at half alpha.
    if (pc.mirrored != 0u) {
        if (fragWorldPos.y < pc.surface) discard;
        alpha = 0.5;
    }
    outColor = vec4(blended ? clamp(lit, 0.0, 1.0) : lit, alpha);
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
    uint lane;      // mesh.fx's lane by graphics fidelity: 0 Low (M211n)
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
    fragUV = inUV;
    fragProp = inColor.g < 0.0 ? 1.0 : 0.0;
    if (pc.lane == 0u && inColor.r < 0.0) {
        // Wreckage_LowFidelity has no depth stage: at Low a wreck (its
        // colour's negative red) casts no shadow. Outside the clip volume.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
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

// The terrain in Moho's shadow map (TTerrainDepth, M210c): FA's vertex
// shader scales the whole homogeneous position, w too, by HeightScale
// (1/128), and its pixel shader writes the undivided z, so the terrain's R is
// its light depth over 128 (about 0). G 1: no mesh is nearest the sun here.
const char* shadow_terrain_frag = R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(gl_FragCoord.z / 128.0, 1.0, 0.0, 1.0);
}
)glsl";

// A caster in Moho's shadow map (DepthPS, M210c): R its light depth, G 0.
const char* shadow_caster_frag = R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
}
)glsl";

// Moho's shadow blur (M210c), its passes as D3D9 runs them: a pixel samples
// at its texel's edge (uv = i / N), so the point taps are whole texels and
// the bilinear ones average two columns. The taps are fetched outright.
// THorizontalBlurDepthToVariance: G at i-2..i+2, [1 4 6 4 1] / 16.
const char* shadow_blur_h_frag = R"glsl(
#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 outColor;
float g(ivec2 p) {
    return texelFetch(src, clamp(p, ivec2(0), textureSize(src, 0) - 1), 0).g;
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float z = (g(p + ivec2(-2, 0)) + 4.0 * g(p + ivec2(-1, 0)) + 6.0 * g(p) +
               4.0 * g(p + ivec2(1, 0)) + g(p + ivec2(2, 0))) / 16.0;
    outColor = vec4(z, z, 0.0, 0.0);
}
)glsl";

// TVerticalBlurDepthToVariance: four bilinear taps of G at v + 1.5, 0.5,
// -0.5 and -1.5 texels, {2 6 6 2} / 16: rows j+1..j-2, each the mean of
// columns i-1 and i.
const char* shadow_blur_v_frag = R"glsl(
#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 outColor;
float g(ivec2 p) {
    return texelFetch(src, clamp(p, ivec2(0), textureSize(src, 0) - 1), 0).g;
}
float pair(ivec2 p) { return 0.5 * (g(p + ivec2(-1, 0)) + g(p)); }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float z = (2.0 * pair(p + ivec2(0, 1)) + 6.0 * pair(p) + 6.0 * pair(p + ivec2(0, -1)) +
               2.0 * pair(p + ivec2(0, -2))) / 16.0;
    outColor = vec4(z, z, 0.0, 0.0);
}
)glsl";

// With ren_ShadowBlur off the terrain reads the map's own G: copied whole.
const char* shadow_copy_frag = R"glsl(
#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 outColor;
void main() {
    float z = texelFetch(src, ivec2(gl_FragCoord.xy), 0).g;
    outColor = vec4(z, z, 0.0, 0.0);
}
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
    uint lane;
} pc;

layout(set = 1, binding = 0) uniform sampler2D texAlbedo;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in float fragProp;

layout(location = 0) out vec4 outColor;

void main() {
    bool clipped = fragProp > 0.5 || pc.technique == 9u || pc.technique == 14u ||
                   pc.technique == 15u || pc.technique == 17u;
    if (clipped && texture(texAlbedo, fragUV).a < 0.5) discard;
    // Moho's map (M210c): R the light depth, G 0, a mesh nearest the sun.
    outColor = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
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
    if (fragColor.a < 0.0) {
        // A negative alpha asks for FA's StrategicIconPS (M215c): the
        // icon's grey texels take the tint (its army's colour), the rest
        // (outlines, the selected ring) keep their own.
        vec3 d = texColor.rgb - vec3(0.5);
        if (dot(d, d) < 0.25) texColor.rgb = fragColor.rgb;
        outColor = texColor;
    } else {
        outColor = texColor * fragColor;
    }
    if (outColor.a < 0.01) discard;
}
)glsl";

const char* resource_icon_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    vec2 viewport;
    float time;
    float glow;
} pc;

layout(location = 0) in vec4 inRect;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out float fragRadius;

void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1),
                                   vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 pos = corners[gl_VertexIndex];
    vec2 pixel = inRect.xy + pos * inRect.zw;
    gl_Position = vec4(pixel / pc.viewport * 2.0 - 1.0, 0.0, 1.0);
    fragUV = pos;
    fragRadius = 0.00277 * length(pixel);
}
)glsl";

const char* resource_icon_frag = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    vec2 viewport;
    float time;
    float glow;
} pc;

layout(set = 0, binding = 0) uniform sampler2D texSampler;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in float fragRadius;

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 size = textureSize(texSampler, 0);
    vec4 color = texelFetch(texSampler, clamp(ivec2(fragUV * vec2(size)), ivec2(0), size - 1), 0);
    if (pc.glow > 0.5) {
        float sine = sin(pc.time - 10.0 * fragRadius);
        color.a *= 0.6 * sine * sine;
    }
    if (color.a <= 0.0) {
        discard;
    }
    outColor = color;
}
)glsl";

// ---------------------------------------------------------------------------
// Beam strips (M214a): particle.fx's BeamVS/BeamPS, the strip built on the CPU
// ---------------------------------------------------------------------------

const char* beam_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    fragUV = inUV;
    fragColor = inColor;
}
)glsl";

const char* beam_frag = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform sampler2D texBeam; // wraps

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(texBeam, fragUV) * fragColor;
}
)glsl";

// ---------------------------------------------------------------------------
// FA's trails (M214b): particle.fx's TrailVS/TrailPS. The ribbon is built on
// the CPU; each vertex carries its end's age fraction t, V across the ribbon
// and the distance coordinate.
// ---------------------------------------------------------------------------

const char* trail_vert = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inTVU; // t, V across, distance coordinate

layout(location = 0) out vec3 fragTVU;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    fragTVU = inTVU;
}
)glsl";

const char* trail_frag = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform sampler2D texRamp;   // ParticleSampler1: clamps
layout(set = 1, binding = 0) uniform sampler2D texRepeat; // ParticleSampler0Wrap: wraps

layout(location = 0) in vec3 fragTVU;

layout(location = 0) out vec4 outColor;

void main() {
    // TrailPS: nothing outside the trail's life (its unborn head, its
    // spent tail).
    float t = fragTVU.x;
    if (t <= 0.0 || t >= 1.0) discard;
    // The ramp clamps: keep its lookup within its edge texels' centres
    // (the cache's sampler repeats).
    vec2 halfTexel = 0.5 / vec2(textureSize(texRamp, 0));
    vec2 rampUV = clamp(vec2(t, fragTVU.y), halfTexel, 1.0 - halfTexel);
    outColor = texture(texRamp, rampUV) * texture(texRepeat, fragTVU.yz);
}
)glsl";

// ---------------------------------------------------------------------------
// Particle billboard shaders
// ---------------------------------------------------------------------------

const char* particle_vert = R"glsl(
#version 450

// FA's particles (M214c): particle.fx's WorldVS, its motion found on the
// CPU; each instance is a quad's centre and the axes its corners span.
layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

layout(location = 0) in vec3 inCenter;
layout(location = 1) in vec3 inAxisX;
layout(location = 2) in vec3 inAxisY;
layout(location = 3) in vec4 inUV;   // u offset, u span, v offset, v span
layout(location = 4) in vec2 inRamp; // age / lifetime, ramp selection

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec2 fragRamp;
layout(location = 2) out vec2 fragScreen; // WorldVS's mTex2: where the vertex is on screen

void main() {
    // 6 vertices per quad (2 triangles), corners at (+-1, +-1).
    const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1),
                                   vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 corner = corners[gl_VertexIndex];
    gl_Position = pc.viewProj * vec4(inCenter + corner.x * inAxisX + corner.y * inAxisY, 1.0);
    // WorldVS: (corner + 1) / 2, in the frame and strip it shows.
    fragUV = vec2((corner.x + 1.0) * 0.5 * inUV.y + inUV.x, (corner.y + 1.0) * 0.5 * inUV.w + inUV.z);
    fragRamp = inRamp;
    // Found at the vertex, as WorldVS does, and interpolated across the quad
    // (for the refracting ones, M214d).
    fragScreen = 0.5 * gl_Position.xy / gl_Position.w + 0.5;
}
)glsl";

const char* particle_frag = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform sampler2D texParticle; // ParticleSampler0: U wraps, V clamps
layout(set = 1, binding = 0) uniform sampler2D texRamp;     // ParticleSampler1: clamps

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec2 fragRamp;

layout(location = 0) out vec4 outColor;

void main() {
    // The cache's sampler repeats: clamp by keeping lookups within the edge
    // texels' centres.
    vec2 texHalf = 0.5 / vec2(textureSize(texParticle, 0));
    vec2 uv = vec2(fragUV.x, clamp(fragUV.y, texHalf.y, 1.0 - texHalf.y));
    vec2 rampHalf = 0.5 / vec2(textureSize(texRamp, 0));
    vec2 rampUV = clamp(fragRamp, rampHalf, 1.0 - rampHalf);
    // WorldPS: the texture times its ramp.
    outColor = texture(texParticle, uv) * texture(texRamp, rampUV);
}
)glsl";

// FA's refracting particles (M214d): particle.fx's WorldRefractPS, the frame
// drawn before them, displaced by their texture's red and green, blended by
// its alpha times their ramp's.
const char* particle_refract_frag = R"glsl(
#version 450

layout(set = 0, binding = 0) uniform sampler2D texParticle; // ParticleSampler0: U wraps, V clamps
layout(set = 1, binding = 0) uniform sampler2D texRamp;     // ParticleSampler1: clamps
layout(set = 2, binding = 0) uniform sampler2D background;  // BackgroundSampler: the frame, copied

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec2 fragRamp;
layout(location = 2) in vec2 fragScreen;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 texHalf = 0.5 / vec2(textureSize(texParticle, 0));
    vec2 uv = vec2(fragUV.x, clamp(fragUV.y, texHalf.y, 1.0 - texHalf.y));
    vec2 rampHalf = 0.5 / vec2(textureSize(texRamp, 0));
    vec2 rampUV = clamp(fragRamp, rampHalf, 1.0 - rampHalf);
    vec4 texel = texture(texParticle, uv);
    vec2 offset = 0.005 * (2.0 * texel.rg - 1.0);
    outColor = vec4(texture(background, fragScreen + offset).rgb,
                    texel.a * texture(texRamp, rampUV).a);
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
