//---------------------------------------------------------------------------
// xform_conventions.cpp
//
// XFORM-CONVENTION-HARNESS-1: GL-free leaf TU holding the two canonical
// clip-space convention matrices. The bodies are moved verbatim from
// camera.cpp's former anonymous namespace (F1 unified-projection); camera.cpp
// now #includes xform_conventions.h and delegates its kAxisSwapMC2toGL /
// kPixelHomogToGLNDC constant initializers here.
//---------------------------------------------------------------------------

#include "xform_conventions.h"

namespace mc2xform {

// F1 unified-projection axisSwap literal -- transplanted from existing
// upload-site logic at code/gamecam.cpp:168-175. Per spec §2.1:
//   GL.x = -MC2.x          (negated)
//   GL.y =  MC2.elevation  (was z; elevation -> up)
//   GL.z =  MC2.ground     (was y; ground -> forward, POSITIVE)
//
// DO NOT "correct" the sign to stock OpenGL -Z forward; this matches the
// existing legacy upload bit-for-bit. Stage A-pre basis-vector test
// (Task 3) verifies empirically.
glm::mat4 makeAxisSwapMC2toGL()
{
    glm::mat4 m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m[c][r] = (r == c) ? 1.0f : 0.0f;
    m[0][0] = -1.0f;
    m[1][1] = 0.0f;
    m[2][1] = 1.0f;
    m[2][2] = 0.0f;
    m[1][2] = 1.0f;
    return m;
}

// F2 unified-projection: D3D-pixel-homogeneous -> GL-NDC conversion matrix.
// Used at camera-update time to precompute cameraToClipGL from the legacy
// cameraToClip. Not consumed in the worldToClipGL() hot path -- the
// composition is fixed once per camera config.
//
// F1 Task 24 (2026-05-22 LATE): D3D-pixel-homogeneous -> GL-NDC conversion.
//
// MC2's existing terrainMVP product (axisSwap * worldToCam * cameraToClip)
// outputs D3D-style pixel-homogeneous clip: clip.xy/clip.w in [0..1] for
// in-screen geometry, Y-down, with clip.w<0 for in-front (per cameraToClip
// at :1943 having (FORWARD_AXIS=2, col=3) = +1.0f under the -z_eye camera).
//
// The legacy shader chain (`screen.x = clip.x*rhw*viewport.x + viewport.z;
// ndc = projection_ * vec4(screen, 1); gl_Position = vec4(ndc*absW, absW);`)
// performed the D3D->GL conversion via the viewport scale + Y-flip projection_
// matrix. F1 Stage A (commit 59fae27) retired that chain in favour of direct
// emit `gl_Position = u_worldToClipGL * vec4(world,1)`, which assumed
// terrainMVP was already in standard GL clip space. It is NOT.
//
// Result of post-Stage-A user mc2_10 canary: terrain/props/mechs render
// upside down and at wrong scale (only narrow center region visible; rest
// black) because the matrix output is D3D-pixel-homog while hardware
// interprets it as GL NDC.
//
// Fix: bake the D3D->GL conversion into the matrix product as a post-
// multiply. For column-vector GLSL convention:
//   M_fix * d3d_clip -> gl_clip
// where M_fix =
//   | -2   0   0   1 |   new.x = -2*lx + lw   ndc.x = 2*lx/lw - 1
//   |  0   2   0  -1 |   new.y = 2*ly - lw    ndc.y = -2*ly/lw + 1 (Y-flip)
//   |  0   0  -1   0 |   new.z = -lz          ndc.z = lz/lw
//   |  0   0   0  -1 |   new.w = -lw          positive for in-front
//
// In Stuff (row-vector + column-major-in-memory + transpose-via-upload),
// the matrix that produces M_fix on the GLSL side is transpose(M_fix):
//   | -2   0   0   0 |
//   |  0   2   0   0 |
//   |  0   0  -1   0 |
//   |  1  -1   0  -1 |
//
// Camera::worldToClipGL() applies this as the final factor:
//   out = (axisSwap * worldToCam * cameraToClip) * kPixelHomogToGLNDC
//
// On-screen NDC matches the legacy chain to floating-point precision
// (verified by hand on the Task 7d sample: world=(-6144, 5120, 273)
// gives NDC=(-1.86, -0.56, 0.12) under both legacy and new paths).
//
// The negation that Task 7g folded into kAxisSwapMC2toGL is subsumed
// by M_fix's row 3 (which negates w directly), so kAxisSwapMC2toGL
// reverts to the pure axis-permutation literal.
glm::mat4 makePixelHomogToGLNDC()
{
    glm::mat4 m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m[c][r] = 0.0f;
    m[0][0] = -2.0f;
    m[1][1] =  2.0f;
    m[2][2] = -1.0f;
    m[0][3] =  1.0f;
    m[1][3] = -1.0f;
    m[3][3] = -1.0f;
    return m;
}

} // namespace mc2xform
