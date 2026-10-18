//---------------------------------------------------------------------------
// xform_conventions.h
//
// XFORM-CONVENTION-HARNESS-1: the two canonical clip-space convention
// matrices, lifted out of camera.cpp into a GL-free leaf TU so a host
// unit test (tests/unit/test_xform_convention.cpp) can link them WITHOUT
// dragging in GL-coupled camera.cpp.
//
// These functions define MC2's transform/clip-space contract:
//   * makeAxisSwapMC2toGL()    -- the MC2 (x, ground, elevation) -> GL
//                                 (-x, elevation, ground) basis permutation,
//                                 an INTENTIONAL handedness change (det == -1).
//   * makePixelHomogToGLNDC()  -- the D3D-pixel-homogeneous -> GL-NDC fix
//                                 (Y-flip + reverse-Z + w-sign), post-
//                                 multiplied into cameraToClipGL.
//
// Behaviour-preserving move: identical math, identical Stuff::Matrix4D types,
// same anonymous-namespace-free symbols so camera.cpp can delegate to them.
// Do NOT "correct" the signs to stock OpenGL conventions -- they match the
// legacy upload bit-for-bit (see the long-form rationale in camera.cpp's
// historical comments, preserved in xform_conventions.cpp).
//---------------------------------------------------------------------------

#ifndef XFORM_CONVENTIONS_H
#define XFORM_CONVENTIONS_H

#include <stuff/stuff.hpp>   // Stuff::Matrix4D (pure CPU, GL-free)

namespace mc2xform {

// MC2 -> GL basis permutation:
//   GL.x = -MC2.x          (negated)
//   GL.y =  MC2.elevation  (was z; elevation -> up)
//   GL.z =  MC2.ground     (was y; ground -> forward, POSITIVE)
// Odd permutation + one negation => determinant == -1 (handedness change).
glm::mat4 makeAxisSwapMC2toGL();

// D3D-pixel-homogeneous -> GL-NDC conversion (Stuff row-vector convention).
// Carries the Y-flip, the reverse-Z mapping, and the w-sign so an in-front
// MC2 vertex yields clip.w > 0.
glm::mat4 makePixelHomogToGLNDC();

} // namespace mc2xform

#endif // XFORM_CONVENTIONS_H
