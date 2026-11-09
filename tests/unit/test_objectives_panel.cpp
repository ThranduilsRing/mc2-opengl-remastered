#include "doctest.h"
#include "objectives_panel.h"
#include <array>
#include <vector>

namespace {
#include "objectives_canvas.inc"

// Exercise the production batch transform without linking the renderer.
struct Vertex { float x, y, u = 0.f, v = 0.f; };
struct Call {
    std::vector<Vertex> vertices;
    bool scaleExempt = false, canvasExempt = true;
};
struct gosRenderer {
    std::vector<Call> hudBatch_;
    void endHudPanel(size_t firstDraw, float scale);
};
#include "objectives_batch.inc"
}

TEST_CASE("objectives panel: stock size through 1080p and slower growth above") {
    using mc2::objectives_panel::scaleForCanvasHeight;
    for (int height : {432, 576, 600, 768, 900, 1080})
        CHECK(scaleForCanvasHeight(height) == 1.f);
    CHECK(scaleForCanvasHeight(1440) == doctest::Approx(0.8660254f));
    CHECK(scaleForCanvasHeight(2160) == doctest::Approx(0.7071068f));
    CHECK(scaleForCanvasHeight(0) == 1.f);
}

TEST_CASE("objectives panel: all aspect classes use the usable canvas height") {
    struct Case { int width, height; float expected; };
    for (const auto& test : {Case{1024,768,1.f}, Case{1920,1080,1.f},
             Case{2560,1440,0.8660254f}, Case{2560,1600,0.8660254f},
             Case{3440,1440,0.8660254f}, Case{5120,1440,0.8660254f},
             Case{3840,2160,0.7071068f}, Case{3840,2400,0.7071068f},
             Case{2880,2160,0.8164966f}}) {
        int x, y, width, height;
        computeUiCanvas16x9(test.width, test.height, &x, &y, &width, &height);
        CHECK(mc2::objectives_panel::scaleForCanvasHeight(height)
            == doctest::Approx(test.expected));
    }
}

TEST_CASE("objectives panel: complete batch scales text and tiles once without leaking") {
    gosRenderer renderer;
    renderer.hudBatch_ = {
        {{{10,20}}},                       // earlier unrelated HUD draw
        {{{-4,80,.2f,.3f},{100,500,.4f,.5f}}}, // panel: both sides of 60% band
        {{{20,90},{30,100},{40,110}}}       // panel: bitmap glyph geometry
    };
    renderer.endHudPanel(1, .5f);
    CHECK(renderer.hudBatch_[0].vertices[0].x == 10.f);
    CHECK_FALSE(renderer.hudBatch_[0].scaleExempt);
    CHECK(renderer.hudBatch_[0].canvasExempt);
    CHECK(renderer.hudBatch_[1].vertices[0].x == -2.f);
    CHECK(renderer.hudBatch_[1].vertices[0].y == 40.f);
    CHECK(renderer.hudBatch_[1].vertices[1].y == 250.f);
    CHECK(renderer.hudBatch_[1].vertices[0].u == .2f);
    CHECK(renderer.hudBatch_[1].vertices[0].v == .3f);
    CHECK(renderer.hudBatch_[2].vertices[2].x == 20.f);
    for (size_t i = 1; i < renderer.hudBatch_.size(); ++i) {
        CHECK(renderer.hudBatch_[i].scaleExempt);
        CHECK_FALSE(renderer.hudBatch_[i].canvasExempt);
    }
    renderer.hudBatch_.push_back({{{60,70}}});
    CHECK(renderer.hudBatch_.back().vertices[0].x == 60.f);
    CHECK_FALSE(renderer.hudBatch_.back().scaleExempt);
}

TEST_CASE("objectives panel: black surround is clear and colored art keeps alpha") {
    using Pixel = std::array<uint8_t,4>;
    Pixel black{0,0,0,255}, translucent{0,0,0,127};
    Pixel red{255,0,0,255}, green{0,255,0,128}, white{255,255,255,255};
    mc2::objectives_panel::clearMarkerBackground(black.data());
    mc2::objectives_panel::clearMarkerBackground(translucent.data());
    CHECK(black[3] == 0);
    CHECK(translucent[3] == 0);
    for (const Pixel before : {red,green,white,Pixel{1,0,0,200},Pixel{255,0,0,0}}) {
        auto after = before;
        mc2::objectives_panel::clearMarkerBackground(after.data());
        CHECK(after == before);
    }
}
