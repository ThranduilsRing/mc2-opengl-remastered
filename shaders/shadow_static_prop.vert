// Depth-only instanced static-prop shadow. Mirrors static_prop.vert
// legacy (non-coalesce) SSBO path. v*M row-vector order (Stuff std430
// col-major), then Stuff->MC2 swizzle - matches static_prop.vert, NOT
// shadow_object.vert's column-vector convention.
//
// NOTE: the non-coalesce branch in static_prop.vert indexes instances_.i[gl_InstanceID]
// (bare, no u_instanceBase). The SSBO range is bound per-type by the caller
// (Task 3), so gl_InstanceID is relative to the bound range start.
layout(location=0) in vec3 a_position;
// SHADOW-PROP-ALPHA-1: forward UVs so the dedicated prop shadow frag
// (shadow_static_prop.frag, program "shadow_static_prop_alpha") can alpha-test
// the foliage card. The shared static-prop VAO already enables attribute 2
// (a_uv, kVertexStride offset 24 — gos_static_prop_batcher.cpp ~2263). The
// empty shadow_instanced.frag (program "shadow_static_prop") ignores v_uv, so
// adding this output is harmless for that pairing.
layout(location=2) in vec2 a_uv;
out vec2 v_uv;

// Per-instance record (112B): generated from RenderCore/gpu_abi/GpuStaticPropInstance.h.
// Only modelMatrix is read here.
#include <include/gpu_abi_static_prop_instance.gen.hglsl>
#include <include/axis.hglsl>
layout(std430, binding=0) readonly buffer Instances { GpuStaticPropInstance i[]; } instances_;

uniform mat4 lightSpaceMatrix;
// SSBO-BIND-ALIGN: the caller now binds the WHOLE instance buffer at offset 0
// (always aligned) and passes the per-type range start here, instead of a
// glBindBufferRange at i*sizeof(Instance). i*112 is not a multiple of
// GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT (32 on this NVIDIA), which the driver
// rejects with GL_INVALID_VALUE -> the SSBO never binds -> shadow casters drop.
// Default 0 (kept compatible with any caller that still binds a per-type range).
uniform int u_instBase;

void main() {
    GpuStaticPropInstance inst = instances_.i[u_instBase + gl_InstanceID];
    vec4 worldStuff = vec4(a_position, 1.0) * inst.modelMatrix;
    vec3 worldMC2 = mc2ToRender(worldStuff.xyz);
    gl_Position = lightSpaceMatrix * vec4(worldMC2, 1.0);
    v_uv = a_uv;
}
