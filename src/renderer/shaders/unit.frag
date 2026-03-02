#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Directional light (same as terrain)
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float NdotL = max(dot(normalize(fragNormal), lightDir), 0.0);
    float lighting = 0.4 + 0.6 * NdotL;

    outColor = vec4(fragColor.rgb * lighting, fragColor.a);
}
