#version 450
// Material preview sphere (LocalPbrMaterialBackend). Mirrors PbrUbo in LocalPbrMaterialBackend.cpp (std140).
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec4 a_tangent;   // xyz + handedness
layout(location=3) in vec2 a_uv;

layout(set=0, binding=0, std140) uniform U {
    mat4 viewProj;   // model is identity
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
    ivec4 has;       // x normal, y orm, z emissive
} u;

layout(location=0) out vec3 v_worldPos;
layout(location=1) out vec3 v_normal;
layout(location=2) out vec3 v_tangent;
layout(location=3) out vec3 v_bitangent;
layout(location=4) out vec2 v_uv;

void main() {
    v_worldPos = a_pos;
    v_normal = normalize(a_normal);
    v_tangent = normalize(a_tangent.xyz);
    v_bitangent = normalize(cross(v_normal, v_tangent) * a_tangent.w);
    v_uv = a_uv;
    gl_Position = u.viewProj * vec4(a_pos, 1.0);
}
