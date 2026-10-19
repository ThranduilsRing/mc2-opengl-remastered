// mclib/camera_frustum_math.h
//
// Pure camera geometry math extracted (CAMERA-FRUSTUM-HARNESS-1) so it can be
// exercised game-free by tools/camera_frustum_harness/ — no Camera instance, no
// GL, no Stuff math types, no game headers. Production (Camera::quadAabbInFrustum,
// the s_pointInScreenTri tile test, gos_Compute43Box) delegates to these,
// adapting its Stuff::Vector3D / __stdcall signatures at the call boundary, so
// every existing call site is unchanged and behavior is byte-identical.
//
// Firewall: header-only, no includes, no globals.

#ifndef MC2_CAMERA_FRUSTUM_MATH_H
#define MC2_CAMERA_FRUSTUM_MATH_H

namespace camera_frustum_math {

// Replica of mclib/mathfunc.h sign2: -1 for negative, 0 otherwise (NOT +1).
inline float sign2f(float f) { return (f < 0.0f) ? -1.0f : 0.0f; }

// Conservative p-vertex AABB-vs-frustum test. planes[6][4] are world-space
// half-spaces (a,b,c,d) admitting where a*x+b*y+c*z+d >= 0 (Gribb-Hartmann,
// unnormalized — sign only). mn/mx are the AABB corners as {x,y,z}. Never
// false-negative. Identical arithmetic to Camera::quadAabbInFrustum.
inline bool aabbInFrustum(const float planes[6][4],
                          const float mn[3], const float mx[3]) {
    for (int p = 0; p < 6; ++p) {
        const float a = planes[p][0], b = planes[p][1],
                    c = planes[p][2], d = planes[p][3];
        const float px = (a >= 0.0f) ? mx[0] : mn[0];
        const float py = (b >= 0.0f) ? mx[1] : mn[1];
        const float pz = (c >= 0.0f) ? mx[2] : mn[2];
        if (a * px + b * py + c * pz + d < 0.0f)
            return false;
    }
    return true;
}

// [LOW-CAMERA-TERRAIN-CULL-1 / FIX-2] Variant that skips one plane index
// (e.g. the near plane, index 4 in the {left,right,bottom,top,near,far} order).
// Used by the terrain LOD-chunk cull so near terrain is not dropped at a low /
// grazing camera pitch (the near plane sits at NearPlaneDistance=-400, clipping
// terrain blocks within ~400 units of the eye). All other planes are tested
// identically to aabbInFrustum(); pass skipPlane<0 to test all 6 (== aabbInFrustum).
inline bool aabbInFrustumSkipPlane(const float planes[6][4],
                                   const float mn[3], const float mx[3],
                                   int skipPlane) {
    for (int p = 0; p < 6; ++p) {
        if (p == skipPlane) continue;
        const float a = planes[p][0], b = planes[p][1],
                    c = planes[p][2], d = planes[p][3];
        const float px = (a >= 0.0f) ? mx[0] : mn[0];
        const float py = (b >= 0.0f) ? mx[1] : mn[1];
        const float pz = (c >= 0.0f) ? mx[2] : mn[2];
        if (a * px + b * py + c * pz + d < 0.0f)
            return false;
    }
    return true;
}

// Screen-space point-in-triangle (the body of s_pointInScreenTri) on plain
// screen-XY floats. Returns true if (mouseX,mouseY) is inside triangle
// (v0,v1,v2). Identical arithmetic to the original.
inline bool pointInScreenTri(float v0x, float v0y, float v1x, float v1y,
                             float v2x, float v2y, long mouseX, long mouseY) {
    const float l1x = v0x - v1x, l1y = v0y - v1y;
    const float l2x = v1x - v2x, l2y = v1y - v2y;
    const float order = sign2f(l2x * l1y - l1x * l2y);

    const float A0 = -(v0y - v1y), B0 = (v0x - v1x);
    const float C0 = -B0 * v0y - A0 * v0x;
    const float D0 = A0 * mouseX + B0 * mouseY + C0;

    const float A1 = -(v1y - v2y), B1 = (v1x - v2x);
    const float C1 = -B1 * v1y - A1 * v1x;
    const float D1 = A1 * mouseX + B1 * mouseY + C1;

    const float A2 = -(v2y - v0y), B2 = (v2x - v0x);
    const float C2 = -B2 * v2y - A2 * v2x;
    const float D2 = A2 * mouseX + B2 * mouseY + C2;

    return (sign2f(D0) == order) && (sign2f(D0) == sign2f(D1)) &&
           (sign2f(D0) == sign2f(D2));
}

// 4:3 pillarbox/letterbox rect for a w x h surface. Writes the centered box and
// returns true if it is a real box (not the full surface). Pure math half of
// gos_Compute43Box (the MC2_FORCE_43 env gate stays in the caller). Identical
// arithmetic to the original's active branch.
inline bool compute43BoxRect(int w, int h, int* ox, int* oy, int* obw, int* obh) {
    int rx = 0, ry = 0, rw = w, rh = h;
    bool active = (w > 0 && h > 0);
    if (active) {
        const double kTarget = 4.0 / 3.0;
        const double aspect = static_cast<double>(w) / static_cast<double>(h);
        if (aspect > kTarget) { rh = h; rw = static_cast<int>((static_cast<double>(h) * kTarget + 0.5)); }
        else if (aspect < kTarget) { rw = w; rh = static_cast<int>((static_cast<double>(w) / kTarget + 0.5)); }
        rx = (w - rw) / 2; ry = (h - rh) / 2;
        if (rw == w && rh == h) active = false;
    }
    if (ox) *ox = rx;
    if (oy) *oy = ry;
    if (obw) *obw = rw;
    if (obh) *obh = rh;
    return active;
}

}  // namespace camera_frustum_math

#endif  // MC2_CAMERA_FRUSTUM_MATH_H
