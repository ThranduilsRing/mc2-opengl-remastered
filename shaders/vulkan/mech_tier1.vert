// VK-P14 MECH TIER-1 — skinned instanced mech VS (SUB-SLICE 1: skinning + flat color).
// =============================================================================
// Port of shaders/mech.vert's skinning + world->clip, into the VK shared-GBuffer
// co-write pipeline. Linear-blend skinning over 4 bone weights (mech.vert:156-180),
// then the Stuff->MC2 axis swap worldMC2 = vec3(-x, z, y) (mech.vert:178). Stock
// weights = (1,0,0,0) collapse to a rigid single-bone lookup (byte-identical to the
// GL rigid path); imported meshes ship real 4-weight blends.
//
// Bone convention (mech.vert:5-7 / gos_mech_batcher.h:72-78): GpuMechBone stores the
// Stuff LinearMatrix4D rows; mat4(row0..row3) fills GLSL COLUMNS from those args, so
// boneT is the TRANSPOSE of the Stuff matrix and boneT * vec4(pos,1) is row-vector
// math (world position in the Stuff/MLR camera frame). Do NOT "fix" the transpose.
//
// GBuffer1 normal: mech.frag writes rc_gbuffer1_screenShadowEligible(normalize(N))
// = vec4(normalize(N)*0.5+0.5, 0.0) where N is the MC2/GL WORLD normal (mech.vert's
// worldNormal = normalize(vec3(-nStuff.x, nStuff.z, nStuff.y))). ★Unlike the static
// prop (Stuff-space), the mech GBuffer normal IS the swapped GL-world normal — mech.vert
// forwards v_normal = worldNormal (the swapped one). We match that here.
#version 450

layout(location = 0) in vec3  a_position;
layout(location = 1) in vec3  a_normal;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uvec4 a_boneIndices;  // u8x4 -> uvec4
layout(location = 4) in vec4  a_boneWeights;  // u8x4 normalized -> [0,1]
layout(location = 5) in vec2  a_tangentOct;   // s16x2 normalized (unused sub-slice 1)
layout(location = 6) in uint  a_aRGBLight;    // BGRA packed (unused sub-slice 1)

layout(set = 0, binding = 0, std140) uniform MechFrameUBO {
    mat4  worldToClipGL;   // gos_GetMechFixBMVPMat4() — GL_FALSE (column-major load)
    float objectDepthBias; // OBJECT_DEPTH_BIAS
    int   materialReady;   // 1 = FS samples the copy-once albedo
    int   _pad0;
    int   _pad1;
} uFrame;

// GpuMechInstance mirror (64B; LOCKSTEP with gos_mech_batcher.h:35). We only read
// baseBoneOffset here; the rest keeps the std430 stride correct.
struct GpuMechInstance {
    uint  typeLodRecordIndex;
    uint  baseBoneOffset;
    uint  lightDataIndex;
    uint  renderFlags;
    vec4  aRGBHighlight;
    vec4  fogRGB;
    uint  objectIdRaw;
    uint  materialIdx;
    uint  _pad2;
    uint  _pad3;
};
layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer {
    GpuMechInstance instances[];
} uInst;

// Per-frame bone palette (GpuMechBone 64B; 4 explicit rows).
struct GpuMechBone { vec4 row0, row1, row2, row3; };
layout(set = 0, binding = 2, std430) readonly buffer BoneBuffer {
    GpuMechBone bones[];
} uBones;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec3 vNormalGL;   // MC2/GL-world normal (swapped), for GBuffer1

void main() {
    GpuMechInstance inst = uInst.instances[gl_InstanceIndex];

    // Linear-blend skinning (mech.vert:156-170). Stock weights=(1,0,0,0) => single-bone.
    mat4 boneT = mat4(0.0);
    for (int bi = 0; bi < 4; ++bi) {
        float w = a_boneWeights[bi];
        if (w > 0.0) {
            GpuMechBone bn = uBones.bones[a_boneIndices[bi] + inst.baseBoneOffset];
            boneT += w * mat4(bn.row0, bn.row1, bn.row2, bn.row3);
        }
    }

    vec4 worldStuff = boneT * vec4(a_position, 1.0);
    // Stuff -> MC2/GL axis swap (mech.vert:178). #1 P-conjugation risk lives here.
    vec3 worldMC2 = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);
    vec3 normalStuff = mat3(boneT) * a_normal;
    vec3 worldNormal = normalize(vec3(-normalStuff.x, normalStuff.z, normalStuff.y));

    vec4 clip = uFrame.worldToClipGL * vec4(worldMC2, 1.0);
    clip.z += uFrame.objectDepthBias * clip.w;   // mech.vert:187
    gl_Position = clip;

    vUv       = a_uv;
    vNormalGL = worldNormal;   // mech.frag forwards the SWAPPED world normal as v_normal
}
