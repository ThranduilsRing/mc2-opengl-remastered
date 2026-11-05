// VK mech shadow-caster VS: depth-only, instanced and skinned. The twin of shaders/shadow_mech.vert (GL): the same weighted bone blend and the Stuff -> MC2
// swizzle, written into the same forward-Z light-space clip position the static-prop caster VS writes, so one render pass holds both kinds of caster.
// mat4(row0..row3) is intentionally the transpose of the Stuff bone matrix so boneT * v is the row-vector product (see mech_tier1.vert): do NOT "fix" it.
// The run's first instance is the draw's firstInstance, so gl_InstanceIndex addresses the instance record directly and no per-run uniform exists.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3  a_position;
layout(location = 3) in uvec4 a_boneIndices;   // R8G8B8A8_UINT
layout(location = 4) in vec4  a_boneWeights;   // R8G8B8A8_UNORM

// The same per-frame UBO the static-prop caster VS reads (one buffer for both pipelines); only the matrix is used here.
layout(set = 0, binding = 0, std140) uniform ShadowFrameUBO {
    mat4 lightSpaceMatrix;
    int  instBase;
    int  _pad0;
    int  _pad1;
    int  _pad2;
} uFrame;

// Per-instance record (64B): generated from RenderCore/gpu_abi/GpuMechInstance.h; a stride that differs from the C++ record shifts every run past the first.
#include "../include/gpu_abi_mech_instance.gen.hglsl"
#include "../include/axis.hglsl"
#include "../include/mech_render_flags.hglsl"
struct GpuMechBone { vec4 row0, row1, row2, row3; };
layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer { GpuMechInstance instances[]; } uInst;
layout(set = 0, binding = 2, std430) readonly buffer BoneBuffer { GpuMechBone bones[]; } uBones;

void main() {
    GpuMechInstance inst = uInst.instances[gl_InstanceIndex];
    const bool preskinned = (inst.renderFlags & MECH_RENDER_FLAG_PRESKINNED) != 0u;   // posed by mech_skin.comp: identity, no bone fetch
    mat4 boneT = mat4(preskinned ? 1.0 : 0.0);
    for (int i = 0; i < 4 && !preskinned; ++i) {
        float w = a_boneWeights[i];
        if (w > 0.0) {
            GpuMechBone b = uBones.bones[a_boneIndices[i] + inst.baseBoneOffset];
            boneT += w * mat4(b.row0, b.row1, b.row2, b.row3);
        }
    }
    vec4 worldStuff = boneT * vec4(a_position, 1.0);
    vec3 worldMC2 = mc2ToRender(worldStuff.xyz);
    gl_Position = uFrame.lightSpaceMatrix * vec4(worldMC2, 1.0);
    // Ground vehicles (renderFlags bit 6) cast through the mover prop feed (captureMoverShadowShape), as before:
    // collapse their mech-feed copy outside the clip volume.
    if ((inst.renderFlags & MECH_RENDER_FLAG_LEGACY_VEHICLE) != 0u) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
}
