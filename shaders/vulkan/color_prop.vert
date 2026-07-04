// VK-P11 STATIC-PROP COLOR — color INSTANCED static-prop VS (Tier-0 seam probe).
// =============================================================================
// Reproduces the GL producer shaders/static_prop.vert world->clip transform so a
// VK-produced color image lands props at the SAME on-screen positions:
//
//   GL (static_prop.vert main):
//     vec4 world_stuff = vec4(a_position, 1.0) * inst.modelMatrix;   // row-vector * M
//     vec3 world_mc2   = vec3(-world_stuff.x, world_stuff.z, world_stuff.y); // Stuff->MC2
//     vec4 clip        = u_worldToClipGL * vec4(world_mc2, 1.0);
//     clip.z          += OBJECT_DEPTH_BIAS * clip.w;                 // reverse-Z surface bias
//     gl_Position      = clip;
//
// LOCKSTEP with C++ GpuStaticPropInstance (gos_static_prop_batcher.h, 112B,
// static_assert'd) — the VK instance SSBO is a byte-identical memcpy of the SAME
// std430 POD. modelMatrix uploaded GL_FALSE (no transpose); GLSL fills a mat4 from
// linear memory column-major, so `vec4(pos,1) * modelMatrix` == the GL VS.
// u_worldToClipGL is the engine's gos_GetObjectFixBMVPMat4() (also GL_FALSE upload),
// carried in the per-frame UBO as 16 linear floats.
//
// Tier-0 scope: the color pass depth-tests against a VK-OWNED D32 (cleared each
// frame to reverse-Z far = 0.0), GEQUAL — matching the GL StaticPropOpaque pass.
// Inter-prop occlusion is correct; prop-vs-terrain occlusion is approximate (props
// composite over the scene). NOT full parity — this is the transport/geometry proof.
#version 450

// Shared static-prop VBO layout (kVertexStride=40): position vec3 @0, normal vec3
// @12, uv vec2 @24, localVertexID uint @32, aRGBLight uint @36. Tier-0 consumes
// position + uv (normal/IDs forwarded-inert; unused in the flat-debug FS).
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uint a_localVertexID;
layout(location = 4) in uint a_aRGBLight;

// Per-frame UBO: the world->clip matrix (16 linear floats, GL_FALSE upload) + the
// reverse-Z object depth bias (matches GL OBJECT_DEPTH_BIAS) + instBase (0; the
// caller sorts instances by typeID and draws each type-run with firstInstance).
layout(set = 0, binding = 0, std140) uniform ColorFrameUBO {
    mat4  worldToClipGL;   // gos_GetObjectFixBMVPMat4() — GL_FALSE (column-major load)
    float objectDepthBias; // reverse-Z surface bias (== OBJECT_DEPTH_BIAS)
    int   instBase;
    int   _pad0;
    int   _pad1;
} uFrame;

// Instance SSBO (set 0 binding 1). Byte-identical to C++ GpuStaticPropInstance
// (112B, alignas(16)); only modelMatrix is read but the FULL layout is kept so
// gl_InstanceIndex strides match the memcpy'd POD exactly.
struct Instance {
    mat4  modelMatrix;       // off 0
    uint  typeID;            // off 64
    uint  firstColorOffset;  // off 68
    uint  flags;             // off 72
    uint  lightDataIndex;    // off 76
    vec4  aRGBHighlight;     // off 80
    vec4  fogRGB;            // off 96
};
layout(set = 0, binding = 1, std430) readonly buffer Instances {
    Instance i[];
} uInstances;

layout(location = 0) out vec2 vUv;

void main() {
    Instance inst = uInstances.i[uFrame.instBase + gl_InstanceIndex];
    // Row-vector * M — identical to the GL VS (Stuff std430 column-major convention).
    vec4 world_stuff = vec4(a_position, 1.0) * inst.modelMatrix;
    // Stuff -> MC2 frame swizzle — identical to the GL VS.
    vec3 world_mc2 = vec3(-world_stuff.x, world_stuff.z, world_stuff.y);
    vec4 clip = uFrame.worldToClipGL * vec4(world_mc2, 1.0);
    // Reverse-Z surface bias (GL static_prop.vert: clip.z += OBJECT_DEPTH_BIAS*clip.w).
    clip.z += uFrame.objectDepthBias * clip.w;
    gl_Position = clip;
    vUv = a_uv;
}
