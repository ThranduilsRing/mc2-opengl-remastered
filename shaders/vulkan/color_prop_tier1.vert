// VK-P11 STATIC-PROP COLOR — TIER-1 (parity) instanced static-prop VS.
// =============================================================================
// Identical world->clip transform to color_prop.vert (Tier-0), but ALSO forwards
// the Stuff-space normal to the FS so the GBuffer1 (MRT1) normal can be written
// bit-adjacent to the GL producer (shaders/static_prop.frag):
//   GBuffer1 = vec4(normalize(v_normal)*0.5+0.5, 0.25)
//
// ★v_normal is STUFF-SPACE, NOT GL/MC2-space. The GL producer's Stuff->MC2 frame
// swap applies to POSITIONS only; the GBuffer normal is written un-swapped (only
// the GL PBR block swaps for lighting). We therefore forward a_normal WITHOUT the
// vec3(-x, z, y) swizzle, exactly like static_prop.vert forwards v_normal.
#version 450

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uint a_localVertexID;
layout(location = 4) in uint a_aRGBLight;

layout(set = 0, binding = 0, std140) uniform ColorFrameUBO {
    mat4  worldToClipGL;   // gos_GetObjectFixBMVPMat4() — GL_FALSE (column-major load)
    float objectDepthBias; // reverse-Z surface bias (== OBJECT_DEPTH_BIAS)
    int   instBase;
    int   _pad0;
    int   _pad1;
} uFrame;

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
layout(location = 1) out vec3 vNormalStuff;   // Stuff-space normal (un-swapped)

void main() {
    Instance inst = uInstances.i[uFrame.instBase + gl_InstanceIndex];
    // Row-vector * M — identical to the GL VS (Stuff std430 column-major convention).
    vec4 world_stuff = vec4(a_position, 1.0) * inst.modelMatrix;
    // POSITION: Stuff -> MC2 frame swizzle — identical to the GL VS.
    vec3 world_mc2 = vec3(-world_stuff.x, world_stuff.z, world_stuff.y);
    vec4 clip = uFrame.worldToClipGL * vec4(world_mc2, 1.0);
    clip.z += uFrame.objectDepthBias * clip.w;
    gl_Position = clip;
    vUv = a_uv;
    // NORMAL: forward in STUFF-SPACE (un-swapped). BIT-EXACT mirror of
    // static_prop.vert:352 `worldNormal = a_normal * mat3(inst.modelMatrix)`
    // (ROW-VECTOR convention — Stuff std430 col-major; `mat3(M) * a_normal` would
    // apply the INVERSE rotation and break parity). The Stuff->MC2 axis swap is NOT
    // applied to the normal (positions only), so this is the un-swapped Stuff-space
    // normal the GL GBuffer1 is written from. The FS then does normalize()*0.5+0.5.
    vNormalStuff = a_normal * mat3(inst.modelMatrix);
}
