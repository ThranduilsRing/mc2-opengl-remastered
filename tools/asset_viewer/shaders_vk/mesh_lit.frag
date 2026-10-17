#version 450
layout(set=0, binding=0, std140) uniform U {
    mat4 viewProj;
    mat4 model;
    vec4 cameraPos;
    vec4 lightDir;   // world space, points FROM the light
    ivec4 flags;     // x = has albedo
} u;
layout(set=0, binding=1) uniform sampler2D albedoTex;

layout(location=0) in vec3 v_posWorld;
layout(location=1) in vec3 v_normal;
layout(location=2) in vec2 v_uv;
layout(location=0) out vec4 fragColor;

void main() {
    vec3 N = normalize(v_normal);
    vec3 L = normalize(-u.lightDir.xyz);
    vec3 albedo = (u.flags.x != 0) ? texture(albedoTex, v_uv).rgb : vec3(0.78);   // flat grey fallback
    float NdotL = max(dot(N, L), 0.0);
    vec3 color = albedo * (NdotL * 0.85 + 0.15);                                    // Lambert + ambient
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));                           // gamma encode (target is UNORM)
    fragColor = vec4(color, 1.0);
}
