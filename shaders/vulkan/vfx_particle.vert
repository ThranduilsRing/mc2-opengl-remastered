// VK-FIELD-VFX-1 -- Vulkan port of the GL particle billboard VS
// (shaders/particle_billboard.vert), UNTEXTURED colored-soft-billboard slice.
//
// PARITY CONTRACT (vs the GL particle VS):
//   - Procedural, VERTEX-LESS: 6 verts/particle (2 triangles) expanded from
//     gl_VertexIndex indexing the per-frame GpuParticle SSBO. SAME expansion as
//     the GL VS (vid/6 -> particleId, vid%6 -> cornerSlot). Draw call is
//     vkCmdDraw(count*6, 1, group.start*6, 0) so gl_VertexIndex is already
//     offset to the group's first vertex.
//   - Billboard: view-aligned via camRight/camUp (already GL-world-space,
//     axis-swapped by the C++ caller), aspect + in-plane spin from velocity.xyz,
//     axis-swapped center (GL_x=-Stuff_x, GL_y=Stuff_z, GL_z=Stuff_y).
//   - MVP: worldToClipGL is the SAME 16 raw floats GL uploads (GL_FALSE); packed
//     column_major (std140 default) -> gl_Position = M * worldPos, exactly like
//     the water island (no transpose, no Y-flip in-shader; the shared GBuffer is
//     authored in GL clip by every island, so consistency is what matters).
//   - UNTEXTURED slice: uv/atlas dropped; the FS shades v_color with a soft
//     radial falloff from v_corner (round soft blob). Textured sprite fidelity is
//     the documented deferred remainder.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "../include/axis.hglsl"

struct Particle {
    vec3  position;  float _pad0;   // 0
    vec4  color;                     // 16
    vec3  velocity;  uint  kind_flags; // 32 / 44
    float lifetime;  float age;      // 48 / 52
    float size;      uint  atlasIndex; // 56 / 60
};
layout(std430, set = 0, binding = 1) readonly buffer Particles {
    Particle particles[];
};

layout(set = 0, binding = 0, std140) uniform VfxVsParams {
    mat4 worldToClipGL;   // SAME 16 raw floats as GL GL_FALSE upload
    vec4 camRight;        // xyz = GL-world-space camera right (w unused)
    vec4 camUp;           // xyz = GL-world-space camera up    (w unused)
} U;

// Atlas UVs for the four billboard corners (normalized [0,1]).
const vec2 kCornerUv[4] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
// Two CCW triangles per quad (matches the GL VS winding fix).
const int kCornerIdx[6] = int[](0, 1, 2, 2, 3, 1);

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_corner;   // [0,1] corner uv for the soft-blob FS

void main() {
    uint particleId = uint(gl_VertexIndex) / 6u;
    int  cornerSlot = int(uint(gl_VertexIndex) % 6u);
    int  cornerIdx  = kCornerIdx[cornerSlot];

    Particle p = particles[particleId];

    // Aspect-scaled corner with in-plane spin (velocity.xyz = sizeX,sizeY,spin
    // from the oracle path; 0,0,0 from placeholder). Mirrors the GL VS floors.
    float sizeFloor = (p.lifetime > 0.5) ? 1.0 : 8.0;
    float sX = (p.velocity.x > 0.0) ? max(p.velocity.x, 4.0) : max(p.size, sizeFloor);
    float sY = (p.velocity.y > 0.0) ? max(p.velocity.y, 4.0) : max(p.size, sizeFloor);
    vec2 unitCorner = (kCornerUv[cornerIdx] - vec2(0.5)) * 2.0;   // [-1,1]
    float cs = cos(p.velocity.z);
    float sn = sin(p.velocity.z);
    vec2 corner = mat2(cs, sn, -sn, cs) * (unitCorner * vec2(sX, sY));

    vec3 stuffCenter = p.position.xyz;
    vec3 glCenter    = mc2ToRender(stuffCenter);
    vec3 worldPos    = glCenter
                     + U.camRight.xyz * corner.x
                     + U.camUp.xyz    * corner.y;

    gl_Position = U.worldToClipGL * vec4(worldPos, 1.0);

    v_color  = p.color;
    v_corner = kCornerUv[cornerIdx];
}
