#version 450
// Preview lit mesh (MeshPreview3D). Mirrors MeshUbo in MeshPreview3D.cpp (std140).
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;

layout(set=0, binding=0, std140) uniform U {
    mat4 viewProj;
    mat4 model;      // rotation about the bounds center
    vec4 cameraPos;
    vec4 lightDir;
    ivec4 flags;     // x = has albedo
} u;

layout(location=0) out vec3 v_posWorld;
layout(location=1) out vec3 v_normal;
layout(location=2) out vec2 v_uv;

void main() {
    vec4 wp = u.model * vec4(a_pos, 1.0);
    v_posWorld = wp.xyz;
    v_normal = normalize(mat3(u.model) * a_normal);
    v_uv = a_uv;
    gl_Position = u.viewProj * wp;
}
