// VK-POST-COMPOSITE-NATIVE-1: conservative Visual-mode composite shader.
// This is the first runtime slice: scene color + exposure + existing visual grade.
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;

layout(push_constant) uniform Params {
    vec4 p0; // exposure, unused, unused, unused
} u;

void main() {
    vec3 color = texture(sceneTex, TexCoord).rgb * u.p0.x;

    color *= vec3(1.05, 1.01, 0.94);
    float lum = dot(color, vec3(0.299, 0.587, 0.114));
    vec3 warm = vec3(1.05, 1.00, 0.93);
    vec3 cool = vec3(0.95, 0.98, 1.04);
    vec3 grade = mix(cool, warm, smoothstep(0.10, 0.75, lum));
    color *= grade;

    vec2 vdir = TexCoord - vec2(0.5);
    float vdist = length(vdir * vec2(1.0, 0.6));
    float vignette = smoothstep(0.85, 0.25, vdist);
    color *= mix(0.82, 1.0, vignette);

    float sunBias = pow(smoothstep(0.0, 1.0, 1.0 - TexCoord.y), 1.5);
    color = mix(color, color * vec3(1.10, 1.03, 0.92), sunBias * 0.25);

    FragColor = vec4(color, 1.0);
}
