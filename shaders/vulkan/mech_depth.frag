#version 450
#extension GL_GOOGLE_include_directive : require
#include "include/mech_frame_ubo_vk.glsl"
#define MC2_TEX_SET 2
#include "include/texture_table_vk.glsl"
#define uAlbedo MC2_TEX(uRun.albedoAo & 65535u, MC2_SAMP_REPEAT_BIASED)
layout(push_constant) uniform MechRunPC { uint materialFlags; uint poolFirst; uint poolCount; uint albedoAo; uint normal; } uRun;
layout(location=0) in vec2 vUv;
void main() {
    if(uFrame.materialReady!=0 && (uRun.materialFlags&1u)!=0u && texture(uAlbedo,vUv).a<0.5)discard;
}
