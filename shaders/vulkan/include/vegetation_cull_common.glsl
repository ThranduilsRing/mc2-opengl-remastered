// Shared declarations for the vegetation GPU cull (vegetation_cull / vegetation_scan / vegetation_compact .comp)
// and the draw-side vertex shader. LOCKSTEP with mc2veg::GpuInstance / GpuRegion / CullParams in
// GameOS/gameos/vk_vegetation_state.h.
struct VegInstance { vec3 pos; float yaw; float scale; uint atlasFrame; float seed; uint pad; };   // 32 B
struct VegRegion { vec4 sphere; uint first; uint last; uint pad0; uint pad1; };                    // 32 B
layout(set = 0, binding = 0, std140) uniform VegParams {
    vec4 plane[4];    // clip-space frustum planes a,b,c,d (left,right,bottom,top), as the CPU loop built them
    vec4 planeLen;    // |abc| per plane
    vec4 clipRow;     // mvp[3], mvp[7], mvp[11], mvp[15]: clip w of a point
    vec4 cam;         // camera chunk-space x,y,z ; MC2_VEG_MAX_DIST
    vec4 misc;        // fadeStart, pixelScale, max(lightDirZ,0), time
    vec4 grid;        // mapHalfWU, blockSideWU, 0, 0
    uvec4 counts;     // regionCount, instanceCount, blockVisibilityCount, chunkSide
    uvec4 flags;      // force-visible, dispatch grid width, 0, 0
} P;
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
