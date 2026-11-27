// Population records and the per-frame cull parameters, shared by the compute stages and the vegetation vertex shader.
// LOCKSTEP with mc2veg::GpuInstance / GpuRegion / CullParams in GameOS/gameos/vk_vegetation_state.h.
struct PackedVegInstance { vec2 xy; uint heightScale; uint appearance; }; // 16 B
struct VegInstance { vec3 pos; float yaw; float scale; uint atlasFrame; float seed; };
VegInstance unpackVegetation(PackedVegInstance p) {
    vec2 hs = unpackHalf2x16(p.heightScale);
    return VegInstance(vec3(p.xy, hs.x), float((p.appearance >> 16u) & 255u) * (6.28318530718 / 256.0),
                       hs.y, p.appearance >> 24u, unpackHalf2x16(p.appearance & 65535u).x);
}
struct VegRegion { vec4 sphere; uint first; uint last; uint pad0; uint pad1; };                    // 32 B
struct VegParamsData {
    vec4 plane[4];    // clip-space frustum planes a,b,c,d (left,right,bottom,top), as the CPU loop built them
    vec4 planeLen;    // |abc| per plane
    vec4 clipRow;     // mvp[3], mvp[7], mvp[11], mvp[15]: clip w of a point
    vec4 cam;         // camera chunk-space x,y,z ; MC2_VEG_MAX_DIST
    vec4 misc;        // fadeStart, pixelScale, max(lightDirZ,0), time
    vec4 grid;        // mapHalfWU, blockSideWU, 0, 0
    uvec4 counts;     // regionCount, instanceCount, blockVisibilityCount, chunkSide
    uvec4 flags;      // force-visible, unused, stock trim enabled, visible capacity
};

uint vegBlockIndex(VegInstance i, VegParamsData p) {
    int side = int(p.counts.w);
    if (side <= 0 || p.grid.y <= 0.0) return 0xffffffffu;
    int col = clamp(int((i.pos.x + p.grid.x) / p.grid.y), 0, side - 1);
    int row = clamp(int((-i.pos.y + p.grid.x) / p.grid.y), 0, side - 1);
    return uint(row * side + col);
}

// Projected card height in pixels (the cull's cover metric; the vertex stage widens sub-2 px field blades).
float vegPixels(VegInstance i, VegParamsData p) {
    // Field roles mark fringe draw outcomes, never population/LOD changes.
    uint role = (i.atlasFrame & 8u) != 0u ? 0u : (i.atlasFrame >> 4u) & 3u;
    float height = i.scale * (role == 3u ? 0.25 : role == 2u ? 0.6 : 1.0);
    float clipW = dot(p.clipRow, vec4(i.pos, 1.0));
    return clipW <= 0.001 ? 0.0 : height * p.misc.y / clipW;
}

// One rank and one continuous scale, shared by the cull and draw. No alpha
// fade, frame-dependent noise or separate binary distance/population tests.
float vegLodScale(VegInstance i, VegParamsData p) {
    // The cooked map-cell population is the world budget. Keep its full density;
    // the persistent seed supplies the rank, independent of the camera.
    const float density = 1.0;
    float rank = fract(i.seed * 17.371 + 0.137);
    return rank < density ? 1.0 : 0.0;
}
