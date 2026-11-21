// tests/unit/test_hzb_cull_reference.cpp
// Pins the CPU reference of shaders/vulkan/hzb_cull.comp (RenderCore/hzb_prop_bounds.h: project_prop + hzb_hidden)
// on synthetic reverse-Z pyramids. Every case states the expected keep bit (keep = drawn, hidden = culled).
// The runtime [CULL_PARITY v1] check compares the GPU mask with this same reference, so these cases are its oracle.
#include "doctest.h"

#include "hzb_prop_bounds.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int W = 64, H = 64, LEVELS = 7;   // 64,32,16,8,4,2,1

struct Pyramid {
    std::vector<std::vector<float>> lv;
    std::vector<int> w, h;
    // reduce: true = MIN (correct for reverse-Z), false = MAX (the historical over-cull bug)
    explicit Pyramid(float fill, bool minReduce = true) {
        w.push_back(W); h.push_back(H);
        lv.emplace_back(size_t(W) * H, fill);
        reduceFrom(minReduce);
    }
    void set(int x, int y, float v) { lv[0][size_t(y) * W + x] = v; }
    void reduceFrom(bool minReduce = true) {
        lv.resize(1); w.resize(1); h.resize(1);
        for (int l = 1; l < LEVELS; ++l) {
            const int pw = w.back(), ph = h.back();
            const int nw = (std::max)(1, (pw + 1) / 2), nh = (std::max)(1, (ph + 1) / 2);
            std::vector<float> next(size_t(nw) * nh);
            const auto& prev = lv.back();
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x) {
                    float v = minReduce ? 1.0f : 0.0f;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const float t = prev[size_t((std::min)(2 * y + dy, ph - 1)) * pw + (std::min)(2 * x + dx, pw - 1)];
                            v = minReduce ? (std::min)(v, t) : (std::max)(v, t);
                        }
                    next[size_t(y) * nw + x] = v;
                }
            lv.push_back(std::move(next)); w.push_back(nw); h.push_back(nh);
        }
    }
    float fetch(int l, int x, int y) const { return lv[size_t(l)][size_t(y) * w[size_t(l)] + x]; }
};

// Column-major GL matrix: clip.x = rx, clip.y = ry, clip.w = 1, clip.z = 0.5 + 0.01 * rz (nearest ~0.5).
struct Mat { float m[16]; };
Mat flatMatrix() {
    Mat r{}; r.m[0] = 1; r.m[5] = 1; r.m[15] = 1; r.m[2 * 4 + 2] = 0.01f; r.m[3 * 4 + 2] = 0.5f; return r;
}

GpuStaticPropInstance identityInstance() {
    GpuStaticPropInstance i{};
    i.modelMatrix[0] = i.modelMatrix[5] = i.modelMatrix[10] = i.modelMatrix[15] = 1;
    return i;
}

// A box in MC2 (stuff) space; render = (-x, z, y), so NDC x = -mc2.x, NDC y = mc2.z.
// ndcX/ndcY give the NDC half-extent rectangle; depth is fixed by mc2.y (render z).
mc2hzb::PropBounds boxNdc(float ndcX0, float ndcX1, float ndcY0, float ndcY1, float rz0 = 0, float rz1 = 0) {
    mc2hzb::PropBounds b{};
    b.lo[0] = -ndcX1; b.hi[0] = -ndcX0;
    b.lo[1] = rz0;    b.hi[1] = rz1;
    b.lo[2] = ndcY0;  b.hi[2] = ndcY1;
    b.valid = true;
    return b;
}

mc2hzb::ProjectedProp project(const mc2hzb::PropBounds& b, const Mat& m = flatMatrix(), float bias = 0) {
    const auto inst = identityInstance();
    const mc2hzb::PropBounds bounds[1] = {b};
    return mc2hzb::project_prop(inst, m.m, bias, bounds, W, H, LEVELS);
}

bool hidden(const mc2hzb::ProjectedProp& p, const Pyramid& py) {
    return mc2hzb::hzb_hidden(p, W, H, [&](int l, int x, int y) { return py.fetch(l, x, y); });
}

// NDC box covering exactly `px` window pixels starting near the screen centre (pixel (32,32)): 2 NDC units == 64 px.
mc2hzb::PropBounds boxPixels(float px) {
    const float n = px / 32.0f;
    return boxNdc(0.0f, n, 0.0f, n);
}

} // namespace

TEST_CASE("hzb ref: fully visible keeps (sky = 0 everywhere)") {
    Pyramid py(0.0f);
    const auto p = project(boxPixels(6.0f));
    CHECK(p.keep == 0);                 // reached the pyramid test
    CHECK_FALSE(hidden(p, py));         // expected keep=1
}

TEST_CASE("hzb ref: fully occluded culls (wall closer than the prop everywhere)") {
    Pyramid py(0.9f);
    const auto p = project(boxPixels(6.0f));
    CHECK(p.nearest == doctest::Approx(0.5f));
    CHECK(hidden(p, py));               // expected keep=0
}

TEST_CASE("hzb ref: prop closer than the wall stays visible") {
    Pyramid py(0.3f);
    const auto p = project(boxPixels(6.0f));
    CHECK_FALSE(hidden(p, py));         // 0.5 >= 0.3 - eps, expected keep=1
}

TEST_CASE("hzb ref: straddling the near plane keeps the whole object") {
    Pyramid py(0.9f);
    Mat m = flatMatrix();
    m.m[2 * 4 + 3] = 1.0f; m.m[3 * 4 + 3] = 0.0f;   // clip.w = rz: bounds from rz -1..1 cross w <= 0
    const auto p = project(boxNdc(0, 0.1f, 0, 0.1f, -1.0f, 1.0f), m);
    CHECK(p.keep == 1);
    CHECK_FALSE(hidden(p, py));         // expected keep=1 even against a full occluder
}

TEST_CASE("hzb ref: wholly clipped depth rejects without an occluder") {
    Pyramid py(0.0f);
    CHECK(hidden(project(boxNdc(0, 0.1f, 0, 0.1f, 60.0f, 60.0f)), py));
    CHECK(hidden(project(boxNdc(0, 0.1f, 0, 0.1f, -60.0f, -60.0f)), py));
    CHECK(project(boxNdc(0, .1f, 0, .1f, 49, 60)).keep == 1); // intersects near
    CHECK(project(boxNdc(0, .1f, 0, .1f, -60, -49)).keep == 1); // intersects far
    Mat behind = flatMatrix(); behind.m[15] = -1;
    CHECK(hidden(project(boxNdc(0, .1f, 0, .1f), behind), py));
    CHECK_FALSE(hidden(project(boxNdc(0, .1f, 0, .1f, 60, 60), flatMatrix(), -.2f), py)); // bias restores visibility
    auto inst = identityInstance(); inst.flags = 4u;
    const mc2hzb::PropBounds bounds[] = {boxNdc(0, .1f, 0, .1f, 50.5f, 50.5f)};
    auto m = flatMatrix(); m.m[2] = .01f; // leaf sway projects onto clip depth
    CHECK(mc2hzb::project_prop(inst, m.m, 0, bounds, W, H, LEVELS).keep == 1); // sway can cross near
}

TEST_CASE("hzb ref: partly off screen keeps (never an undersized clipped footprint)") {
    Pyramid py(0.9f);
    CHECK(project(boxNdc(0.5f, 1.5f, 0, 0.2f)).keep == 1);              // crosses x = 1
    CHECK(project(boxNdc(-1.0f, -0.5f, 0, 0.2f)).keep == 1);            // touches x = -1 (<= -1)
    CHECK(hidden(project(boxNdc(0.5f, 1.5f, 0, 0.2f)), py) == false);
}

TEST_CASE("hzb ref: wholly offscreen rejects without an occluder") {
    Pyramid sky(0.0f);
    for (const auto& b : {boxNdc(2, 3, 0, .2f), boxNdc(-3, -2, 0, .2f),
                          boxNdc(0, .2f, 2, 3), boxNdc(0, .2f, -3, -2)}) {
        CHECK(hidden(project(b), sky));
    }
    CHECK(project(boxNdc(1.01f, 1.02f, 0, .2f)).keep == 1); // rounding/jitter guard
    CHECK(project(boxNdc(-2, 2, -2, 2)).keep == 1); // encloses the view
}

TEST_CASE("hzb ref: offscreen rejection encloses leaf sway") {
    const auto b = boxNdc(1.2f, 1.3f, 0, .2f);
    const mc2hzb::PropBounds bounds[1] = {b};
    auto inst = identityInstance();
    const Mat m = flatMatrix();
    CHECK(mc2hzb::project_prop(inst, m.m, 0, bounds, W, H, LEVELS).tiny == 1);
    inst.flags |= 4u;
    CHECK(mc2hzb::project_prop(inst, m.m, 0, bounds, W, H, LEVELS).keep == 1);
    const mc2hzb::PropBounds farBounds[1] = {boxNdc(4, 5, 0, .2f)};
    CHECK(mc2hzb::project_prop(inst, m.m, 0, farBounds, W, H, LEVELS).tiny == 1);
    Mat perspective = m; perspective.m[3] = -1.0f; perspective.m[15] = 2.0f;
    CHECK(mc2hzb::project_prop(inst, perspective.m, 0, bounds, W, H, LEVELS).keep == 1);
    const mc2hzb::PropBounds inside[1] = {boxNdc(.1f, .2f, 0, .2f)};
    perspective = m; perspective.m[3] = 1.0f; perspective.m[15] = .5f;
    CHECK(mc2hzb::project_prop(inst, perspective.m, 0, inside, W, H, LEVELS).keep == 0); // unchanged HZB path
}

TEST_CASE("hzb ref: no bounds / invalid type keeps") {
    const auto inst = identityInstance();
    const Mat m = flatMatrix();
    mc2hzb::PropBounds invalid{};
    const mc2hzb::PropBounds bounds[1] = {invalid};
    CHECK(mc2hzb::project_prop(inst, m.m, 0, bounds, W, H, LEVELS).keep == 1);
    auto oob = identityInstance(); oob.typeID = 5;
    CHECK(mc2hzb::project_prop(oob, m.m, 0, bounds, W, H, LEVELS).keep == 1);
}

TEST_CASE("hzb ref: mip selection at footprint boundaries") {
    // pixels are floor(x0)-1 .. ceil(x1)+1, so a box of N px grows by up to 2 plus rounding; assert lod = ceil(log2(extent)).
    struct Row { float px; uint32_t lod; };
    for (const Row& r : {Row{0.0f, 0}, Row{1.0f, 2}, Row{2.0f, 2}, Row{5.0f, 3}, Row{13.0f, 4}, Row{29.0f, 5}}) {
        const auto p = project(boxPixels(r.px));
        const float extent = (std::max)(p.pixels[2] - p.pixels[0], p.pixels[3] - p.pixels[1]);
        uint32_t expect = 0;
        while (expect + 1 < uint32_t(LEVELS) && float(1u << expect) < extent) ++expect;
        CAPTURE(r.px);
        CAPTURE(extent);
        CHECK(p.lod == expect);
        CHECK(p.lod >= r.lod);   // footprint margin can only raise the level
    }
    // Exactly a power of two stays at that level; one more pixel moves up.
    mc2hzb::ProjectedProp q{}; q.keep = 0;
    q.pixels[0] = 8; q.pixels[1] = 8; q.pixels[2] = 12; q.pixels[3] = 12; q.nearest = 0.5f;   // extent 4 -> lod 2
    {
        Pyramid py(0.0f);
        q.lod = 2;
        CHECK_FALSE(hidden(q, py));
        Pyramid wall(0.9f);
        CHECK(hidden(q, wall));          // footprint (8..12)/4 = 2..3 inside the 16x16 level
    }
}

TEST_CASE("hzb ref: a one-texel hole inside the footprint keeps (MIN pyramid carries it to every mip)") {
    for (float px : {1.0f, 2.0f, 5.0f, 13.0f, 29.0f}) {
        Pyramid py(0.9f);
        const auto p = project(boxPixels(px));
        // hole in the centre of the footprint at base resolution
        py.set(33, 33, 0.0f);
        py.reduceFrom();
        CAPTURE(px);
        CHECK_FALSE(hidden(p, py));      // expected keep=1
    }
}

TEST_CASE("hzb ref: tiny objects (sub-pixel to a few pixels)") {
    Pyramid wall(0.9f);
    Pyramid sky(0.0f);
    for (float px : {0.0f, 0.2f, 1.0f, 3.0f}) {
        const auto p = project(boxPixels(px));
        CAPTURE(px);
        CHECK(p.keep == 0);
        CHECK(hidden(p, wall));          // expected keep=0
        // Sub-pixel and zero-area boxes are rejected outright (small-instance cull); one pixel and up still reach the pyramid.
        if (px < 1.0f) CHECK(hidden(p, sky));
        else CHECK_FALSE(hidden(p, sky));     // expected keep=1
    }
    // A hole far outside a tiny footprint must not rescue it: the per-object LOD reads only its own texels.
    Pyramid py(0.9f);
    py.set(5, 5, 0.0f);
    py.reduceFrom();
    CHECK(hidden(project(boxPixels(1.0f)), py));
}

TEST_CASE("hzb ref: regression MAX-vs-MIN reduction (historical over-cull)") {
    // Half the footprint is occluded (0.9), half is open sky (0.0). The farthest occluder is 0: the prop is visible.
    auto build = [](bool minReduce) {
        Pyramid py(0.0f, minReduce);
        for (int y = 0; y < H; ++y) for (int x = 0; x < 34; ++x) py.set(x, y, 0.9f);
        py.reduceFrom(minReduce);
        return py;
    };
    const auto p = project(boxPixels(13.0f));   // straddles the x = 34 boundary at lod 4
    CHECK(p.lod >= 1);
    CHECK_FALSE(hidden(p, build(true)));        // correct MIN: expected keep=1
    CHECK(hidden(p, build(false)));             // a MAX pyramid would cull it: the bug this test forbids
}

TEST_CASE("hzb ref: regression fixed-Lmin (per-object LOD must follow footprint size)") {
    // The lod must grow with the footprint; one fixed level for every object was the second historical bug.
    uint32_t last = 0; bool grew = false;
    for (float px : {1.0f, 5.0f, 13.0f, 29.0f}) {
        const auto p = project(boxPixels(px));
        CHECK(p.lod >= last);
        if (p.lod > last) grew = true;
        last = p.lod;
    }
    CHECK(grew);
    CHECK(last >= 4);
    // Large footprint with an occluder covering only its left half: must keep, whatever coarse level is available.
    Pyramid py(0.0f);
    for (int y = 0; y < H; ++y) for (int x = 0; x < 32; ++x) py.set(x, y, 0.9f);
    py.reduceFrom();
    CHECK_FALSE(hidden(project(boxPixels(29.0f)), py));
}

TEST_CASE("hzb ref: depth bias moves nearest, margin is 2e-5") {
    Pyramid py(0.5f);
    const auto at = project(boxPixels(6.0f), flatMatrix(), 0.0f);
    CHECK_FALSE(hidden(at, py));                                 // 0.5 < 0.5 - 2e-5 is false
    const auto far = project(boxPixels(6.0f), flatMatrix(), -0.1f);
    CHECK(far.nearest == doctest::Approx(0.4f));
    CHECK(hidden(far, py));                                      // 0.4 < 0.5 - 2e-5
}

TEST_CASE("hzb ref: sub-pixel and zero-area boxes are rejected without a pyramid fetch") {
    Pyramid empty(0.0f);   // nothing occludes: only the small-instance predicate can hide
    CHECK(hidden(project(boxPixels(0.0f)), empty));            // zero area (a point)
    CHECK(hidden(project(boxPixels(0.4f)), empty));            // under one pixel in both axes
    CHECK_FALSE(hidden(project(boxPixels(1.0f)), empty));      // exactly one pixel stays
    CHECK_FALSE(hidden(project(boxPixels(5.0f)), empty));
    CHECK(hidden(project(boxNdc(0.0f, 0.0f, 0.0f, 0.5f)), empty));      // edge-on: zero width, tall
    // Thin boxes on the 64 px test viewport (y = 32 + 32*ndcY): 0.32 px tall between the centres 31.5 and 32.5 rasterizes nothing,
    // 1.28 px tall over the centre 32.5 stays (5075a0770 pixel-centre rule; the old 1 px extent rule kept both).
    CHECK(hidden(project(boxNdc(0.0f, 0.5f, 0.0f, 0.01f)), empty));
    CHECK_FALSE(hidden(project(boxNdc(0.0f, 0.5f, 0.0f, 0.04f)), empty));
}

TEST_CASE("hzb ref: the 1/256 px grid rounds outward and tiny rejection never applies to unprojectable boxes") {
    CHECK(mc2hzb::tiny_box(10.0f, 10.0f, 10.0f, 10.0f));
    CHECK(mc2hzb::tiny_box(10.001f, 10.0f, 10.5f, 10.5f));
    CHECK(mc2hzb::tiny_box(10.0f, 10.0f, 11.0f, 10.5f));               // y [10, 10.5) holds no centre (half-open)
    CHECK_FALSE(mc2hzb::tiny_box(10.0f, 10.0f, 11.0f, 10.6f));         // y reaches past the centre 10.5: stays
    CHECK_FALSE(mc2hzb::tiny_box(10.0f, 10.0f, 10.9999f, 10.9999f));   // 10.9999 snaps up to 11: exactly one pixel wide, stays
    // Sub-pixel boxes: culled only when no pixel centre (k+0.5) lies inside; one that straddles a centre still draws and stays.
    CHECK_FALSE(mc2hzb::tiny_box(10.3f, 10.3f, 10.7f, 10.7f));          // 0.4 px box over the centre (10.5, 10.5)
    CHECK(mc2hzb::tiny_box(10.6f, 10.3f, 10.9f, 10.7f));                // x gap: 10.6..10.9 holds no centre
    CHECK(mc2hzb::tiny_box(10.3f, 10.6f, 10.7f, 10.9f));                // y gap
    CHECK_FALSE(mc2hzb::tiny_box(10.9f, 10.9f, 11.6f, 11.6f));          // 0.7 px box over (11.5, 11.5)
    Pyramid empty(0.0f);
    CHECK_FALSE(hidden(project(boxNdc(0.5f, 1.5f, 0.0f, 0.001f)), empty));   // crosses x = 1: kept whole, never tiny-culled
}

TEST_CASE("hzb ref: tiny test runs on a 1080p reference grid") {
    CHECK(mc2hzb::ref_scale(1080) == 1.0f);
    CHECK(mc2hzb::ref_scale(720) == 1.0f);
    CHECK(mc2hzb::ref_scale(2160) == 0.5f);
    const float s4k = mc2hzb::ref_scale(2160);
    CHECK_FALSE(mc2hzb::tiny_box(10.3f, 10.3f, 10.7f, 10.7f, 1.0f));          // identical to the old test at 1080
    CHECK(mc2hzb::tiny_box(21.2f, 21.2f, 22.7f, 22.7f, s4k));                 // 1.5 px at 4K = 0.75 ref px, no centre
    CHECK_FALSE(mc2hzb::tiny_box(21.0f, 21.0f, 23.5f, 23.5f, s4k));           // 2.5 px at 4K = 1.25 ref px, always a centre
}

TEST_CASE("hzb ref: side planes reject near-straddling boxes (props-cull-sideplanes)") {
    Pyramid py(0.9f);
    Mat m = flatMatrix();
    m.m[2 * 4 + 3] = 1.0f; m.m[3 * 4 + 3] = 0.0f;   // clip.w = rz: rz -1..1 crosses the near plane
    // Fully below the bottom plane (every corner has w + y < 0), one corner behind the eye: hidden, not kept.
    for (const auto& b : {boxNdc(0, .1f, -5, -4, -1, 1), boxNdc(0, .1f, 4, 5, -1, 1),
                          boxNdc(4, 5, 0, .1f, -1, 1), boxNdc(-5, -4, 0, .1f, -1, 1)}) {
        const auto p = project(b, m);
        CHECK(p.keep == 0);
        CHECK(p.tiny == 1);
    }
    // Straddling near and inside the frustum: kept. Straddling near and ONE side plane: kept.
    CHECK(project(boxNdc(0, .1f, 0, .1f, -1, 1), m).keep == 1);
    CHECK(project(boxNdc(0, .5f, -2, 0.5f, -1, 1), m).keep == 1);
    CHECK(project(boxNdc(.5f, 2, 0, .1f, -1, 1), m).keep == 1);
    CHECK(project(boxNdc(-2, -.5f, 0, .1f, -1, 1), m).keep == 1);
    // Just outside the plane by less than the 4 px pad stays kept; leaf sway widens the pad.
    CHECK(project(boxNdc(0, .1f, -1.05f, -1.01f, -1, 1), m).keep == 1);
}

TEST_CASE("hzb ref: lod floor of 1 (level 0 is not built) and it only keeps more") {
    // Boxes clamped at each screen edge (extent <= 1 would have picked lod 0), and tiny boxes inside.
    for (const auto& b : {boxNdc(0.99f, 1.0f, 0.0f, 0.01f), boxNdc(-1.0f, -0.99f, 0.0f, 0.01f),
                          boxNdc(0.0f, 0.01f, 0.99f, 1.0f), boxNdc(0.0f, 0.01f, -1.0f, -0.99f), boxPixels(1.0f)}) {
        const auto p = project(b);
        if (p.keep == 0 && p.tiny == 0) CHECK(p.lod >= 1);
    }
    // Random pyramid and boxes: whatever the new rule hides, the old rule (lod from 0) also hides.
    uint32_t seed = 12345u;
    const auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1u << 24); };
    Pyramid py(0.0f);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) py.set(x, y, rnd() < 0.3f ? 0.0f : 0.9f);
    py.reduceFrom();
    int hiddenNew = 0;
    for (int i = 0; i < 2000; ++i) {
        const float x0 = rnd() * 2 - 1, y0 = rnd() * 2 - 1;
        auto p = project(boxNdc(x0, (std::min)(1.0f, x0 + rnd() * 0.2f), y0, (std::min)(1.0f, y0 + rnd() * 0.2f)));
        if (p.keep != 0) continue;
        CHECK(p.lod >= 1);
        auto old = p;
        old.lod = 0;
        const float extent = (std::max)(p.pixels[2] - p.pixels[0], p.pixels[3] - p.pixels[1]);
        while (old.lod + 1 < uint32_t(LEVELS) && float(1u << old.lod) < extent) ++old.lod;
        if (hidden(p, py)) { ++hiddenNew; CHECK(hidden(old, py)); }
    }
    CHECK(hiddenNew > 0);
}
