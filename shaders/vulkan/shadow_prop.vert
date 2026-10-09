// VK-P8a SHADOW-INTEROP — depth-only INSTANCED static-prop shadow-caster VS.
// =============================================================================
// This is the LIVE geometry-region VS. It reproduces the GL producer shaders/shadow_static_prop.vert
// math BIT-FOR-BIT so a VK-produced D32 shadow map matches the GL-produced one:
//
//   GL (shadow_static_prop.vert):
//     vec4 worldStuff = vec4(a_position, 1.0) * inst.modelMatrix;   // row-vector * M
//     vec3 worldMC2   = vec3(-worldStuff.x, worldStuff.z, worldStuff.y); // Stuff->MC2
//     gl_Position     = lightSpaceMatrix * vec4(worldMC2, 1.0);
//
// LOCKSTEP with C++ GpuStaticPropInstance (gos_static_prop_batcher.h, 112B,
// static_assert'd) — the VK instance SSBO is a byte-identical memcpy of the SAME
// std430 POD the GL path binds, so `inst.modelMatrix` is loaded from the identical
// 16 floats. GL uploaded modelMatrix with GL_FALSE (no transpose); GLSL fills a
// mat4 from linear memory column-major, so `vec4(pos,1) * modelMatrix` here is the
// exact same operation as the GL VS. lightSpaceMatrix is the engine's
// getDynamicLightSpaceMatrix() (also GL_FALSE upload), carried in the per-frame UBO
// as 16 linear floats; GLSL loads it column-major, and the GL VS applies it as
// `lightSpaceMatrix * v` (matrix * column vector) — reproduced identically here.
//
// The whole file compiles to SPIR-V at build time (glslang/glslc). Vulkan reverse-Z
// is NOT used for shadows — the GL dynamic shadow map is forward-Z [0,1] GL_LESS, so
// the pipeline uses depthCompareOp=LESS and z stays [0,1]. Vulkan's clip space is
// already y-down/z[0,1]; gl_Position here is the light-space clip position and the
// depth written is gl_Position.z/w, matching GL calcShadow's stored convention.
#version 450
#extension GL_GOOGLE_include_directive : require

// Shared static-prop VBO: position at location 0 (kVertexStride, offset 0). The
// GL path also forwards a_uv at location 2 for the foliage alpha-test program;
// P8a is OPAQUE-ONLY (empty frag), so only position is consumed here.
layout(location = 0) in vec3 a_position;

// Per-frame UBO: the dynamic light-space matrix (16 linear floats, GL_FALSE upload)
// + the per-type instance-base offset (mirrors the GL u_instBase uniform, since the
// caller sorts instances by typeID and draws each type-run with a base offset).
layout(set = 0, binding = 0, std140) uniform ShadowFrameUBO {
    mat4 lightSpaceMatrix;   // getDynamicLightSpaceMatrix() — GL_FALSE (column-major load)
    int  instBase;           // per-type-run base index into the instance SSBO
    int  _pad0;
    int  _pad1;
    int  _pad2;
} uFrame;

// Instance SSBO (set 0 binding 1 — same single descriptor set as the UBO, matching
// the C++ one-set / two-binding layout). Byte-identical to C++ GpuStaticPropInstance
// (112B, alignas(16)).
// Only modelMatrix is read (the shadow VS ignores the rest), but the FULL 112-byte
// layout is kept so gl_InstanceIndex strides match the memcpy'd POD exactly.
#include "../include/gpu_abi_static_prop_instance.gen.hglsl"
layout(set = 0, binding = 1, std430) readonly buffer Instances {
    GpuStaticPropInstance i[];
} uInstances;

void main() {
    GpuStaticPropInstance inst = uInstances.i[uFrame.instBase + gl_InstanceIndex];
    // Row-vector * M — identical to the GL VS (Stuff std430 column-major convention).
    vec4 worldStuff = vec4(a_position, 1.0) * inst.modelMatrix;
    // Stuff -> MC2 frame swizzle — identical to the GL VS.
    vec3 worldMC2 = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);
    gl_Position = uFrame.lightSpaceMatrix * vec4(worldMC2, 1.0);
}
