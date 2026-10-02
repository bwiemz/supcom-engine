// The shields' shaders (M211k): mesh.fx's shield techniques, drawn with the
// mesh pipelines' vertex input, push block and descriptor sets.
//
//   18 ShieldUEF          FourUVTexShiftScaleVS, ShieldPS
//   19 ShieldCybran       FourUVTexShiftScaleVS, then ShieldPositionNormalOffsetVS;
//                         ShieldCybranPS(0.17) both
//   20 ShieldAeon         ShieldNormalVS, ShieldAeonPS
//   21 ShieldSeraphim     ShieldNormalVS, ShieldSeraphimPS
//   22 ShieldFill         FlatVS, ShieldFillPS (depth alone)
//   23 ShieldImpact       ShieldImpactVS, ShieldImpactPS(2, 0.2)
//   24 CybranShieldImpact ShieldImpactVS, CybranShieldImpactPS(6, 0.15, 4.5)
//   25 PhaseShield        P1: PositionNormalOffsetVS(0.05), PhaseShieldPS (M211l)
//   26 SeraphimPersonalShield  P1: the same, SeraphimPhaseShieldPS
//
// (A personal shield's P0, the unit itself, draws with the mesh shaders.)
//
// Each technique's medium fidelity variant, which High draws too, and its
// low fidelity one at graphics fidelity 0 (M211n). Shields are never in the
// water's reflection (Moho reflects only units), so the shaders' `mirrored`
// clip never applies.

#include "renderer/shader_utils.hpp"

#include <string>

namespace osc::renderer::shaders {

namespace {

const char* kShieldPushBlock = R"glsl(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    uint boneBase;
    uint bonesPerInst;
    float eyeX, eyeY, eyeZ;
    uint technique; // MeshTechnique: the shields' 18 to 24 (M211k)
    uint pass;      // ShieldCybran's second pass: 1
    float time;     // FA's time: the newest tick plus the interpolant, wrapped
    uint mirrored;
    float surface;
    uint lane;       // mesh.fx's lane by graphics fidelity: 0 Low (M211n)
    uint shadowMode; // unused here
} pc;
)glsl";

const char* kShieldVertMain = R"glsl(
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 8) in uvec4 inBoneIndices;
layout(location = 9) in vec4 inBoneWeights;
layout(location = 10) in vec3 inTangent;
layout(location = 11) in vec3 inBinormal;
layout(location = 12) in float inColorLookup;
layout(location = 13) in float inShaderTime; // material.x: the tick the instance was made
layout(location = 14) in float inParameter;  // material.y: the shield's health
layout(location = 3) in mat4 inModel;
layout(location = 7) in vec4 inColor;

layout(std430, set = 1, binding = 0) readonly buffer BoneBuffer {
    mat4 bones[];
} boneSSBO;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec4 fragTex0;
layout(location = 2) out vec4 fragTex1;
layout(location = 3) out vec2 fragTex2;
layout(location = 4) out vec3 fragTangent;
layout(location = 5) out vec3 fragBinormal;
layout(location = 6) out vec3 fragWorldPos;
layout(location = 7) flat out vec2 fragMaterial; // age in ticks, the parameter

// A technique's texture scales and scrolls (its VS's uniforms): texcoord0.xy,
// .zw, texcoord1.xy, .zw scaled by `scale`, then moved by the age times
// `shift0` (texcoord0) and `shift1` (texcoord1).
struct Layers {
    vec4 scale;
    vec4 shift0;
    vec4 shift1;
};

Layers layers() {
    // The Low lanes' ThreeUVTexShiftScaleLoFiVS (M211n): texcoord0.xy, .zw
    // and texcoord1.xy, each scaled and moved by the age
    if (pc.lane == 0u && pc.technique == 18u) // ShieldUEF_LowFidelity
        return Layers(vec4(1, 3, 32, 1), vec4(0, 0, 0.0003, 0.005), vec4(-0.001, -0.005, 0, 0));
    if (pc.lane == 0u && pc.technique == 19u) // ShieldCybran_LowFidelity
        return Layers(vec4(1, 2, 1, 1), vec4(0, 0, 0, 0.002), vec4(0.001, -0.003, 0, 0));
    if (pc.lane == 0u && (pc.technique == 20u || pc.technique == 21u)) // Aeon's, Seraphim's
        return Layers(vec4(1, 12, 8, 1), vec4(0, 0, 0, 0.032), vec4(0.012, -0.032, 0, 0));
    if (pc.technique == 18u) // ShieldUEF_MedFidelity: FourUVTexShiftScaleVS
        return Layers(vec4(1, 3, 32, 6), vec4(0, 0, 0.0003, 0.005),
                      vec4(-0.001, -0.005, -0.0003, -0.0008));
    if (pc.technique == 19u && pc.pass == 0u) // ShieldCybran P0: FourUVTexShiftScaleVS
        return Layers(vec4(1, 1, 2, 1), vec4(-0.01, 0, -0.002, 0), vec4(0, 0.0012, 0.001, -0.0015));
    if (pc.technique == 19u) // ShieldCybran P1: ShieldPositionNormalOffsetVS
        return Layers(vec4(1, 1, 4, 1), vec4(0.01, 0, -0.002, 0), vec4(0, 0.0012, 0.001, -0.003));
    if (pc.technique == 20u) // ShieldAeon_MedFidelity: ShieldNormalVS
        return Layers(vec4(1, 12, 8, 3), vec4(0, 0, 0, 0.032), vec4(0.012, -0.032, 0, 0.0012));
    // ShieldSeraphim_MedFidelity: ShieldNormalVS
    return Layers(vec4(5, 1, 1, 11), vec4(-0.00153, -0.0159, 0, 0),
                  vec4(0.003, -0.0045, -0.005, -0.045));
}

void main() {
    mat4 bone = mat4(1.0);
    if (pc.bonesPerInst > 0u) {
        uint base = pc.boneBase + uint(gl_InstanceIndex) * pc.bonesPerInst;
        bone = boneSSBO.bones[base + inBoneIndices[0]]; // FA skins by boneIndex[0]
    }
    // time - material.x: the instance's age, in ticks
    float age = pc.time - inShaderTime;
    vec3 position = inPosition;
    // ShieldPositionNormalOffsetVS(0.01): Cybran's second shell, pushed out
    // along the normal (over the bone's scale, 1 for the shield's sphere)
    if (pc.technique == 19u && pc.pass == 1u) position += inNormal * 0.01;
    // PositionNormalOffsetVS(0.05), a personal shield's shell (M211l): over
    // the palette's scale, which for a unit (skinned) is its own: 0.05 out
    // in the world whatever its size
    if (pc.technique == 25u || pc.technique == 26u)
        position += inNormal * (0.05 / max(length(inModel[0].xyz), 1e-6));
    vec4 worldPos = inModel * (bone * vec4(position, 1.0));
    gl_Position = pc.viewProj * worldPos;
    fragWorldPos = worldPos.xyz;
    mat3 normalMat = mat3(inModel) * mat3(bone);
    fragNormal = normalMat * inNormal;
    fragTangent = normalMat * inTangent;
    fragBinormal = normalMat * inBinormal;
    fragMaterial = vec2(age, inParameter);

    // texcoord0 is the vertex's two uv sets (the shields' are the same)
    vec4 uv = vec4(inUV, inUV);
    fragTex2 = inUV;
    if (pc.technique == 23u || pc.technique == 24u) {
        // ShieldImpactVS: ShieldImpact's (-0.003, -0.1, -0.085, -0.15, 0.25,
        // 0, 0, 0, 2) or CybranShieldImpact's (0, 0, 0, 0, 1, -0.003, -0.06,
        // -0.01, 1)
        bool cybran = pc.technique == 24u;
        float t0x = cybran ? 0.0 : -0.003;
        float t0yb = cybran ? 0.0 : -0.1;
        float t0ye = cybran ? 0.0 : -0.085;
        float t0yo = cybran ? 0.0 : -0.15;
        float t1s = cybran ? 1.0 : 0.25;
        float t1x = cybran ? -0.003 : 0.0;
        float t1y = cybran ? -0.06 : 0.0;
        float t2x = cybran ? -0.01 : 0.0;
        float fadeTime = cybran ? 1.0 : 2.0;
        fragTex0 = uv;
        fragTex0.x += t0x * age;
        fragTex0.y += t0yo + mix(t0yb, t0ye, age / fadeTime) * age;
        fragTex1 = uv;
        fragTex1.xy *= t1s;
        fragTex1.x += t1x * age;
        fragTex1.y += t1y * age;
        // Out of step by the tick it was made (material.x itself)
        fragTex2.x += fract(t2x * inShaderTime);
        return;
    }
    if (pc.technique == 22u || pc.technique == 25u || pc.technique == 26u) {
        // FlatVS: position alone; PositionNormalOffsetVS: texcoord0 as it is
        fragTex0 = uv;
        fragTex1 = uv;
        return;
    }
    Layers l = layers();
    fragTex0 = uv * vec4(l.scale.xx, l.scale.yy) + age * l.shift0;
    fragTex1 = uv * vec4(l.scale.zz, l.scale.ww) + age * l.shift1;
}
)glsl";

const char* kShieldFragMain = R"glsl(
layout(set = 0, binding = 0) uniform sampler2D albedoSampler;
layout(set = 2, binding = 0) uniform sampler2D specularSampler;
layout(set = 3, binding = 0) uniform sampler2D normalsSampler;
layout(set = 4, binding = 2) uniform samplerCube environmentSampler; // "<default>"
layout(set = 5, binding = 0) uniform sampler2D lookupSampler;
layout(set = 6, binding = 0) uniform sampler2D secondarySampler;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragTex0;
layout(location = 2) in vec4 fragTex1;
layout(location = 3) in vec2 fragTex2;
layout(location = 4) in vec3 fragTangent;
layout(location = 5) in vec3 fragBinormal;
layout(location = 6) in vec3 fragWorldPos;
layout(location = 7) flat in vec2 fragMaterial;

layout(location = 0) out vec4 outColor;

const float PI_FA = 3.14; // mesh.fx's

// FA's viewDirection: the point's device position turned into the world by
// the view's rotation (as mesh.frag's faViewDirection).
vec3 faViewDirection(vec3 worldPos) {
    vec4 clip = pc.viewProj * vec4(worldPos, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    mat4 m = pc.viewProj;
    vec3 right = normalize(vec3(m[0][0], m[1][0], m[2][0]));
    vec3 upDevice = normalize(vec3(m[0][1], m[1][1], m[2][1]));
    vec3 back = -normalize(vec3(m[0][3], m[1][3], m[2][3]));
    return normalize(ndc.x * right + ndc.y * upDevice + ndc.z * back);
}

// FA's ComputeNormal: green along the binormal, alpha along the tangent.
vec3 computeNormal(vec2 uv) {
    vec4 nmap = texture(normalsSampler, uv);
    vec3 n = vec3(nmap.g * 2.0 - 1.0, nmap.a * 2.0 - 1.0, 0.0);
    n.z = sqrt(max(0.0, 1.0 - n.x * n.x - n.y * n.y));
    return normalize(mat3(fragBinormal, fragTangent, fragNormal) * n);
}

// A pulse FA's shaders take from a clock: sin(frac(rate * t) * 3.14)
float pulse(float rate, float t) {
    return sin(fract(rate * t) * PI_FA);
}

vec4 shieldUEF(float health) { // ShieldPS
    vec4 colorMask = texture(albedoSampler, fragTex0.xy);
    vec4 albedo = texture(albedoSampler, fragTex0.zw);
    vec3 normal = vec3(texture(secondarySampler, fragTex1.xy).ga * 2.0 - 1.0, 0.0);
    normal.z = sqrt(max(0.0, 1.0 - normal.x * normal.x - normal.y * normal.y));
    vec3 specular = texture(specularSampler, fragTex1.zw).rgb;
    // mul(albedo.rgr, normal.rgb): two vectors, so their dot product
    vec4 color = vec4(vec3(dot(albedo.rgr, normal)) + vec3(0.0, 0.0, 0.25), 1.0);
    // Three layers of noise pick the alpha
    if (specular.g <= albedo.r) {
        if (specular.b >= albedo.g)
            color.a = color.b >= normal.b ? 0.12 : mix(0.05, 0.0, pulse(0.01, pc.time));
        else
            color.a = normal.b >= albedo.r ? 0.2 : mix(0.01, 0.1, pulse(0.01, pc.time));
    } else {
        if (specular.r >= albedo.r)
            color.a = specular.b >= albedo.r ? 0.025 : 0.1;
        else
            color.a = specular.g >= albedo.g ? 0.02 : mix(0.37, 0.46, pulse(0.01, pc.time));
    }
    color.rgb += vec3(0.0, 0.0, 0.15);
    // Redder as its health falls
    vec4 colorMod1 = mix(vec4(0.5, 0.0, 0.0, 0.05), color, 0.5);
    colorMod1 = mix(color, colorMod1 + color, pulse(0.06, pc.time));
    color = mix(colorMod1, color, health);
    color += colorMask.b * 0.95;
    color.a *= colorMask.a; // masks the pinch at the sphere's top
    return color;
}

// The Low lanes (M211n)
vec4 shieldLoFi() { // ShieldLoFiPS (UEF)
    vec4 colorMask = texture(albedoSampler, fragTex0.xy);
    vec4 albedo = texture(albedoSampler, fragTex0.zw);
    vec4 secondary = texture(secondarySampler, fragTex1.xy);
    vec4 specular = texture(specularSampler, fragTex1.xy);
    vec3 color = vec3(0.0, 0.0, 0.3) + albedo.r + secondary.b;
    float alpha = colorMask.b * 0.7 + (specular.g <= albedo.g ? 0.2 : 0.1);
    color += vec3(0.0, 0.0, 0.15) + colorMask.bbb;
    return vec4(color, alpha * colorMask.a);
}

vec4 shieldCybranLoFi() { // ShieldCybranLoFiPS(0.24): one pass
    vec4 albedo = texture(albedoSampler, fragTex0.xy);
    vec4 albedo2 = texture(albedoSampler, fragTex0.zw);
    vec3 specular = texture(specularSampler, fragTex1.xy).rgb;
    return vec4(vec3(0.2, 0.0, 0.5) * specular.b, 0.24 + (albedo.r + albedo2.r) * 0.8);
}

vec4 shieldAeonLoFi() { // ShieldAeonLoFiPS(0.5): Aeon's, and Seraphim's at Low
    const float a = 0.5;
    vec4 albedo = texture(albedoSampler, fragTex0.xy);
    vec4 specular = texture(specularSampler, fragTex0.zw);
    vec3 specular2 = texture(specularSampler, fragTex1.xy).rgb;
    vec3 color = (vec3(dot(albedo.rgb * a, vec3(0.6))) + albedo.rgb * a) *
                 (specular.rrr * 1.45 + specular2.rrr * 2.2);
    return vec4(color, 0.33 * a * albedo.a);
}

vec4 shieldCybran(float age, float health) { // ShieldCybranPS(0.17)
    vec4 albedo = texture(albedoSampler, fragTex0.xy);
    vec4 albedo2 = texture(albedoSampler, fragTex0.zw);
    vec3 specular = texture(specularSampler, fragTex1.xy).rgb;
    vec3 specular2 = texture(specularSampler, fragTex1.zw).rgb;
    vec3 color2 = vec3(albedo2.b * specular2.g * 3.0);
    vec3 color3 = vec3(specular2.g * albedo.a);
    vec3 color4 = vec3((albedo2.g - specular2.b) * specular.b) * albedo.a;
    vec3 finalColor = vec3(0.05, 0.0, 0.3) + color4 - color2 * color3;
    vec3 colorMod1 = mix(vec3(0.2, 0.0, 0.0), finalColor, 0.5);
    colorMod1 = mix(finalColor, (colorMod1 - finalColor) + (color4 + colorMod1), pulse(0.06, age));
    finalColor = mix(colorMod1, finalColor, health);
    finalColor += (albedo.r + albedo2.r) * 0.1;
    finalColor -= 1.0 - albedo.a;
    float clradd = finalColor.r + finalColor.g + finalColor.b;
    if (clradd < 0.1) finalColor = vec3(0.15, 0.15, 0.3);
    else if (clradd > 0.1 && clradd < 0.2) finalColor = vec3(specular.b);
    finalColor += (albedo.r + albedo2.r) * vec3(0.0, 0.0, 0.3);
    return vec4(finalColor, 0.17 + (albedo.r + albedo2.r) * 0.2);
}

vec4 shieldAeon(float health) { // ShieldAeonPS
    vec4 albedo = texture(albedoSampler, fragTex0.xy);
    vec3 specular = texture(specularSampler, fragTex0.zw).rgb;
    vec3 specular2 = texture(specularSampler, fragTex1.xy).rgb;
    vec3 normal = computeNormal(fragTex1.zw * 4.0);
    vec3 V = faViewDirection(fragWorldPos);
    float phongAmount = clamp(dot(reflect(-V, normal), V), 0.0, 1.0) * 0.6;
    vec3 environment = texture(environmentSampler, reflect(-V, normal)).rgb;
    float terrainBand = albedo.b * 0.5;
    vec3 color1 = phongAmount + environment - albedo.ggg;
    vec3 color2 = specular.rrr * mix(0.6, 1.3, pulse(0.015, pc.time));
    vec3 color3 = specular2.rrr * mix(2.0, 2.2, pulse(0.0045, pc.time));
    vec3 finalColor = (color1 * color2) * color3;
    vec3 color4 = (finalColor * normal) * 0.65 + finalColor;
    finalColor = color4 * environment * albedo.a;
    vec3 colorMod1 = mix(vec3(0.7, 0.3, 0.3), finalColor, 0.9);
    finalColor = mix(colorMod1, finalColor, health);
    float alpha = 0.707 * ((environment.r + environment.g + environment.b) * 0.25) + terrainBand;
    // FA mixes by the health twice
    return vec4(mix(colorMod1, finalColor, health), alpha);
}

vec4 shieldSeraphim() { // ShieldSeraphimPS
    // (FA also reads the normal map at texcoord1.zw into m, which both
    // branches below overwrite: dead there too.)
    vec3 normal = computeNormal(fragTex1.zw);
    vec4 uvaddress = texture(normalsSampler, fragTex1.xy);
    vec4 specular = texture(specularSampler, fragTex0.xy + uvaddress.rb * 0.1);
    // abs(cos(dot(float4(0,1,0,0), normal))): the dot of a float4 and a
    // float3 is the up axis alone; the cos is FA's
    float dp = abs(cos(normal.y));
    const float maxBrightness = 0.453;
    float channelColor = maxBrightness - clamp(1.0 - dp, 0.0, maxBrightness);
    float t = abs(normalize(fragNormal).y);
    const float timeCutoff = 0.753;
    float dp2 = abs(dot(faViewDirection(fragWorldPos), normal));
    // Fading toward the dome's top, never quite away
    float m = t < timeCutoff ? 1.0 : 1.0 - 0.7 * (t - timeCutoff) / (1.0 - timeCutoff);
    float alpha = m * (dp2 * 0.3 + channelColor) * 1.75;
    return vec4(vec3(0.425, 0.76274, 1.0) * dp * dp * specular.rgb, alpha);
}

vec4 shieldImpact(float age) { // ShieldImpactPS(2, 0.2)
    float alphaFade = clamp(1.0 - (age - 2.0) * 0.2, 0.0, 1.0);
    vec4 albedo = texture(albedoSampler, fragTex0.xy);
    vec4 normal = texture(specularSampler, fragTex1.xy);
    float alphaMask = texture(specularSampler, fragTex2).a;
    return vec4(vec3(0.0, 0.0, 0.5) + normal.r,
                alphaMask * albedo.g * (normal.r + normal.g) * alphaFade);
}

vec4 cybranShieldImpact(float age) { // CybranShieldImpactPS(6, 0.15, 4.5)
    float alphaFade = clamp(1.0 - (age - 6.0) * 0.15, 0.0, 1.0);
    vec4 color0 = texture(specularSampler, fragTex0.xy);
    vec4 color1 = texture(specularSampler, fragTex1.xy);
    vec4 color2 = texture(specularSampler, fragTex2);
    return vec4(color1.rrr + color0.g, color1.b * color2.r * 4.5 * alphaFade * color0.a);
}

// PhaseShieldPS and SeraphimPhaseShieldPS (M211l): a personal shield's
// electric shell, from three scrolled reads of its lookup (the Seraphim's:
// its secondary texture)
vec4 phaseShield(float age, bool seraphim) {
    vec2 tc1 = fragTex0.xy * 0.5 + age * vec2(0.005, 0.02);
    vec2 tc2 = fragTex0.xy * 4.0 + age * vec2(-0.008, 0.008);
    vec2 tc3 = fragTex0.xy * 0.01 + age * vec2(-0.0018, 0.0);
    vec4 lookup = seraphim ? texture(secondarySampler, tc1) : texture(lookupSampler, tc1);
    vec4 lookup2 = seraphim ? texture(secondarySampler, tc2) : texture(lookupSampler, tc2);
    vec4 lookup3 = seraphim ? texture(secondarySampler, tc3) : texture(lookupSampler, tc3);
    float electricity = lookup.r * lookup2.b;
    vec4 baseShellColor = vec4(0.5, 0.5, 1.0, 1.0);
    vec4 glowPulse = vec4(lookup3.ggg, min(lookup3.g, 0.65) + electricity);
    return (baseShellColor + electricity) * glowPulse;
}

void main() {
    float age = fragMaterial.x;
    float health = fragMaterial.y;
    vec4 color = vec4(0.0); // ShieldFillPS: nothing (its pipeline writes depth alone)
    bool low = pc.lane == 0u; // the Low lanes (M211n)
    if (pc.technique == 18u) color = low ? shieldLoFi() : shieldUEF(health);
    else if (pc.technique == 19u) color = low ? shieldCybranLoFi() : shieldCybran(age, health);
    else if (pc.technique == 20u) color = low ? shieldAeonLoFi() : shieldAeon(health);
    else if (pc.technique == 21u) color = low ? shieldAeonLoFi() : shieldSeraphim();
    else if (pc.technique == 23u) color = shieldImpact(age);
    else if (pc.technique == 24u) color = cybranShieldImpact(age);
    else if (pc.technique == 25u) color = phaseShield(age, false);
    else if (pc.technique == 26u) color = phaseShield(age, true);
    // FA drew into an 8-bit target, which clamps what a shader writes
    // before it blends: Cybran's colour goes below 0, UEF's alpha past 1.
    outColor = clamp(color, 0.0, 1.0);
}
)glsl";

} // namespace

const char* shield_vert() {
    static const std::string source = std::string(kShieldPushBlock) + kShieldVertMain;
    return source.c_str();
}

const char* shield_frag() {
    static const std::string source = std::string(kShieldPushBlock) + kShieldFragMain;
    return source.c_str();
}

} // namespace osc::renderer::shaders
