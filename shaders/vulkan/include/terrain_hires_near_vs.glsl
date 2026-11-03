// TERRAIN-HIRES-NEAR-1: mode-1 (LOD0) terrain vertex at the 4x visual-height bake density.
// Shared by terrain_tier1.vert and terrain_tier1_skirt.vert. Needs, declared before the
// include: uT (frame UBO), uP (patch constants), sampleH(), heightsFine[], TERRAIN_DEPTH_FUDGE.
//
// Local offsets are in FINE units (the C++ side builds the patch mesh at 4*quadCount).
// Coarse vertices (fine coords divisible by 4) land on hiresCornerH(), the exact height a
// mode-2 (coarser band) vertex Z-swaps to, so LOD0 and the far bands share their corners.
// Edges stitched to a coarser neighbour (edgeStitch byte Sc, in coarse units) are linear
// between those corner heights, matching the neighbour's straight edge: no cracks.

float hiresBlendW() { return clamp(uT.visualDisplaceFar, 0.0, 1.0); }

float hiresFineAt(int fx, int fy) {
    fx = clamp(fx, 0, uT.visualSide - 1);
    fy = clamp(fy, 0, uT.visualSide - 1);
    return heightsFine[fx + fy * uT.visualSide];
}

// Displaced height at a coarse vertex (what a mode-2 band draws there).
float hiresCornerH(int cx, int cy) {
    cx = clamp(cx, 0, uT.mapSide - 1);
    cy = clamp(cy, 0, uT.mapSide - 1);
    return mix(sampleH(cx, cy), hiresFineAt(cx * 4, cy * 4), hiresBlendW());
}

float hiresNearHeight(ivec2 lo, bool stitch, out int fx, out int fy) {
    int maxF = (uT.mapSide - 1) * 4;
    fx = clamp(uP.blockOriginX * 4 + lo.x, 0, maxF);
    fy = clamp(uP.blockOriginY * 4 + lo.y, 0, maxF);
    if (stitch && uP.edgeStitch != 0) {
        int qx = uP.quadCountX * 4, qy = uP.quadCountY * 4;
        int Sc = 0, along = 0;
        bool alongX = true;
        if      (lo.y == 0  && ((uP.edgeStitch)       & 0xFF) > 0) { Sc = (uP.edgeStitch)       & 0xFF; along = lo.x; alongX = true;  }
        else if (lo.y == qy && ((uP.edgeStitch >> 8)  & 0xFF) > 0) { Sc = (uP.edgeStitch >> 8)  & 0xFF; along = lo.x; alongX = true;  }
        else if (lo.x == 0  && ((uP.edgeStitch >> 16) & 0xFF) > 0) { Sc = (uP.edgeStitch >> 16) & 0xFF; along = lo.y; alongX = false; }
        else if (lo.x == qx && ((uP.edgeStitch >> 24) & 0xFF) > 0) { Sc = (uP.edgeStitch >> 24) & 0xFF; along = lo.y; alongX = false; }
        int ScF = Sc * 4;
        if (ScF > 0 && (along % ScF) != 0) {
            int   c0 = (along / ScF) * Sc;
            int   c1 = c0 + Sc;
            float t  = float(along - c0 * 4) / float(ScF);
            if (alongX) {
                int cy = uP.blockOriginY + lo.y / 4;
                return mix(hiresCornerH(uP.blockOriginX + c0, cy), hiresCornerH(uP.blockOriginX + c1, cy), t);
            }
            int cx = uP.blockOriginX + lo.x / 4;
            return mix(hiresCornerH(cx, uP.blockOriginY + c0), hiresCornerH(cx, uP.blockOriginY + c1), t);
        }
    }
    // Coarse surface under the vertex, TL-BR diagonal like the coarse patch triangulation
    // (only matters when MC2_TERRAIN_VISUAL_DISPLACE_FAR < 1).
    int   cx = min(fx / 4, uT.mapSide - 2);
    int   cy = min(fy / 4, uT.mapSide - 2);
    float u  = float(fx - cx * 4) * 0.25;
    float v  = float(fy - cy * 4) * 0.25;
    float h00 = sampleH(cx, cy), h10 = sampleH(cx + 1, cy);
    float h01 = sampleH(cx, cy + 1), h11 = sampleH(cx + 1, cy + 1);
    float hc = (v >= u) ? h00 + (h11 - h01) * u + (h01 - h00) * v
                        : h00 + (h10 - h00) * u + (h11 - h10) * v;
    return mix(hc, hiresFineAt(fx, fy), hiresBlendW());
}

// Clip position of a mode-1 vertex; `drop` pulls skirt bottoms below the surface.
vec4 hiresNearClip(ivec2 lo, bool stitch, float drop, out vec3 worldPos) {
    int fx, fy;
    float h = hiresNearHeight(lo, stitch, fx, fy) - drop;
    worldPos = vec3(float(fx) * 32.0 - uT.halfMap, uT.halfMap - float(fy) * 32.0, h);
    vec4 c = uT.worldToClipGL * vec4(worldPos, 1.0);
    c.z += 2.0 * TERRAIN_DEPTH_FUDGE * c.w;   // FUDGE=0 -> no-op (lockstep with the coarse path)
    return c;
}
