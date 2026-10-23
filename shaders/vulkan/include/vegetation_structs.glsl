// Population records and the per-frame cull parameters, shared by the compute stages and the vegetation vertex shader.
// LOCKSTEP with mc2veg::GpuInstance / GpuRegion / CullParams in GameOS/gameos/vk_vegetation_state.h.
struct VegInstance { vec3 pos; float yaw; float scale; uint atlasFrame; float seed; uint pad; };   // 32 B
struct VegRegion { vec4 sphere; uint first; uint last; uint pad0; uint pad1; };                    // 32 B
struct VegParamsData {
    vec4 plane[4];    // clip-space frustum planes a,b,c,d (left,right,bottom,top), as the CPU loop built them
    vec4 planeLen;    // |abc| per plane
    vec4 clipRow;     // mvp[3], mvp[7], mvp[11], mvp[15]: clip w of a point
    vec4 cam;         // camera chunk-space x,y,z ; MC2_VEG_MAX_DIST
    vec4 misc;        // fadeStart, pixelScale, max(lightDirZ,0), time
    vec4 grid;        // mapHalfWU, blockSideWU, 0, 0
    uvec4 counts;     // regionCount, instanceCount, blockVisibilityCount, chunkSide
    uvec4 flags;      // force-visible, dispatch grid width, stock trim enabled, 0
};

uint vegBlockIndex(VegInstance i, VegParamsData p) {
    int side = int(p.counts.w);
    if (side <= 0 || p.grid.y <= 0.0) return 0xffffffffu;
    int col = clamp(int((i.pos.x + p.grid.x) / p.grid.y), 0, side - 1);
    int row = clamp(int((-i.pos.y + p.grid.x) / p.grid.y), 0, side - 1);
    return uint(row * side + col);
}

// One rank and one continuous scale, shared by the cull and draw. No alpha
// fade, frame-dependent noise or separate binary distance/population tests.
float vegLodScale(VegInstance i, VegParamsData p) {
    if (p.flags.x != 0u) return 1.0;
    uint role = (i.atlasFrame >> 4u) & 3u;
    float height = i.scale * (role == 3u ? 0.25 : role == 2u ? 0.6 : 1.0);
    float clipW = dot(p.clipRow, vec4(i.pos, 1.0));
    if (clipW <= 0.001) return 0.0;
    float pixels = height * p.misc.y / clipW;
    float distanceFade = 1.0 - smoothstep(p.misc.x, p.cam.w, length(i.pos - p.cam.xyz));
    // Terrain LOD is discrete; only the continuous cover metric controls density.
    float density = min(distanceFade, smoothstep(3.0, 12.0, pixels));
    float rank = fract(i.seed * 17.371 + 0.137);
    return clamp((density - rank) / 0.15, 0.0, 1.0);
}
