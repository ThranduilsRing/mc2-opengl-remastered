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

// Field carpet share kept by distance: full to 1500 WU, thinning by rank to 35% by 4500 WU so a low,
// long view stays inside the visible list. The vertex stage widens the survivors by 1/sqrt of it, so the
// gameplay-camera sward (~2600 WU, owner ref 3) keeps its cover instead of opening onto bare ground.
float vegFieldThin(VegInstance i, VegParamsData p) {
    return mix(1.0, 0.35, smoothstep(1500.0, 4500.0, length(i.pos - p.cam.xyz)));
}

// Projected card height in pixels (the cull's cover metric; the vertex stage widens sub-2 px field blades).
float vegPixels(VegInstance i, VegParamsData p) {
    uint role = (i.atlasFrame >> 4u) & 3u;
    float height = i.scale * (role == 3u ? 0.25 : role == 2u ? 0.6 : 1.0);
    float clipW = dot(p.clipRow, vec4(i.pos, 1.0));
    return clipW <= 0.001 ? 0.0 : height * p.misc.y / clipW;
}

// One rank and one continuous scale, shared by the cull and draw. No alpha
// fade, frame-dependent noise or separate binary distance/population tests.
float vegLodScale(VegInstance i, VegParamsData p) {
    if (p.flags.x != 0u) return 1.0;
    float clipW = dot(p.clipRow, vec4(i.pos, 1.0));
    if (clipW <= 0.001) return 0.0;
    float pixels = vegPixels(i, p);
    float distanceFade = 1.0 - smoothstep(p.misc.x, p.cam.w, length(i.pos - p.cam.xyz));
    // Terrain LOD is discrete; only the continuous cover metric controls density.
    // Grass (meadow-field blades, atlas bit 3, and clump grass, layers 0-3) is the carpet (owner ref 3:
    // "actually add grass", then "speckly / not solid - add density"): field blades stay down to ~0.25 px
    // (the vertex stage widens them), clump grass to ~0.5 px. Woody cards keep 3-12 px.
    bool fieldCard = (i.atlasFrame & 8u) != 0u;
    bool grassCard = fieldCard || (i.atlasFrame & 15u) < 4u;
    float thin = fieldCard ? vegFieldThin(i, p) : 1.0;
    float cover = fieldCard ? smoothstep(0.25, 1.0, pixels) : smoothstep(0.5, 2.0, pixels);
    float density = grassCard ? min(min(distanceFade, thin), cover)
                              : min(distanceFade, smoothstep(3.0, 12.0, pixels));
    float rank = fract(i.seed * 17.371 + 0.137);
    return clamp((density - rank) / 0.15, 0.0, 1.0);
}
