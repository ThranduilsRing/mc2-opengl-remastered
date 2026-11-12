// Shared declarations for the vegetation GPU cull (vegetation_cull / vegetation_compact .comp).
#include "vegetation_structs.glsl"
layout(set = 0, binding = 0, std140) uniform VegParams { VegParamsData P; };
layout(std430, set = 0, binding = 1) readonly buffer VegVis { uint blockVis[]; };
layout(std430, set = 0, binding = 2) readonly buffer VegInst { PackedVegInstance inst[]; };
layout(std430, set = 0, binding = 3) readonly buffer VegReg { VegRegion region[]; };
layout(std430, set = 0, binding = 5) coherent buffer VegCounts { uint counts[]; }; // survivors per region
layout(std430, set = 0, binding = 6) buffer VegOffsets { uint offsets[]; };   // exclusive prefix of counts
layout(std430, set = 0, binding = 7) buffer VegDense { uint dense[]; };       // the compact survivor id list, population order
layout(std430, set = 0, binding = 8) buffer VegArgs { uint args[]; };         // 2 x VkDrawIndirectCommand, drawn, survivor total, cull done counter, pad, VkDispatchIndirectCommand (x = visible region count)
layout(std430, set = 0, binding = 9) buffer VegVisList { uint visList[]; };   // visible region ids (vegetation_region); counts[]/offsets[] are indexed by position in this list
float vegSmooth(float a, float b, float x) {
    float t = clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
bool survives(VegInstance i, bool force) {
    uint role = (i.atlasFrame & 8u) != 0u ? 0u : (i.atlasFrame >> 4u) & 3u;
    float scale = i.scale * (role == 3u ? 0.25 : role == 2u ? 0.6 : 1.0);
    if (force) return true;
    for (int k = 0; k < 4; ++k)
        if (P.plane[k].x * i.pos.x + P.plane[k].y * i.pos.y + P.plane[k].z * i.pos.z + P.plane[k].w < -1.5 * scale * P.planeLen[k]) return false;
    uint block = vegBlockIndex(i, P);
    uint visibility = block < P.counts.z ? blockVis[block] : 2u;
    return visibility != 0u && vegLodScale(i, P) > 0.0;
}
