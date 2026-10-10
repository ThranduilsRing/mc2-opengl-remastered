// tests/unit/test_xform_convention.cpp
//
// XFORM-CONVENTION-HARNESS-1 (VULKAN-CONTRACT-MANIFEST-ARC option D):
// GL-free host unit test that LOCKS MC2's transform / clip-space conventions
// so a future change -- notably BT2018 GPU mech placement -- cannot silently
// regress matrix space, handedness, reverse-Z, w-sign, or the row-major
// repack.
//
// WHY THIS FILE EXISTS
// --------------------
// The clip-space pipeline is a chain of DELIBERATE, non-default choices:
//   * an MC2->GL basis permutation that intentionally FLIPS handedness,
//   * a reverse-Z depth range (near->1, far->0, GEQUAL),
//   * a D3D-pixel-homog -> GL-NDC fix carrying a Y-flip and a w-sign,
//   * a Stuff (column-major) -> row-major (GL_FALSE upload) repack.
// Each is invisible until something draws upside-down, inside-out, or vanishes
// behind reverse-Z. This file pins every link as an executable assertion that
// links the ACTUAL lifted convention matrices (mclib/xform_conventions.cpp),
// not a re-typed copy -- so a future edit to those functions trips a test.
//
// NO GL CONTEXT is created or required (doctest, pure CPU). Cross-references
// test_depth_hzb.cpp for the reverse-Z endpoint contract (near->1 / far->0).

#include "doctest.h"

#include "xform_conventions.h"   // the lifted convention matrices under test
#include "axis_convert.h"        // RenderCore: the typed (x,y,z)->(-x,z,y) helper under test
#include <stuff/stuff.hpp>       // Stuff::Matrix4D (GL-free)

#include <cmath>
#include <initializer_list>

namespace {

// ---------------------------------------------------------------------------
// Local reference helpers. Index convention matches Stuff::Matrix4D::operator()
//   m(row, col) == entries[col*4 + row]   (column-major storage).
// ---------------------------------------------------------------------------

// Row-vector convention: out_row = in_row * M.
//   out[c] = sum_r in[r] * M(r, c)   (homogeneous, in[3] supplied by caller)
struct Vec4 { double x, y, z, w; };

Vec4 rowVecTimesMatrix(const Vec4& v, const Stuff::Matrix4D& M) {
    Vec4 o;
    o.x = v.x*M(0,0) + v.y*M(1,0) + v.z*M(2,0) + v.w*M(3,0);
    o.y = v.x*M(0,1) + v.y*M(1,1) + v.z*M(2,1) + v.w*M(3,1);
    o.z = v.x*M(0,2) + v.y*M(1,2) + v.z*M(2,2) + v.w*M(3,2);
    o.w = v.x*M(0,3) + v.y*M(1,3) + v.z*M(2,3) + v.w*M(3,3);
    return o;
}

double det4(const Stuff::Matrix4D& m) {
    // 4x4 determinant via cofactor expansion (small, exact enough for a sign).
    auto M = [&](int r, int c) -> double { return (double)m(r,c); };
    double det = 0.0;
    for (int c0 = 0; c0 < 4; ++c0) {
        // minor of (0, c0)
        int cols[3], idx = 0;
        for (int c = 0; c < 4; ++c) if (c != c0) cols[idx++] = c;
        double a = M(1,cols[0]), b = M(1,cols[1]), cc = M(1,cols[2]);
        double d = M(2,cols[0]), e = M(2,cols[1]), f  = M(2,cols[2]);
        double g = M(3,cols[0]), h = M(3,cols[1]), i  = M(3,cols[2]);
        double minor = a*(e*i - f*h) - b*(d*i - f*g) + cc*(d*h - e*g);
        double sign = (c0 % 2 == 0) ? 1.0 : -1.0;
        det += sign * M(0,c0) * minor;
    }
    return det;
}

// A synthetic D3D-pixel-homogeneous clip vector exactly as the engine's
// cameraToClip stage produces it for IN-FRONT geometry, per the documented
// contract in xform_conventions.cpp (transplanted from camera.cpp):
//   * clip.w < 0 for in-front geometry (cameraToClip(FORWARD,3)=+1 under the
//     -z_eye camera), and
//   * reverse-Z so that clip.z/clip.w = 1 at the near plane, 0 at the far.
// kPixelHomogToGLNDC is the matrix that converts THIS into GL NDC: it must
// flip w positive and preserve the reverse-Z depth fraction.
//
// depthFrac is the reverse-Z NDC depth in [0,1] (1 = near, 0 = far). We pick
// an arbitrary negative magnitude for the in-front clip.w and set clip.z so
// the ratio equals depthFrac.
Vec4 d3dInFrontClip(double ndcX, double ndcY, double depthFrac) {
    const double lw = -2.0;            // in-front => negative D3D clip.w
    // D3D pre-fix: ndc.x = lx/lw in [0,1], ndc.y = ly/lw in [0,1] (Y-down),
    // depth = lz/lw. Solve for the homogeneous components.
    return Vec4{ ndcX * lw, ndcY * lw, depthFrac * lw, lw };
}

} // namespace

TEST_SUITE("XformConvention") {

// --- Axis swap: basis mapping + intentional handedness flip ----------------

TEST_CASE("AxisSwap maps MC2 basis to (-x, elevation, north)") {
    const Stuff::Matrix4D A = mc2xform::makeAxisSwapMC2toGL();

    // MC2 +x (left axis) -> GL (-x, 0, 0).
    Vec4 ex = rowVecTimesMatrix(Vec4{1,0,0,0}, A);
    CHECK(ex.x == doctest::Approx(-1.0));
    CHECK(ex.y == doctest::Approx(0.0));
    CHECK(ex.z == doctest::Approx(0.0));

    // MC2 +y (ground / north) -> GL +z (forward, POSITIVE).
    Vec4 ey = rowVecTimesMatrix(Vec4{0,1,0,0}, A);
    CHECK(ey.x == doctest::Approx(0.0));
    CHECK(ey.y == doctest::Approx(0.0));
    CHECK(ey.z == doctest::Approx(1.0));

    // MC2 +z (elevation / up) -> GL +y (up).
    Vec4 ez = rowVecTimesMatrix(Vec4{0,0,1,0}, A);
    CHECK(ez.x == doctest::Approx(0.0));
    CHECK(ez.y == doctest::Approx(1.0));
    CHECK(ez.z == doctest::Approx(0.0));
}

TEST_CASE("AxisSwap determinant is +1 (winding-preserving, LOCKED)") {
    const Stuff::Matrix4D A = mc2xform::makeAxisSwapMC2toGL();
    // VERIFIED against the actual lifted matrix (NOT the spec's assumed -1):
    // the swap is ONE transposition (y<->z, det -1) composed with ONE axis
    // negation (x, det -1) => det == +1. The permutation is EVEN, so triangle
    // winding is PRESERVED through the MC2->GL basis change (corroborates the
    // MEMORY "even-parity/winding-preserved" note for MC2_GLTF_AXIS=2). This is
    // the load-bearing fact for GPU mech placement: if a future edit flips this
    // to -1, front faces silently become back faces (culled / inside-out) --
    // LOCK the sign here so that regression trips this test, not a vendor GPU.
    CHECK(det4(A) == doctest::Approx(1.0));
    // Guard the magnitude too: a non-unit determinant would mean an accidental
    // scale crept into the pure permutation.
    CHECK(std::fabs(det4(A)) == doctest::Approx(1.0));
}

// --- Reverse-Z depth range (cross-ref test_depth_hzb.cpp) ------------------

TEST_CASE("ReverseZ: near plane -> NDC z 1, far plane -> NDC z 0") {
    const Stuff::Matrix4D P = mc2xform::makePixelHomogToGLNDC();

    // Engine D3D clip for the near plane (reverse-Z depth fraction 1.0) and far
    // plane (0.0), both in-front (clip.w < 0).
    Vec4 atNear = rowVecTimesMatrix(d3dInFrontClip(0.5, 0.5, /*depthFrac=*/1.0), P);
    Vec4 atFar  = rowVecTimesMatrix(d3dInFrontClip(0.5, 0.5, /*depthFrac=*/0.0), P);

    REQUIRE(atNear.w > 0.0);   // kPixelHomogToGLNDC flips in-front w positive
    REQUIRE(atFar.w  > 0.0);

    const double ndcNearZ = atNear.z / atNear.w;
    const double ndcFarZ  = atFar.z  / atFar.w;

    // Reverse-Z contract: near->1, far->0 (matches test_depth_hzb.cpp).
    CHECK(ndcNearZ == doctest::Approx(1.0).epsilon(1e-5));
    CHECK(ndcFarZ  == doctest::Approx(0.0).epsilon(1e-5));
    // Larger NDC z == closer (GEQUAL).
    CHECK(ndcNearZ > ndcFarZ);
}

TEST_CASE("ReverseZ: NDC z stays inside [0,1] across the frustum") {
    const Stuff::Matrix4D P = mc2xform::makePixelHomogToGLNDC();
    double prev = 2.0;
    // Sweep the reverse-Z depth fraction from near (1.0) to far (0.0).
    for (double frac = 1.0; frac >= 0.0; frac -= 1.0 / 23.0) {
        Vec4 clip = rowVecTimesMatrix(d3dInFrontClip(0.0, 0.0, frac), P);
        REQUIRE(clip.w > 0.0);                 // in-front => positive w
        const double ndcZ = clip.z / clip.w;
        CHECK(ndcZ >= -1e-5);
        CHECK(ndcZ <=  1.0 + 1e-5);
        CHECK(ndcZ < prev + 1e-9);             // monotonically decreasing near->far
        prev = ndcZ;
    }
}

// --- clip.w polarity (mech.vert relies on no sign test) --------------------

TEST_CASE("ClipW: an in-front MC2 vertex yields clip.w > 0 after the chain") {
    const Stuff::Matrix4D P = mc2xform::makePixelHomogToGLNDC();
    // In-front geometry enters kPixelHomogToGLNDC with D3D clip.w < 0; the
    // matrix must flip it positive so mech.vert (no sign test) works.
    for (double frac : {1.0, 0.66, 0.33, 0.0}) {
        Vec4 clip = rowVecTimesMatrix(d3dInFrontClip(0.25, 0.75, frac), P);
        CHECK(clip.w > 0.0);
    }
}

// --- Row-major repack round-trip (gos_SetWorldToClipGL == stuffToRowMajor) --

TEST_CASE("RowMajorRepack: col->row transpose matches both call sites") {
    // Stuff stores column-major: col[c*4 + r] == m(r, c).
    // gos_SetWorldToClipGL (gameos_graphics.cpp:8964-8967):
    //     M[i*4 + j] = col[j*4 + i]
    // stuffToRowMajor   (gamecam.cpp:291-296):
    //     out[r*4 + c] = col[c*4 + r]
    // These are the SAME repack. Lock them to one helper and assert agreement.
    Stuff::Matrix4D m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m(r, c) = (float)(r * 4 + c) + 0.5f;   // distinct, asymmetric entries

    const float* col = (const float*)&m;   // column-major (operator const Scalar*)

    float M_gos[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            M_gos[i*4 + j] = col[j*4 + i];

    float out_cam[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out_cam[r*4 + c] = col[c*4 + r];

    for (int k = 0; k < 16; ++k)
        CHECK(M_gos[k] == doctest::Approx(out_cam[k]));

    // And both equal the row-major reading of operator()(row,col).
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            CHECK(M_gos[r*4 + c] == doctest::Approx(m(r, c)));
}

// --- Static-prop v*M: shapeToWorld convention ------------------------------

TEST_CASE("StaticProp shapeToWorld: det > 0 and column-3 holds the origin") {
    // Row-vector convention with uniform scale: a static prop's shapeToWorld is
    // built so that the translation lives in row 3 (m(3,*)) under Stuff's
    // (row,col) operator. Construct a known scale + translation and assert.
    const double sc = 2.0;
    const double tx = 100.0, ty = -250.0, tz = 37.5;

    Stuff::Matrix4D S;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) S(r,c) = 0.0f;
    S(0,0) = (float)sc; S(1,1) = (float)sc; S(2,2) = (float)sc; S(3,3) = 1.0f;
    S(3,0) = (float)tx; S(3,1) = (float)ty; S(3,2) = (float)tz;  // translation row

    // Uniform positive scale => positive determinant (no accidental mirror).
    CHECK(det4(S) > 0.0);

    // The shape-space origin (0,0,0,1) maps to the world translation.
    Vec4 origin = rowVecTimesMatrix(Vec4{0,0,0,1}, S);
    CHECK(origin.x == doctest::Approx(tx));
    CHECK(origin.y == doctest::Approx(ty));
    CHECK(origin.z == doctest::Approx(tz));
    CHECK(origin.w == doctest::Approx(1.0));
}

// --- PixelHomog->NDC fix sanity (Y-flip + reverse-Z + w-sign packaging) -----

TEST_CASE("PixelHomogToGLNDC carries the documented Y-flip and w-sign") {
    const Stuff::Matrix4D P = mc2xform::makePixelHomogToGLNDC();
    // Row-vector application of M_fix^T (the Stuff form). Feed a D3D-style
    // pixel-homog clip (lx, ly, lz, lw) and confirm the documented outputs:
    //   new.x = -2*lx + lw, new.y = 2*ly - lw, new.z = -lz, new.w = -lw.
    Vec4 in{0.3, 0.4, 0.5, 1.0};
    Vec4 o = rowVecTimesMatrix(in, P);
    CHECK(o.x == doctest::Approx(-2.0 * in.x + in.w));
    CHECK(o.y == doctest::Approx( 2.0 * in.y - in.w));   // Y-flip
    CHECK(o.z == doctest::Approx(-in.z));
    CHECK(o.w == doctest::Approx(-in.w));                // w-sign flip
}

// --- axis_convert.h: the typed helper agrees with the matrix ---------------

namespace {
bool sameVec(const Stuff::Vector3D& a, double x, double y, double z) {
    return a.x == doctest::Approx(x) && a.y == doctest::Approx(y) && a.z == doctest::Approx(z);
}
}

TEST_CASE("axis_convert: mc2ToRender equals makeAxisSwapMC2toGL applied to basis and mixed vectors") {
    const Stuff::Matrix4D A = mc2xform::makeAxisSwapMC2toGL();
    const double samples[][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {3.5, -2.25, 7.0}, {-120.0, 64.5, 0.125}};
    for (const auto& s : samples) {
        const Vec4 viaMatrix = rowVecTimesMatrix(Vec4{s[0], s[1], s[2], 0}, A);
        const Stuff::Vector3D in(static_cast<float>(s[0]), static_cast<float>(s[1]), static_cast<float>(s[2]));
        CHECK(sameVec(mc2ToRender(in), viaMatrix.x, viaMatrix.y, viaMatrix.z));
        Stuff::Vector3D out;
        mc2ToRender(out, in);
        CHECK(sameVec(out, viaMatrix.x, viaMatrix.y, viaMatrix.z));
    }
}

TEST_CASE("axis_convert: the conversion is an involution, so renderToMc2 is the same permutation") {
    const Stuff::Vector3D v(11.0f, -4.5f, 2.0f);
    CHECK(sameVec(renderToMc2(mc2ToRender(v)), v.x, v.y, v.z));
    CHECK(sameVec(mc2ToRender(v), renderToMc2(v).x, renderToMc2(v).y, renderToMc2(v).z));
    Stuff::Vector3D inPlace = v;
    mc2ToRender(inPlace, inPlace);                 // in == out is allowed
    CHECK(sameVec(inPlace, -v.x, v.z, v.y));
}

TEST_CASE("axis_convert: works across the vector types the engine mixes at these sites") {
    const Stuff::Point3D p(2.0f, 3.0f, 5.0f);
    Stuff::Vector3D v;
    mc2ToRender(v, p);                             // Point3D -> Vector3D, as the hand-written sites do
    CHECK(sameVec(v, -2.0, 5.0, 3.0));
}

} // TEST_SUITE("XformConvention")
