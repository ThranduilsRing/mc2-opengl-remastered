#version 450
#extension GL_GOOGLE_include_directive : require
#include "include/mech_frame_ubo_vk.glsl"
layout(set=0,binding=3) uniform sampler2D uAlbedo;
layout(push_constant) uniform MechRunPC { uint materialFlags; uint poolFirst; uint poolCount; } uRun;
layout(location=0) in vec2 vUv;
void main() {
    if(uFrame.materialReady!=0 && (uRun.materialFlags&1u)!=0u && texture(uAlbedo,vUv).a<0.5)discard;
}
