// Vegetation crossed-card vertex shader fed by the GPU cull (vegetation_cull/scan/compact.comp).
// Draw: two indirect strips of eight vertices (first vertex 0 and 8), one instance per survivor. gl_InstanceIndex indexes the dense survivor
// list, which names a persistent population record. The per-frame values the CPU used to bake into a 64 B vertex per survivor (distance, wind
// fade, light z, time) come from the cull parameters. Output interface and fragment shader are vfx_particle.frag, unchanged.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0, std140) uniform VfxVsParams {
    mat4 worldToClipGL;   // SAME 16 raw floats as GL GL_FALSE upload
    vec4 camRight;
    vec4 camUp;
} U;

#include "include/vegetation_structs.glsl"
layout(std430, set = 2, binding = 0) readonly buffer VegInst { VegInstance inst[]; };
layout(std430, set = 2, binding = 1) readonly buffer VegDense { uint dense[]; };
layout(set = 2, binding = 2, std140) uniform VegDrawParams { VegParamsData D; };   // the frame's cull parameters

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_flags;
layout(location = 3) out vec4 v_vegData;
layout(location = 4) out vec2 v_vegAux;
#include "include/vegetation_vk.glsl"

void main() {
    VegInstance i = inst[dense[gl_InstanceIndex]];
    float dist3 = length(i.pos - D.cam.xyz);
    float fadeStart = D.misc.x;
    float windFade = 1.0 - smoothstep(fadeStart * 0.8, fadeStart, dist3);
    v_color = vec4(0.0);
    mc2VegetationVertex(i.pos, i.yaw, i.scale, i.seed, i.atlasFrame, 1.0, windFade, D.misc.z, D.misc.w);
}
