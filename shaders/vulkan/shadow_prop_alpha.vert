// VK alpha-tested static-prop shadow-caster VS: the twin of shaders/shadow_static_prop.vert (GL), which also forwards the vertex UV so the fragment shader can cut the
// leaf silhouette. The position math is the opaque caster's (shadow_prop.vert), including its per-frame UBO and instance record; only the UV is added.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 a_position;
layout(location = 2) in vec2 a_uv;

layout(set = 0, binding = 0, std140) uniform ShadowFrameUBO {
    mat4 lightSpaceMatrix;
    int  instBase;
    int  _pad0;
    int  _pad1;
    int  _pad2;
} uFrame;

#include "../include/gpu_abi_static_prop_instance.gen.hglsl"
#include "../include/axis.hglsl"
layout(set = 0, binding = 1, std430) readonly buffer Instances {
    GpuStaticPropInstance i[];
} uInstances;

layout(location = 0) out vec2 v_uv;

void main() {
    GpuStaticPropInstance inst = uInstances.i[uFrame.instBase + gl_InstanceIndex];
    vec4 worldStuff = vec4(a_position, 1.0) * inst.modelMatrix;
    vec3 worldMC2 = mc2ToRender(worldStuff.xyz);
    gl_Position = uFrame.lightSpaceMatrix * vec4(worldMC2, 1.0);
    v_uv = a_uv;
}
