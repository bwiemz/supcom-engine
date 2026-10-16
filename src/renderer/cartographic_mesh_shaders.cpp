// A cartographic view's meshes (Moho's MeshRenderer::RenderCartographic,
// retail effects/mesh.fx): CartographicVS, then by the push block's
// technique (CartographicTechnique) and pass:
//
//   1 Unit      P0 CartographicPS(false): the instance's colour, alpha by
//                  how far the surface faces from up (0.5 to 1)
//               P1 CartographicGlowPS: alpha alone, 0.2 edge^2
//   2 Feature   CartographicPS(true): the height ramp at the point's
//               elevation, banded by the light's tone; albedo alpha < 0.5
//               clipped
//   3 Feedback  CartographicFeedbackPS: the albedo
//   4 Place     CartographicPlacePS: the instance's colour, alpha 1/8
//   5 Build     CartographicBuildPS: the colour lightened by 0.1, alpha 0.4
//   6 Shield    CartographicShieldPS: white at tone^80, alpha 0.4 of it
//
// All but the shield clip what lies under surfaceElevation. No lighting,
// shadow, fog or terrain reaches them.

#include "renderer/shader_utils.hpp"

namespace osc::renderer::shaders {

namespace {

// CartographicVS: the world matrix alone (no build growth, wreck crumple,
// sway or feedback scaling), the elevation the world height. The skinning
// is the mesh shader's.
const char* kVert = R"glsl(
#version 450

layout(push_constant) uniform Push {
    mat4 viewProj;
    uint boneBase;
    uint bonesPerInst;
    float surface; // surfaceElevation
    float elevMin; // minimumElevation
    float elevMax; // maximumElevation
    uint technique;
    uint pass;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in mat4 inModel;
layout(location = 7) in vec4 inColor;
layout(location = 8) in uvec4 inBoneIndices;
layout(location = 9) in vec4 inBoneWeights;

layout(std430, set = 1, binding = 0) readonly buffer BoneBuffer {
    mat4 bones[];
} boneSSBO;

layout(location = 0) out float fragElevation;
layout(location = 1) out vec2 fragUV;
layout(location = 2) flat out vec4 fragColor;
layout(location = 3) out vec3 fragNormal;

void main() {
    mat4 bone = mat4(1.0);
    if (pc.bonesPerInst > 0u) {
        uint base = pc.boneBase + uint(gl_InstanceIndex) * pc.bonesPerInst;
        bone = inBoneWeights[0] * boneSSBO.bones[base + inBoneIndices[0]]
             + inBoneWeights[1] * boneSSBO.bones[base + inBoneIndices[1]]
             + inBoneWeights[2] * boneSSBO.bones[base + inBoneIndices[2]]
             + inBoneWeights[3] * boneSSBO.bones[base + inBoneIndices[3]];
    }
    vec4 world = inModel * (bone * vec4(inPosition, 1.0));
    fragElevation = world.y;
    gl_Position = pc.viewProj * world;
    fragUV = inUV;
    fragColor = inColor;
    // mul(normal, (float3x3)worldMatrix), normalised by the pixel shaders
    fragNormal = mat3(inModel) * mat3(bone) * inNormal;
}
)glsl";

const char* kFrag = R"glsl(
#version 450

layout(push_constant) uniform Push {
    mat4 viewProj;
    uint boneBase;
    uint bonesPerInst;
    float surface;
    float elevMin;
    float elevMax;
    uint technique;
    uint pass;
} pc;

layout(set = 0, binding = 0) uniform sampler2D albedo;      // LINEAR, WRAP
layout(set = 2, binding = 0) uniform sampler2D hypsometric; // read POINT, CLAMP

layout(location = 0) in float fragElevation;
layout(location = 1) in vec2 fragUV;
layout(location = 2) flat in vec4 fragColor;
layout(location = 3) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

void main() {
    // clip(vertex.elevation - surfaceElevation)
    if (pc.technique != 6u && fragElevation - pc.surface < 0.0) discard;
    vec3 n = normalize(fragNormal);
    const vec3 light = normalize(vec3(-1.0, 3.0, -1.0));
    if (pc.technique == 1u) {
        if (pc.pass == 0u) {
            float edge = clamp(1.0 - clamp(n.y, 0.0, 1.0), 0.5, 1.0);
            outColor = vec4(fragColor.rgb, edge);
        } else {
            float edge = 1.0 - clamp(n.y, 0.0, 1.0);
            outColor = vec4(0.0, 0.0, 0.0, 0.2 * edge * edge);
        }
    } else if (pc.technique == 2u) {
        // A flat map (no span) indexes the ramp's first texel, where
        // Moho's division would make no number
        float span = pc.elevMax - pc.elevMin;
        float h = span > 0.0 ? (fragElevation - pc.elevMin) / span : 0.0;
        if (texture(albedo, fragUV).a - 0.5 < 0.0) discard;
        // tex1D, POINT and CLAMP: texel floor(h * 256), within the ramp
        int i = int(clamp(floor(h * 256.0), 0.0, 255.0));
        vec3 c = texelFetch(hypsometric, ivec2(i, 0), 0).rgb;
        float tone = dot(n, light);
        float cd0 = tone > 0.21 ? 0.25 : 0.0;
        float cd1 = tone > 0.66 ? 0.50 : 0.0;
        float cs = tone > 0.95 ? 0.10 : 0.0;
        c = 0.25 * c + cd0 * c + cd1 * c + cs;
        outColor = vec4(c, 1.0);
    } else if (pc.technique == 3u) {
        outColor = vec4(texture(albedo, fragUV).rgb, 1.0);
    } else if (pc.technique == 4u) {
        outColor = vec4(fragColor.rgb, 0.125);
    } else if (pc.technique == 5u) {
        outColor = vec4(clamp(fragColor.rgb + vec3(0.1), 0.0, 1.0), 0.4);
    } else {
        float tone = pow(clamp(dot(n, light), 0.0, 1.0), 80.0);
        outColor = vec4(vec3(tone), 0.4 * tone);
    }
}
)glsl";

} // namespace

const char* cartographic_mesh_vert() {
    return kVert;
}

const char* cartographic_mesh_frag() {
    return kFrag;
}

} // namespace osc::renderer::shaders
