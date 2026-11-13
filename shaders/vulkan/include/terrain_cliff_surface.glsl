// One spatial visual surface for patch corners, skirts and material normals.
// Gameplay heights and the shipped visual-height payload remain untouched.
#include "../../include/terrain_cliff_math.hglsl"
float cliffRawHeight(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(uT.visualSide - 1));
    return heightsFine[p.x + p.y * uT.visualSide];
}
float cliffVisualHeight(ivec2 p) {
    float c = cliffRawHeight(p);
    float n = cliffRawHeight(p + ivec2(0, -2)), s = cliffRawHeight(p + ivec2(0, 2));
    float e = cliffRawHeight(p + ivec2(2, 0)), w = cliffRawHeight(p + ivec2(-2, 0));
    float rise = max(max(abs(n - c), abs(s - c)), max(abs(e - c), abs(w - c)));
    if (rise <= 24.0) return c;
    float diagonal = cliffRawHeight(p + ivec2(-2, -2)) + cliffRawHeight(p + ivec2(2, -2))
                   + cliffRawHeight(p + ivec2(-2, 2)) + cliffRawHeight(p + ivec2(2, 2));
    return cliffFilteredHeight(c, n + s + e + w, diagonal, rise);
}
