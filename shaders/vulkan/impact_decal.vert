// Impact box decal: one instance per decal (RenderCore/ImpactDecals.h GpuDecal), 36 vertices of the decal's box.
// Only the faces turned away from the camera are kept, so every covered pixel shades exactly once, including when the
// camera is inside the box. The box lives in MC2 world space, the space the terrain world-to-clip matrix takes.
#version 450

struct ImpactDecal { vec4 v[8]; };
layout(std430, set = 0, binding = 5) readonly buffer Decals { ImpactDecal d[]; } decals;
layout(std140, set = 0, binding = 4) uniform ImpactFrame {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 eye;        // homogeneous camera point
    vec4 lightDir;   // toward the sun; w = ambient share
    vec4 screen;     // w, h, 1/w, 1/h
} F;

layout(location = 0) flat out uint vInstance;

// Faces as corner loops; corner i = (bit0 ? +x : -x, bit1 ? +y : -y, bit2 ? +z : -z). Face order +X -X +Y -Y +Z -Z.
const int kFace[24] = int[24](1, 3, 7, 5,   0, 4, 6, 2,   2, 6, 7, 3,   0, 1, 5, 4,   4, 5, 7, 6,   0, 2, 3, 1);
const int kTri[6] = int[6](0, 1, 2, 0, 2, 3);

void main() {
    ImpactDecal D = decals.d[gl_InstanceIndex];
    vInstance = uint(gl_InstanceIndex);
    int face = gl_VertexIndex / 6;
    int corner = kFace[face * 4 + kTri[gl_VertexIndex % 6]];
    vec3 s = vec3((corner & 1) != 0 ? 1.0 : -1.0, (corner & 2) != 0 ? 1.0 : -1.0, (corner & 4) != 0 ? 1.0 : -1.0);
    vec3 c = D.v[0].xyz;
    vec3 ax = D.v[1].xyz;
    vec3 ay = D.v[2].xyz;
    vec3 az = D.v[3].xyz;
    vec3 world = c + ax * s.x + ay * s.y + az * s.z;

    // Outward face normal (scaled) and the face centre.
    float sgn = (face & 1) == 0 ? 1.0 : -1.0;
    vec3 n = sgn * (face < 2 ? ax : (face < 4 ? ay : az));
    vec3 fc = c + n;
    vec3 toEye = F.eye.xyz - fc * F.eye.w;
    if (dot(n, toEye) > 0.0) {   // camera-facing: drop the face
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        return;
    }
    gl_Position = F.viewProj * vec4(world, 1.0);
}
