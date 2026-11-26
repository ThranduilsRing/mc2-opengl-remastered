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
//   - Atlas uv: group sub-rect (push constants) plus the per-particle flipbook
//     frame when atlasColumns > 1, as the GL VS computes v_uv; head flag from
//     kind_flags bit 0.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "../include/axis.hglsl"

struct Particle {
    vec3  position;  uint _pad0;    // 0 / 12: group index on the indexed path
    vec4  color;                     // 16
    vec3  velocity;  uint  kind_flags; // 32 / 44
    float lifetime;  float age;      // 48 / 52
    float size;      uint  atlasIndex; // 56 / 60
};
layout(std430, set = 0, binding = 1) readonly buffer Particles {
    uint words[]; // scalar-packed: 16 words/legacy particle, 10/indexed vertex
};
vec2 read2(uint at) { return uintBitsToFloat(uvec2(words[at], words[at+1u])); }
vec3 read3(uint at) { return uintBitsToFloat(uvec3(words[at], words[at+1u], words[at+2u])); }
vec4 read4(uint at) { return uintBitsToFloat(uvec4(words[at], words[at+1u], words[at+2u], words[at+3u])); }
Particle readParticle(uint index) {
    uint b = index * 16u;
    return Particle(read3(b), words[b+3u], read4(b+4u),
        read3(b+8u), words[b+11u], uintBitsToFloat(words[b+12u]),
        uintBitsToFloat(words[b+13u]), uintBitsToFloat(words[b+14u]), words[b+15u]);
}

layout(set = 0, binding = 0, std140) uniform VfxVsParams {
    mat4 worldToClipGL;   // SAME 16 raw floats as GL GL_FALSE upload
    vec4 camRight;        // xyz = GL-world-space camera right (w unused)
    vec4 camUp;           // xyz = GL-world-space camera up    (w unused)
} U;

// Per-group atlas sub-rect; LOCKSTEP with VfxGroupPush in vulkan_vfx_island.cpp.
layout(push_constant) uniform VfxGroupPush {
    vec2 uvOffset;
    vec2 uvSize;
    uint atlasColumns;   // 0 / 1 = static, > 1 = flipbook (frame = atlasIndex)
    uint blendMode;      // VFX blend recipe, VFX_ADD_*
} G;

#ifdef VFX_INDEXED
struct Group {
    vec2 uvOffset;
    vec2 uvSize;
    uint atlasColumns;
    uint blendMode;
    uint page;
    uint blend;   // island blend mode 0/1/2/3/4/5; the fragment shader weights it for the premultiplied fold
};
layout(std430, set = 1, binding = 1) readonly buffer Groups { Group groups[]; };
layout(location = 5) flat out uint v_page;
#endif
layout(location = 6) flat out uint v_blend;

// Atlas UVs for the four billboard corners (normalized [0,1]).
const vec2 kCornerUv[4] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
// Two CCW triangles per quad (matches the GL VS winding fix).
const int kCornerIdx[6] = int[](0, 1, 2, 2, 3, 1);

// Group additive mode (push constant blendMode).
const uint ADDITIVE_ONE_ONE = 2u;   // pure additive (GL OneOne); ADDITIVE_* mirrors VfxAdditive in vulkan_vfx_island.cpp

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_flags;

layout(location = 3) out vec4 v_vegData;
layout(location = 4) out vec2 v_vegAux;
void main() {
#ifdef VFX_INDEXED
    uint record = G.atlasColumns == 0xffffffffu ? uint(gl_VertexIndex) : uint(gl_VertexIndex) / 6u;
    Group g = groups[words[record * 16u + 3u]];
    v_page = g.page; v_blend = g.blend;
#else
    // Keep the original page-binding shader usable without descriptor indexing.
    #define g G
    v_blend = G.blendMode;
#endif
    v_vegData=vec4(0.0); v_vegAux=vec2(0.0);   // vegetation has its own vertex shader (vfx_vegetation.vert); the fragment shader still reads these
    if (G.atlasColumns == 0xfffffffdu) {
        uint b = uint(gl_VertexIndex) * 10u;
        gl_Position = U.worldToClipGL * vec4(mc2ToRender(read3(b)), 1.0);
        v_color = read4(b+3u); v_uv = read2(b+7u); v_flags = words[b+9u];
        if ((v_flags & 128u) != 0u)
            gl_Position.xy += v_uv * vec2(U.camRight.w, U.camUp.w) * gl_Position.w;
        return;
    }
    // Native mesh/ribbon vertices share the 64-byte frame-ring stride.
    if (g.atlasColumns == 0xffffffffu) {
        Particle p = readParticle(uint(gl_VertexIndex));
        // The terrain MVP includes the MC2-to-render conversion. Native
        // gosFX geometry is already in render space, so convert it back first.
        vec3 world = mc2ToRender(p.position);
        gl_Position = U.worldToClipGL * vec4(world, 1.0);
        // Surface flares need depth clearance, but moving their world corners
        // toward the camera enlarges explosions. Preserve authored screen size.
        if ((p.kind_flags & 64u) != 0u && g.blendMode == ADDITIVE_ONE_ONE) {
            vec3 liftedWorld = world - normalize(cross(U.camRight.xyz, U.camUp.xyz))
                             * min(4.0 * p.size, 0.25 * max(gl_Position.w, 0.0));
            vec4 lifted = U.worldToClipGL * vec4(liftedWorld, 1.0);
            gl_Position.z = lifted.z / lifted.w * gl_Position.w;
        }
        // PointCloud retains the original screen-sized right triangle.
        if ((p.kind_flags & 128u) != 0u)
            gl_Position.xy += vec2(p.velocity.x * U.camRight.w, p.velocity.y * U.camUp.w) * gl_Position.w;
        v_uv = p.velocity.xy;
        v_color = p.color;
        v_flags = p.kind_flags;
        return;
    }
    uint particleId = uint(gl_VertexIndex) / 6u;
    int  cornerSlot = int(uint(gl_VertexIndex) % 6u);
    int  cornerIdx  = kCornerIdx[cornerSlot];

    Particle p = readParticle(particleId);

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
    // Beam heads and additive flares sit on the surface they hit and would fail the depth test there, so lift them
    // toward the camera. Smoke, dust and alpha sprites keep true depth. The lift never crosses the near plane.
    bool pulled = ((p.kind_flags & 1u) != 0u) || (g.blendMode == ADDITIVE_ONE_ONE);
    if (pulled) {
        vec3  toCamera = normalize(cross(U.camRight.xyz, U.camUp.xyz));
        float viewW    = (U.worldToClipGL * vec4(glCenter, 1.0)).w;
        float nearD    = U.worldToClipGL[3][2] / (U.worldToClipGL[2][2] - 1.0);
        float room     = clamp(viewW - max(nearD, 0.0) * 1.05 - 0.01, 0.0, 0.5 * max(viewW, 0.0));
        glCenter      -= toCamera * min(4.0 * max(sX, sY), room);
    }
    vec3 worldPos    = glCenter
                     + U.camRight.xyz * corner.x
                     + U.camUp.xyz    * corner.y;

    gl_Position = U.worldToClipGL * vec4(worldPos, 1.0);

    vec2 frameOffset = vec2(0.0);
    if (g.atlasColumns > 1u) {
        frameOffset = vec2(float(p.atlasIndex % g.atlasColumns), float(p.atlasIndex / g.atlasColumns));
    }
    v_uv      = g.uvOffset + (frameOffset + kCornerUv[cornerIdx]) * g.uvSize;
    v_color   = p.color;
    v_flags = (p.kind_flags & 1u) | (g.blendMode == ADDITIVE_ONE_ONE ? 2u : 0u);
}
