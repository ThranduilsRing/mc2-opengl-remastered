// One spatial visual surface for patch corners, skirts and vegetation roots.
#include "../../include/terrain_cliff_math.hglsl"
#ifdef __cplusplus
template<class Raw> inline float cliffVisualHeight(Raw raw, int x, int y) {
#define CLIFF_RAW(dx, dy) raw(x + dx, y + dy)
    using std::abs; using std::max;
#else
float cliffRawHeight(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(uT.visualSide - 1));
    return heightsFine[p.x + p.y * uT.visualSide];
}
float cliffVisualHeight(ivec2 p) {
#define CLIFF_RAW(dx, dy) cliffRawHeight(p + ivec2(dx, dy))
#endif
    float c = CLIFF_RAW(0, 0);
    float n = CLIFF_RAW(0, -2), s = CLIFF_RAW(0, 2);
    float e = CLIFF_RAW(2, 0), w = CLIFF_RAW(-2, 0);
    float rise = max(max(abs(n - c), abs(s - c)), max(abs(e - c), abs(w - c)));
    if (rise <= 24.0) return c;
    float diagonal = CLIFF_RAW(-2, -2) + CLIFF_RAW(2, -2)
                   + CLIFF_RAW(-2, 2) + CLIFF_RAW(2, 2);
    return cliffFilteredHeight(c, n + s + e + w, diagonal, rise);
}
#undef CLIFF_RAW
