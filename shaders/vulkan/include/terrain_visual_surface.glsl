// Terrain vertices and water coverage share the visual bake, cliff filter and border blend.
#include "terrain_cliff_surface.glsl"
float hiresBlendW() { return clamp(uT.visualDisplaceFar, 0.0, 1.0); }
float hiresEdgeW(int fx, int fy) {
    int maxF = (uT.mapSide - 1) * 4;
    int d = min(min(fx, fy), min(maxF - fx, maxF - fy));
    return clamp(float(d) * 0.125, 0.0, 1.0);
}
float hiresFineAt(int fx, int fy) { return cliffVisualHeight(ivec2(fx, fy)); }
float hiresCornerH(int cx, int cy) {
    cx = clamp(cx, 0, uT.mapSide - 1);
    cy = clamp(cy, 0, uT.mapSide - 1);
    return mix(sampleH(cx, cy), hiresFineAt(cx * 4, cy * 4), hiresBlendW() * hiresEdgeW(cx * 4, cy * 4));
}
float hiresSurfaceHeight(int fx, int fy) {
    int cx = min(fx / 4, uT.mapSide - 2), cy = min(fy / 4, uT.mapSide - 2);
    float u = float(fx - cx * 4) * 0.25, v = float(fy - cy * 4) * 0.25;
    float h00 = sampleH(cx, cy), h10 = sampleH(cx + 1, cy);
    float h01 = sampleH(cx, cy + 1), h11 = sampleH(cx + 1, cy + 1);
    float hc = (v >= u) ? h00 + (h11 - h01) * u + (h01 - h00) * v
                        : h00 + (h10 - h00) * u + (h11 - h10) * v;
    return mix(hc, hiresFineAt(fx, fy), hiresBlendW() * hiresEdgeW(fx, fy));
}
