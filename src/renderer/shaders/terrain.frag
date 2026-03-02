#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in float fragHeight;

layout(location = 0) out vec4 outColor;

void main() {
    // Height-based coloring: green (low) -> brown (mid) -> white (high)
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

    // Simple directional light from upper-right
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float NdotL = max(dot(normalize(fragNormal), lightDir), 0.0);
    float lighting = 0.3 + 0.7 * NdotL; // ambient + diffuse

    outColor = vec4(baseColor * lighting, 1.0);
}
