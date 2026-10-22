// Shared declarations for the vegetation GPU cull (vegetation_cull / vegetation_scan / vegetation_compact .comp).
#include "vegetation_structs.glsl"
layout(set = 0, binding = 0, std140) uniform VegParams { VegParamsData P; };
layout(std430, set = 0, binding = 1) readonly buffer VegVis { uint blockVis[]; };
layout(std430, set = 0, binding = 2) readonly buffer VegInst { VegInstance inst[]; };
layout(std430, set = 0, binding = 3) readonly buffer VegReg { VegRegion region[]; };
layout(std430, set = 0, binding = 4) buffer VegScratch { uint scratch[]; };   // survivors at region.first + local rank
layout(std430, set = 0, binding = 5) buffer VegCounts { uint counts[]; };     // survivors per region
layout(std430, set = 0, binding = 6) buffer VegOffsets { uint offsets[]; };   // exclusive prefix of counts
layout(std430, set = 0, binding = 7) buffer VegDense { uint dense[]; };       // the compact survivor id list, population order
layout(std430, set = 0, binding = 8) buffer VegArgs { uint args[]; };         // 2 x VkDrawIndirectCommand, then the survivor total
float vegSmooth(float a, float b, float x) {
    float t = clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
