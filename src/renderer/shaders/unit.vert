#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

// Per-vertex cube data (binding 0)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

// Per-instance data (binding 1)
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
