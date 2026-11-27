// Vegetation crossed-card vertex shader fed by the GPU cull (vegetation_cull/scan/compact.comp).
// Draw: two indirect strips of eight vertices (first vertex 0 and 8), one instance per survivor. gl_InstanceIndex indexes the dense survivor
// list, which names a persistent population record. The per-frame values the CPU used to bake into a 64 B vertex per survivor (distance, wind
// fade, light z, time) come from the cull parameters; its own fragment stage writes opaque cutout depth and normals.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0, std140) uniform VfxVsParams {
    mat4 worldToClipGL;   // SAME 16 raw floats as GL GL_FALSE upload
    vec4 camRight;
    vec4 camUp;
} U;

#include "include/vegetation_structs.glsl"
layout(std430, set = 2, binding = 0) readonly buffer VegInst { PackedVegInstance inst[]; };
layout(std430, set = 2, binding = 1) readonly buffer VegDense { uint dense[]; };
layout(set = 2, binding = 2, std140) uniform VegDrawParams { VegParamsData D; };   // the frame's cull parameters
layout(std430, set = 2, binding = 3) readonly buffer VegFootprints { uint footprintWords[]; };

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_flags;
layout(location = 3) out vec4 v_vegData;
layout(location = 4) out vec2 v_vegAux;
layout(location = 5) out vec3 v_cardNormal;
layout(location = 6) out vec3 v_worldPos;
layout(location = 7) flat out vec3 v_rootPos;
layout(location = 8) flat out uint v_atlasLayer;
layout(location = 9) out vec3 v_shadowWorld;
// The depth pre-pass and the shading pass must produce bit-identical depth (the shading pass tests EQUAL against it).
invariant gl_Position;
#include "include/vegetation_vk.glsl"

void main() {
    uint denseId = dense[gl_InstanceIndex];
    if (denseId == 0x7fffffffu) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }   // unfilled thinned slot: no geometry
    VegInstance i = unpackVegetation(inst[denseId & 0x7fffffffu]);
    // Keep rejected fringe slots in the capped list, but emit no geometry.
    if ((i.atlasFrame & 56u) == 40u) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    float windFade = 1.0;
    float shrink = vegLodScale(i, D);
    uint frame = i.atlasFrame;
    float widthScale = (frame & 8u) != 0u ? 1.6 : 1.0;
    v_color = vec4(0.0);
    // Woody cards retain their silhouette while the fragment stage fades coverage.
    bool woody = (frame & 8u) == 0u && (frame & 15u) >= 4u;
    mc2VegetationVertex(i.pos, i.yaw, i.scale * (woody ? 1.0 : shrink), i.seed, frame, shrink, windFade, D.misc.z, D.misc.w, widthScale, (denseId & 0x80000000u) != 0u, vegPixels(i, D) < 1.5);
}
