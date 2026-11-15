// TERRAIN-HIRES-NEAR-1: mode-1 (LOD0) terrain vertex at the 4x visual-height bake density.
// Shared by terrain_tier1.vert and terrain_tier1_skirt.vert. Needs, declared before the
// include: uT (frame UBO), uP (patch constants), sampleH(), heightsFine[], TERRAIN_DEPTH_FUDGE.
//
// Local offsets are in FINE units (the C++ side builds the patch mesh at 4*quadCount).
// Coarse vertices (fine coords divisible by 4) land on hiresCornerH(), the exact height a
// mode-2 (coarser band) vertex Z-swaps to, so LOD0 and the far bands share their corners.
// Edges stitched to a coarser neighbour (edgeStitch byte Sc, in coarse units) are linear
// between those corner heights, matching the neighbour's straight edge: no cracks.

#include "terrain_visual_surface.glsl"

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
            int   c1 = min(c0 + Sc, alongX ? uP.quadCountX : uP.quadCountY);   // partial edge: mc2lod::edgeHeight
            float t  = float(along - c0 * 4) / float((c1 - c0) * 4);
            if (alongX) {
                int cy = uP.blockOriginY + lo.y / 4;
                return mix(hiresCornerH(uP.blockOriginX + c0, cy), hiresCornerH(uP.blockOriginX + c1, cy), t);
            }
            int cx = uP.blockOriginX + lo.x / 4;
            return mix(hiresCornerH(cx, uP.blockOriginY + c0), hiresCornerH(cx, uP.blockOriginY + c1), t);
        }
    }
    return hiresSurfaceHeight(fx, fy);
}

// terrain-sse-lod-2 geomorph. uP.morph = 4x6-bit corner weights (NW bits 0-5, NE 6-11,
// SW 12-17, SE 18-23) + the next coarser level's stride in coarse cells (bits 24-31).
// Corners next to a stitched, skirted, off-map or differently-levelled block carry 0, so
// those edges never move; a shared same-level edge interpolates the same two corner
// weights from both sides.
float terrainMorphW(vec2 uv) {
    float nw = float(uP.morph & 63), ne = float((uP.morph >> 6) & 63);
    float sw = float((uP.morph >> 12) & 63), se = float((uP.morph >> 18) & 63);
    return mix(mix(nw, ne, uv.x), mix(sw, se, uv.x), uv.y) / 63.0;
}

// Coarse-vertex height the next coarser level draws: the Z-swapped corner when displaced
// (mode 1/2), else the coarse height field (mode 0).
float terrainParentCornerH(int cx, int cy, bool disp) {
    return disp ? hiresCornerH(cx, cy) : sampleH(cx, cy);
}

// Height of the stride-TF TL-BR parent surface at block-local fine position loF (block is
// qxF x qyF fine cells; fine = 4 per coarse cell; parent vertices are multiples of TF).
// Mirrors mc2lod::parentSurfaceHeight (terrain_lod_rules.h, doctested).
float terrainParentH(ivec2 loF, int qxF, int qyF, int TF, bool disp) {
    int x0 = (loF.x / TF) * TF, y0 = (loF.y / TF) * TF;
    if (x0 >= qxF) x0 = ((qxF - 1) / TF) * TF;
    if (y0 >= qyF) y0 = ((qyF - 1) / TF) * TF;
    int x1 = min(x0 + TF, qxF), y1 = min(y0 + TF, qyF);
    float u = float(loF.x - x0) / float(x1 - x0), v = float(loF.y - y0) / float(y1 - y0);
    int bx = uP.blockOriginX, by = uP.blockOriginY;
    float tl = terrainParentCornerH(bx + x0 / 4, by + y0 / 4, disp), tr = terrainParentCornerH(bx + x1 / 4, by + y0 / 4, disp);
    float bl = terrainParentCornerH(bx + x0 / 4, by + y1 / 4, disp), br = terrainParentCornerH(bx + x1 / 4, by + y1 / 4, disp);
    return (u >= v) ? tl + u * (tr - tl) + v * (br - tr) : tl + v * (bl - tl) + u * (br - bl);
}

// Geomorphed height of a vertex at block-local fine position loF.
float terrainMorphH(float h, ivec2 loF, bool disp) {
    int T = (uP.morph >> 24) & 0xFF;
    if (T <= 0) return h;
    int qxF = uP.quadCountX * 4, qyF = uP.quadCountY * 4;
    float w = terrainMorphW(vec2(loF) / vec2(qxF, qyF));
    return w > 0.0 ? mix(h, terrainParentH(loF, qxF, qyF, T * 4, disp), w) : h;
}

// Clip position of a mode-1 vertex; `drop` pulls skirt bottoms below the surface.
vec4 hiresNearClip(ivec2 lo, bool stitch, float drop, out vec3 worldPos) {
    int fx, fy;
    float h = terrainMorphH(hiresNearHeight(lo, stitch, fx, fy), lo, true) - drop;
    worldPos = vec3(float(fx) * 32.0 - uT.halfMap, uT.halfMap - float(fy) * 32.0, h);
    vec4 c = uT.worldToClipGL * vec4(worldPos, 1.0);
    c.z += 2.0 * TERRAIN_DEPTH_FUDGE * c.w;   // FUDGE=0 -> no-op (lockstep with the coarse path)
    return c;
}
