#version 450
#extension GL_GOOGLE_include_directive : require
invariant gl_Position;
layout(location=0) in vec3 a_position;
layout(location=2) in vec2 a_uv;
layout(location=3) in uvec4 a_boneIndices;
layout(location=4) in vec4 a_boneWeights;
#include "include/mech_frame_ubo_vk.glsl"
#include "../include/gpu_abi_mech_instance.gen.hglsl"
#include "../include/axis.hglsl"
#include "../include/mech_render_flags.hglsl"
layout(set=0,binding=1,std430) readonly buffer InstanceBuffer { GpuMechInstance instances[]; } uInst;
struct GpuMechBone { vec4 row0,row1,row2,row3; };
layout(set=0,binding=2,std430) readonly buffer BoneBuffer { GpuMechBone bones[]; } uBones;
layout(location=0) out vec2 vUv;
void main() {
    GpuMechInstance inst=uInst.instances[gl_InstanceIndex];
    const bool preskinned=(inst.renderFlags&MECH_RENDER_FLAG_PRESKINNED)!=0u;   // posed by mech_skin.comp: identity, no bone fetch
    mat4 boneT=mat4(preskinned?1.0:0.0);
    for(int bi=0;bi<4&&!preskinned;++bi) {
        float w=a_boneWeights[bi];
        if(w>0.0) {
            GpuMechBone bn=uBones.bones[a_boneIndices[bi]+inst.baseBoneOffset];
            boneT+=w*mat4(bn.row0,bn.row1,bn.row2,bn.row3);
        }
    }
    vec4 worldStuff=boneT*vec4(a_position,1.0);
    vec3 worldMC2=mc2ToRender(worldStuff.xyz);
    vec4 clip=uFrame.worldToClipGL*vec4(worldMC2,1.0);
    clip.z+=uFrame.objectDepthBias*clip.z;
    gl_Position=clip;
    vUv=a_uv;
}
