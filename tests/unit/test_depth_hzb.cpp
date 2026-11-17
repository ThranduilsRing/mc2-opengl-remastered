// tests/unit/test_depth_hzb.cpp
// GL-free CPU reference tests that pin the
// reverse-Z / Hi-Z (HZB) depth contract BEFORE any runtime HZB pyramid or
// occlusion-culling code exists.
//
// WHY THIS FILE EXISTS
// --------------------
// A wrong HZB min/max reduction convention does not crash -- it silently
// over-culls visible geometry, producing intermittent invisible-object bugs
// that are extremely expensive to diagnose later. This file is semantic
// infrastructure: it encodes the depth contract as executable, CI-checked
// assertions so the runtime HZB implementation has a fixed target to match.
//
// PROJECT DEPTH CONTRACT (do NOT change without re-verifying):
//   * Reverse-Z:        near plane -> depth 1.0, far plane -> depth 0.0.
//                       LARGER depth value == CLOSER to the camera.
//                       (gameosmain.cpp glClearDepth(0.0f); gameos_graphics.cpp
//                        glDepthFunc(GL_GREATER); appear.h HUD_DEPTH 0.9999f;
//                        terrain_depth_bias.h "GEQUAL: the LARGER NDC z wins".)
//   * Clip / NDC depth: ZERO_TO_ONE  (glClipControl(GL_LOWER_LEFT,
//                       GL_ZERO_TO_ONE) -- mandatory / fail-closed). Window
//                       depth and NDC z both live in [0,1].
//   * Sky / far / no-geometry sentinel: depth == 0.0 (clear value).
//
// All math below is LOCAL REFERENCE MATH. The project has no depth-linearize
// or HZB-reduce helper yet. When the runtime HZB lands it
// MUST reproduce these exact rules; if a reusable helper is later promoted
// into RenderCore, port these tests to call it instead of the locals here.
#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Reference depth math (reverse-Z, ZERO_TO_ONE). Local to the test; mirrors
// the regime the engine enforces at runtime.
// ---------------------------------------------------------------------------

// Map a positive view-space distance (eye units, +ahead) to reverse-Z
// ZERO_TO_ONE window depth for a standard perspective projection.
//
//   forward-Z 0..1 :  z01 = far/(far-near) * (1 - near/d)
//   reverse-Z 0..1 :  depth = 1 - z01   (near->1, far->0)
//
// This is the *intuition* model used by the tests -- the runtime uses the GPU
// projection matrix, but the near=1/far=0 endpoints and monotonic ordering
// below are the contract that matters for HZB.
double reverseZDepthFromDistance(double dist, double nearD, double farD) {
    const double z01 = (farD / (farD - nearD)) * (1.0 - nearD / dist);
    return 1.0 - z01;
}

// "Is a closer than b?" under the project's reverse-Z convention.
bool isCloser(double depthA, double depthB) { return depthA > depthB; }

// Sky / far / no-geometry test. Far == clear == 0.0.
bool isSkyOrFar(double depth) { return depth <= 0.0; }

// ---- HZB reduction --------------------------------------------------------
//
// CONSERVATIVE OCCLUSION REDUCTION FOR REVERSE-Z == min().
//
// Derivation (the whole reason this file exists):
//   The scene depth buffer holds, per pixel, the NEAREST surface -> the
//   LARGEST reverse-Z value. An object is occluded across a tile iff its
//   closest point lies behind the occluder at EVERY pixel of the tile. The
//   WEAKEST occluder (the one that hides the least) is the farthest-back
//   surface in the tile == the SMALLEST depth value. To never cull a visible
//   object, the coarse HZB texel must store that weakest/farthest occluder,
//   i.e. the MINIMUM child depth. So the reverse-Z HZB is a MIN pyramid.
//
//   Cull test then reads: cull iff  objClosestDepth (max over footprint)
//                                   <  hzbTexel (min over footprint).
//
//   Forward-Z (near=0/far=1) would store MAX. Reverse-Z FLIPS this to MIN.
//   Storing MAX here would record the nearest occluder and over-cull objects
//   that are actually visible behind a thin foreground sliver -> invisible
//   objects. That inversion is the entire trap this file guards against.
double hzbReduce2x2(double a, double b, double c, double d) {
    return std::min(std::min(a, b), std::min(c, d));
}

// Conservative reverse-Z cull decision (matches the comment above).
// objClosestDepth = max reverse-Z depth over the object's screen footprint.
// hzbTexel        = reduced (min) occluder depth for the covered tile.
bool hzbCullsObject(double objClosestDepth, double hzbTexel) {
    return objClosestDepth < hzbTexel;
}

// ---- Mip dimension ladder -------------------------------------------------
//
// Each level halves each dimension INDEPENDENTLY and ROUNDS UP (ceil), floored
// at 1, so the coarse level fully covers the finer one with a clamped 2x2
// fetch -- no source texel is ever dropped on an odd extent. This differs from
// GL's native glGenerateMipmap, which (a) AVERAGES (wrong: HZB needs min) and
// (b) floors the size. The runtime HZB therefore needs a custom reduction
// pass; it cannot lean on glGenerateMipmap. Round-up keeps the chain
// conservative without per-level 3x3 edge taps.
uint32_t hzbNextMipDim(uint32_t d) {
    if (d <= 1u) return 1u;            // 1 is the terminal, idempotent extent
    return (d + 1u) / 2u;             // ceil(d/2)
}

// Full ladder for a single axis, terminating at and INCLUDING the 1 extent.
std::vector<uint32_t> hzbMipLadder(uint32_t base) {
    std::vector<uint32_t> levels;
    uint32_t d = base == 0u ? 0u : base;
    if (d == 0u) return levels;       // degenerate; no levels
    levels.push_back(d);
    while (d > 1u) {
        d = hzbNextMipDim(d);
        levels.push_back(d);
    }
    return levels;
}

// Number of mip levels in a 2D HZB pyramid for a WxH
// base (ceil ladder down to 1x1). This is exactly the level count the runtime
// allocates via glTexStorage2D. It equals the runtime's compact form that
// halves max(w,h) by ceil until it reaches 1; the test also asserts it equals
// the LONGER per-axis ladder length.
uint32_t hzbMipCount2D(uint32_t w, uint32_t h) {
    uint32_t maxDim = (w > h) ? w : h;
    if (maxDim == 0u) return 0u;
    uint32_t mips = 1u;
    while (maxDim > 1u) { maxDim = hzbNextMipDim(maxDim); ++mips; }
    return mips;
}

} // namespace

TEST_SUITE("DepthHZB") {

// --- Depth convention ------------------------------------------------------

TEST_CASE("DepthConvention reverse Z maps near to one and far to zero") {
    const double nearD = 1.0;
    const double farD  = 1000.0;

    const double dNear = reverseZDepthFromDistance(nearD, nearD, farD);
    const double dFar  = reverseZDepthFromDistance(farD,  nearD, farD);

    CHECK(dNear == doctest::Approx(1.0)); // near plane -> 1.0
    CHECK(dFar  == doctest::Approx(0.0)); // far  plane -> 0.0
}

TEST_CASE("DepthConvention ZERO_TO_ONE keeps depth inside the unit range") {
    const double nearD = 1.0;
    const double farD  = 1000.0;
    // Sample across the frustum; every window-depth must stay in [0,1].
    for (double dist = nearD; dist <= farD; dist += 7.3) {
        const double depth = reverseZDepthFromDistance(dist, nearD, farD);
        CHECK(depth >= -1e-9);
        CHECK(depth <= 1.0 + 1e-9);
    }
}

TEST_CASE("DepthConvention larger depth means closer") {
    const double nearD = 1.0;
    const double farD  = 1000.0;

    const double dClose = reverseZDepthFromDistance(10.0,  nearD, farD);
    const double dMid   = reverseZDepthFromDistance(100.0, nearD, farD);
    const double dFar   = reverseZDepthFromDistance(900.0, nearD, farD);

    // Monotonic: nearer surfaces carry strictly larger reverse-Z depth.
    CHECK(dClose > dMid);
    CHECK(dMid   > dFar);

    CHECK(isCloser(dClose, dFar));
    CHECK_FALSE(isCloser(dFar, dClose));
}

TEST_CASE("DepthConvention far and sky sentinel is zero") {
    CHECK(isSkyOrFar(0.0));          // clear value / no geometry
    CHECK_FALSE(isSkyOrFar(0.25));   // real geometry is > 0
    CHECK_FALSE(isSkyOrFar(1.0));    // near plane is decidedly not sky
}

// --- HZB reduction convention ----------------------------------------------

TEST_CASE("HZB reverse Z parent stores farthest occluder via min reduction") {
    // Four child depths; smallest == farthest occluder under reverse-Z.
    const double nearest  = 0.90; // closest surface in the tile
    const double farthest = 0.10; // weakest occluder in the tile
    const double parent = hzbReduce2x2(nearest, 0.55, farthest, 0.40);

    // The conservative reverse-Z HZB keeps the farthest (min), NOT the nearest.
    CHECK(parent == doctest::Approx(farthest));
    CHECK(parent != doctest::Approx(nearest));
}

TEST_CASE("HZB conservative cull never hides a visible object") {
    // Tile occluders range from a thin near sliver (0.95) to a far wall (0.30).
    // Conservative reduction must keep the FAR wall (0.30) so an object that
    // pokes out behind the sliver is NOT culled.
    const double hzb = hzbReduce2x2(0.95, 0.80, 0.60, 0.30);
    CHECK(hzb == doctest::Approx(0.30));

    // Object whose closest point (0.50) is NEARER than the kept occluder
    // (0.30 is farther) -> visible -> must NOT be culled.
    CHECK_FALSE(hzbCullsObject(/*objClosest=*/0.50, hzb));

    // A MAX reduction would have stored 0.95 and wrongly culled this object:
    const double wrongMaxHzb = std::max(std::max(0.95, 0.80), std::max(0.60, 0.30));
    CHECK(hzbCullsObject(/*objClosest=*/0.50, wrongMaxHzb)); // the invisible-object bug
}

TEST_CASE("HZB conservative cull rejects a fully occluded object") {
    // Whole tile is a near wall; the farthest occluder is still close (0.70).
    const double hzb = hzbReduce2x2(0.85, 0.80, 0.75, 0.70);
    CHECK(hzb == doctest::Approx(0.70));

    // Object entirely behind that wall (closest point 0.40 < 0.70) -> occluded.
    CHECK(hzbCullsObject(/*objClosest=*/0.40, hzb));
}

TEST_CASE("HZB reduction is order independent and idempotent") {
    CHECK(hzbReduce2x2(0.1, 0.2, 0.3, 0.4) == doctest::Approx(0.1));
    CHECK(hzbReduce2x2(0.4, 0.3, 0.2, 0.1) == doctest::Approx(0.1));
    // Uniform tile reduces to itself (a flat region stays flat up the pyramid).
    CHECK(hzbReduce2x2(0.5, 0.5, 0.5, 0.5) == doctest::Approx(0.5));
}

// --- Mip dimension ladder ---------------------------------------------------

TEST_CASE("HZB mip dimensions halve for even extents") {
    CHECK(hzbNextMipDim(8) == 4u);
    CHECK(hzbNextMipDim(4) == 2u);
    CHECK(hzbNextMipDim(2) == 1u);
    CHECK(hzbMipLadder(8) == std::vector<uint32_t>({8, 4, 2, 1}));
}

TEST_CASE("HZB mip dimensions round up for odd extents") {
    CHECK(hzbNextMipDim(7) == 4u);  // ceil(3.5) -> 4, not floor 3
    CHECK(hzbNextMipDim(5) == 3u);
    CHECK(hzbNextMipDim(3) == 2u);
    // 7 reaches 1 via 4,2; 5 via 3,2. No source texel dropped on the way.
    CHECK(hzbMipLadder(7) == std::vector<uint32_t>({7, 4, 2, 1}));
    CHECK(hzbMipLadder(5) == std::vector<uint32_t>({5, 3, 2, 1}));
}

TEST_CASE("HZB mip dimensions reduce each axis independently for 1xN and Nx1") {
    // A 1xN (or Nx1) target collapses the long axis while the unit axis stays 1.
    CHECK(hzbMipLadder(1) == std::vector<uint32_t>({1}));
    CHECK(hzbMipLadder(6) == std::vector<uint32_t>({6, 3, 2, 1}));
    // e.g. a 1x6 image: rows ladder = {1}, cols ladder = {6,3,2,1}; the chain
    // length is max(1, 4) = 4 levels, the unit axis clamped at 1 throughout.
}

TEST_CASE("HZB 1x1 terminal mip is stable") {
    CHECK(hzbNextMipDim(1) == 1u);   // 1 reduces to 1 (idempotent terminal)
    CHECK(hzbNextMipDim(0) == 1u);   // defensive: never emit a 0 extent
    CHECK(hzbMipLadder(1) == std::vector<uint32_t>({1}));
    // Re-reducing the terminal must not grow or oscillate the chain.
    CHECK(hzbNextMipDim(hzbNextMipDim(1)) == 1u);
}

// --- 2D mip count (the level count the runtime allocates) -------------------

TEST_CASE("HZB 2D mip count equals the longer per-axis ladder") {
    // Concrete display resolutions (runtime allocates this many R32F levels).
    CHECK(hzbMipCount2D(1920, 1080) == 12u); // ladder(1920): 1920..1 = 12
    CHECK(hzbMipCount2D(1280, 720)  == 12u); // ladder(1280): 1280..1 = 12
    CHECK(hzbMipCount2D(512, 512)   == 10u); // 512..1 = 10
    CHECK(hzbMipCount2D(1, 1)       == 1u);  // single-texel target

    // The compact max(w,h) form must agree with the longer per-axis ladder.
    const uint32_t cases[][2] = {
        {1920, 1080}, {1280, 720}, {7, 3}, {1, 64}, {64, 1}, {5, 5}, {1, 1}
    };
    for (const auto& c : cases) {
        const uint32_t expected =
            (uint32_t)std::max(hzbMipLadder(c[0]).size(), hzbMipLadder(c[1]).size());
        CHECK(hzbMipCount2D(c[0], c[1]) == expected);
    }
}

TEST_CASE("HZB 2D mip count reduces non-square extents to a 1x1 terminal") {
    // A 1xN / Nx1 target collapses the long axis; count = long-axis ladder.
    CHECK(hzbMipCount2D(1, 64) == hzbMipCount2D(64, 1));
    CHECK(hzbMipCount2D(1, 64) == (uint32_t)hzbMipLadder(64).size());
    // Odd long axis still terminates (ceil ladder, no dropped texel).
    CHECK(hzbMipCount2D(7, 3) == (uint32_t)hzbMipLadder(7).size());
}

// --- Occlusion probe decision -------------------------------------------------

TEST_CASE("HZB probe: a tile min never culls its own child texels") {
    // The runtime self-consistency probe asserts exactly this: a parent texel
    // stores MIN over the tile, and every child depth in the tile is >= that
    // min (reverse-Z), so the conservative cull comparison must KEEP all of
    // them. wouldCull over self-points must be 0.
    const double children[4] = { 0.90, 0.55, 0.10, 0.40 };
    const double parent = hzbReduce2x2(children[0], children[1], children[2], children[3]);
    for (double d : children) {
        // hzbCullsObject(objClosest=d, hzbTexel=parent) must be false: d >= min.
        CHECK_FALSE(hzbCullsObject(d, parent));
    }
}

TEST_CASE("HZB probe: an object strictly farther than the tile min is culled") {
    const double parent = hzbReduce2x2(0.90, 0.55, 0.30, 0.40); // min = 0.30
    CHECK(parent == doctest::Approx(0.30));
    // Farther than the farthest occluder (smaller reverse-Z) -> occluded.
    CHECK(hzbCullsObject(/*objClosest=*/0.20, parent));
    // Exactly at / nearer than the min -> kept (boundary is conservative-keep).
    CHECK_FALSE(hzbCullsObject(/*objClosest=*/0.30, parent));
    CHECK_FALSE(hzbCullsObject(/*objClosest=*/0.95, parent));
}

} // TEST_SUITE("DepthHZB")

#include "RenderCore/hzb_prop_bounds.h"
#include <limits>

TEST_CASE("HZB prop bounds enclose off-centre geometry and preserve render axes") {
    GpuStaticPropInstance instance{};
    for (int i = 0; i < 4; ++i) instance.modelMatrix[i*5] = 1;
    instance.modelMatrix[3] = .2f; instance.modelMatrix[7] = .1f; instance.modelMatrix[11] = .3f;
    float clip[16] = {};
    for (int i = 0; i < 4; ++i) clip[i*5] = 1;
    clip[14] = .5f;
    const mc2hzb::PropBounds box[] = {{{-.1f,-.1f,-.1f},{.1f,.1f,.1f},true}};
    const auto projected = mc2hzb::project_prop(instance,clip,.001f,box,101,79,8);
    CHECK(projected.keep == 0);
    CHECK(projected.nearest == doctest::Approx(.701f));
    // Stuff translation (.2,.1,.3) becomes render (-.2,.3,.1).
    CHECK(projected.pixels[0] <= .35f*101);
    CHECK(projected.pixels[2] >= .45f*101);
    CHECK(projected.pixels[1] <= .60f*79);
    CHECK(projected.pixels[3] >= .70f*79);
    CHECK(projected.lod > 0);
    CHECK((1u << projected.lod) >= projected.pixels[2]-projected.pixels[0]);
    CHECK((1u << projected.lod) >= projected.pixels[3]-projected.pixels[1]);
}

TEST_CASE("HZB prop bounds keep near-clipped, offscreen, missing and invalid geometry") {
    GpuStaticPropInstance instance{};
    for (int i = 0; i < 4; ++i) instance.modelMatrix[i*5] = 1;
    float clip[16] = {};
    for (int i = 0; i < 4; ++i) clip[i*5] = 1;
    clip[14] = .5f;
    const mc2hzb::PropBounds box[] = {{{-.1f,-.1f,-.1f},{.1f,.1f,.1f},true}};
    SUBCASE("near plane") { instance.modelMatrix[7] = .5f; }
    SUBCASE("screen edge") { instance.modelMatrix[3] = 1.f; }
    SUBCASE("missing type") { instance.typeID = 1; }
    SUBCASE("not finite") { clip[0] = std::numeric_limits<float>::quiet_NaN(); }
    const auto projected = mc2hzb::project_prop(instance,clip,0,box,101,79,8);
    CHECK(projected.keep == 1);
}

TEST_CASE("HZB prop bounds wholly behind the eye are not force-kept") {
    // intro-alpha-props-1: homogeneous rejection drops bounds whose every corner has w < 0
    // (nothing can rasterize), so keep is 0 (not force-kept); near-plane straddlers stay kept (case above).
    GpuStaticPropInstance instance{};
    for (int i = 0; i < 4; ++i) instance.modelMatrix[i*5] = 1;
    float clip[16] = {};
    for (int i = 0; i < 4; ++i) clip[i*5] = 1;
    clip[14] = .5f;
    clip[15] = -1;
    const mc2hzb::PropBounds box[] = {{{-.1f,-.1f,-.1f},{.1f,.1f,.1f},true}};
    const auto projected = mc2hzb::project_prop(instance,clip,0,box,101,79,8);
    CHECK(projected.keep == 0);
}
