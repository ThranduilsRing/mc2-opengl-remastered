// tests/unit/test_screen_pick.cpp
// Unit tests for the screenPickCompute pure coord transform.
// Does NOT test any runtime wrapper (those call gos_GetViewport / require a GL context).

#include "doctest.h"
#include "../../RenderWorld/ScreenPick.h"
#include "cursor_world_pick.h"
#include "pick_query.h"
#include <algorithm>
#include <array>
#include <vector>
#include <utility>

TEST_CASE("pick priority/depth/handle selection equals sorted result on ties") {
    struct Hit { mc2::pick::Rank rank; int identity; };
    // Every priority, tied depths, tied handles, and exact ties (first input wins).
    std::vector<Hit> hits;
    for (int priority = 0; priority < 10; ++priority)
        for (int depth = 0; depth < 3; ++depth)
            for (int handle = 1; handle < 4; ++handle)
                for (int duplicate = 0; duplicate < 2; ++duplicate)
                    hits.push_back({{priority, float(depth), handle}, int(hits.size())});
    unsigned seed = 891u;
    for (int order = 0; order < 100; ++order) {
        for (size_t i = hits.size(); i > 1; --i) {
            seed = seed * 1664525u + 1013904223u;
            std::swap(hits[i - 1], hits[seed % i]);
        }
        // Include misses, single hits, all-tied priority groups, and the full corpus.
        for (size_t count : {size_t(0), size_t(1), size_t(9), size_t(31), hits.size()}) {
            auto sorted = std::vector<Hit>(hits.begin(), hits.begin() + count);
            std::stable_sort(sorted.begin(), sorted.end(), [](const Hit& a, const Hit& b) {
                if (a.rank.depth != b.rank.depth) return a.rank.depth < b.rank.depth;
                return a.rank.handle < b.rank.handle;
            });
            int expected = -1, priority = 10;
            for (const auto& hit : sorted)
                if (hit.rank.priority < priority) {
                    expected = hit.identity;
                    priority = hit.rank.priority;
                }
            mc2::pick::Rank best;
            int got = -1;
            for (size_t i = 0; i < count; ++i)
                if (mc2::pick::precedes(hits[i].rank, best)) {
                    best = hits[i].rank;
                    got = hits[i].identity;
                }
            CHECK(got == expected);
        }
    }
}

TEST_CASE("GUI-covered pointer skips pick but clears hover and reentry picks fresh") {
    int old = 1, fresh = 2, newest = 3;
    int* target = &old;
    int picks = 0, clears = 0;
    bool bars = true, targeted = true;
    auto clear = [&] { ++clears; bars = false; targeted = false; };
    auto pick = [&] {
        CHECK_FALSE(bars);
        CHECK_FALSE(targeted);
        ++picks;
        return picks == 1 ? &fresh : &newest;
    };
    mc2::pick::refreshHover(true, target, clear, pick);
    CHECK(target == nullptr);
    CHECK(picks == 0);
    CHECK(clears == 1);
    CHECK_FALSE(bars);
    CHECK_FALSE(targeted);
    mc2::pick::refreshHover(false, target, clear, pick);
    CHECK(target == &fresh);
    mc2::pick::refreshHover(false, target, clear, pick);
    CHECK(target == &newest);
    CHECK(picks == 2);
    CHECK(clears == 3);
}

TEST_CASE("screen pick scene scaling keeps center and edge clicks aligned") {
    for (const auto scene : {std::pair{3840,2160}, std::pair{2560,1440}, std::pair{1920,1080}}) {
        int x=1920, y=1080;
        REQUIRE(RenderWorld::screenPickScenePixel(x,y,3840,2160,scene.first,scene.second));
        CHECK(x == scene.first/2); CHECK(y == scene.second/2);
        x=3839; y=2159;
        REQUIRE(RenderWorld::screenPickScenePixel(x,y,3840,2160,scene.first,scene.second));
        CHECK(x == scene.first-1); CHECK(y == scene.second-1);
        x=0; y=0;
        REQUIRE(RenderWorld::screenPickScenePixel(x,y,3840,2160,scene.first,scene.second));
        CHECK(x == 0); CHECK(y == 0);
    }
}

TEST_CASE("screen pick scene scaling never clamps outside clicks onto an object") {
    for (const auto point : {std::pair{-1,0}, std::pair{0,-1}, std::pair{3840,0}, std::pair{0,2160}}) {
        int x=point.first, y=point.second;
        CHECK_FALSE(RenderWorld::screenPickScenePixel(x,y,3840,2160,1920,1080));
        CHECK(x == point.first); CHECK(y == point.second);
    }
    int x=0, y=0;
    CHECK_FALSE(RenderWorld::screenPickScenePixel(x,y,0,2160,1920,1080));
    CHECK_FALSE(RenderWorld::screenPickScenePixel(x,y,3840,2160,0,1080));
}

TEST_CASE("cursor pick miss keeps previous position without a fallback projection") {
    struct Camera {
        bool hit = false;
        int calls = 0;
        bool pickTerrainRay(long, long, int& out) { ++calls; out = 99; return hit; }
    } camera;
    int out = 7;
    CHECK_FALSE(pickCursorWorld(&camera, 0, 0, out, 42, true));
    CHECK(out == 42);
    CHECK_FALSE(pickCursorWorld(&camera, 0, 0, out, 0, false));
    CHECK(out == 42);
    camera.hit = true;
    CHECK(pickCursorWorld(&camera, 0, 0, out, 42, true));
    CHECK(out == 99);
    CHECK(camera.calls == 3);
}

TEST_CASE("screenPickCompute full-screen identity: mouse pixel == FBO pixel") {
    RenderWorld::ScreenPickContext ctx{};
    ctx.mouseX = 100; ctx.mouseY = 50;
    ctx.vMulX  = 800.f; ctx.vMulY  = 600.f;
    ctx.vAddX  = 0.f;   ctx.vAddY  = 0.f;
    ctx.drawableWidth = 800; ctx.drawableHeight = 600;
    RenderWorld::screenPickCompute(&ctx);
    CHECK(ctx.fboX == 100);
    CHECK(ctx.fboY == 50);
    CHECK(ctx.glX  == 100);
    CHECK(ctx.glY  == 549);  // 600 - 1 - 50
}

TEST_CASE("screenPickCompute GL y-flip: top of screen maps to glY == drawableHeight-1") {
    RenderWorld::ScreenPickContext ctx{};
    ctx.mouseX = 0; ctx.mouseY = 0;
    ctx.vMulX  = 800.f; ctx.vMulY  = 600.f;
    ctx.vAddX  = 0.f;   ctx.vAddY  = 0.f;
    ctx.drawableWidth = 800; ctx.drawableHeight = 600;
    RenderWorld::screenPickCompute(&ctx);
    CHECK(ctx.fboY == 0);
    CHECK(ctx.glY  == 599);
}

TEST_CASE("screenPickCompute bottom of screen maps to glY == 0") {
    RenderWorld::ScreenPickContext ctx{};
    ctx.mouseX = 0; ctx.mouseY = 599;
    ctx.vMulX  = 800.f; ctx.vMulY  = 600.f;
    ctx.vAddX  = 0.f;   ctx.vAddY  = 0.f;
    ctx.drawableWidth = 800; ctx.drawableHeight = 600;
    RenderWorld::screenPickCompute(&ctx);
    CHECK(ctx.fboY == 599);
    CHECK(ctx.glY  == 0);
}

TEST_CASE("screenPickCompute sub-viewport with offset") {
    // vMulX=640, drawableWidth=800 -> scaleX=800/640=1.25
    // vAddX=80 -> fboX = 80 + 100*1.25 = 205
    // vAddY=60 -> fboY = 60 + 0*1.25   = 60
    RenderWorld::ScreenPickContext ctx{};
    ctx.mouseX = 100; ctx.mouseY = 0;
    ctx.vMulX  = 640.f; ctx.vMulY  = 480.f;
    ctx.vAddX  = 80.f;  ctx.vAddY  = 60.f;
    ctx.drawableWidth = 800; ctx.drawableHeight = 600;
    RenderWorld::screenPickCompute(&ctx);
    CHECK(ctx.fboX == 205);
    CHECK(ctx.fboY == 60);
    CHECK(ctx.glX  == 205);
    CHECK(ctx.glY  == 539);  // 600 - 1 - 60
}

TEST_CASE("screenPickCompute degenerate viewport (vMulX==0): no-op") {
    RenderWorld::ScreenPickContext ctx{};
    ctx.mouseX = 100; ctx.mouseY = 50;
    ctx.vMulX  = 0.f;   ctx.vMulY = 600.f;
    ctx.drawableWidth = 800; ctx.drawableHeight = 600;
    ctx.fboX = 999; ctx.glX = 999;  // sentinel
    RenderWorld::screenPickCompute(&ctx);
    CHECK(ctx.fboX == 999);  // unchanged
    CHECK(ctx.glX  == 999);  // unchanged
}
