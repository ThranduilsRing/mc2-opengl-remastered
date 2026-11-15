// UI-CHROME-SPLIT-1: manifest round-trip, load-time sheet re-pack, nine-slice geometry, and the checked-in
// data/art/ui manifest (every widget file present, sized to its rect).
#include "doctest.h"

#include "ui_chrome_sheets.h"
#include "texture_decode.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef MC2_REPO_ROOT
#error "MC2_REPO_ROOT must be defined by tests/unit/CMakeLists.txt"
#endif

using namespace mc2::ui_chrome;

TEST_CASE("ui_chrome inset excludes whole images and content sprites") {
    const auto inset = [](std::string_view path, bool pixel = false, bool legacy = false, bool nine = false,
                          float u = .25f, float v = .25f, float r = .75f, float b = .75f) {
        return insetChromeCrop(path, pixel, legacy, nine, u, v, r, b);
    };
    CHECK_FALSE(inset("data/art/gui/mcl_mm_2.png"));
    CHECK(inset("data/art/gui/mcl_splashscreen_frame_2.png"));
    CHECK(inset("data/art/gui/MCUI_Universal1.tga"));
    CHECK(inset("data/art/ui/panel.png", false, false, true));
    CHECK_FALSE(inset("data/art/gui/mcl_mm_2.png", false, false, false, 0, 0, 1, 1));
    CHECK_FALSE(inset("data/art/gui/mcl_mm_2.png", true));
    CHECK_FALSE(inset("data/art/gui/mcl_mm_2.png", false, true));
    CHECK_FALSE(inset("data/art/gui/mcui_jumpjets.png"));
    CHECK_FALSE(inset("data/art/gui/mcui_high7.png"));
    CHECK_FALSE(inset("data/art/portraits/pilot.png"));
}

namespace {
Sheet makeSheet(bool rgba) {
    Sheet s;
    s.key = "demo";
    s.w = 8;
    s.h = 6;
    s.rgba = rgba;
    Widget a; a.name = "a"; a.x = 0; a.y = 0; a.w = 4; a.h = 3;
    Widget b; b.name = "b"; b.x = 4; b.y = 3; b.w = 4; b.h = 3; b.hasNine = true; b.nine = {1, 1, 1, 1};
    s.widgets = {a, b};
    return s;
}
Pixels solid(int w, int h, int ch, uint8_t v) {
    Pixels p; p.w = w; p.h = h; p.channels = ch; p.data.assign(size_t(w) * h * ch, v);
    return p;
}
}  // namespace

TEST_CASE("ui_chrome shipped credits contain no machine paths") {
    // Trigger path: CREDITS.txt (displayed verbatim by PlayerFrontEnd).
    std::ifstream input(std::filesystem::path(MC2_REPO_ROOT) / "CREDITS.txt");
    REQUIRE(input.good());
    std::string line;
    while (std::getline(input, line)) {
        INFO(line);
        CHECK(line.find("mc2-opengl") == std::string::npos);
        // Owner 11-15: credits are MC2 base game + MC2 modding only; libraries live in THIRD_PARTY_NOTICES.txt.
        CHECK(line.find("3rdparty/") == std::string::npos);
        for (size_t i = 1; i + 1 < line.size(); ++i)
            CHECK_FALSE(((i == 1 || line[i-2] == ' ' || line[i-2] == '(') &&
                                          line[i] == ':' && (line[i+1] == '/' || line[i+1] == '\\') &&
                                          ((line[i-1] >= 'A' && line[i-1] <= 'Z') ||
                                           (line[i-1] >= 'a' && line[i-1] <= 'z'))));
    }
}

TEST_CASE("ui_chrome credits bay follows the menu frame at 1080p and 4K") {
    for (float scale : {1.0f, 2.0f}) {
        const auto r = menuCreditsBay(1920 * scale, 1080 * scale);
        CHECK(r[0] == doctest::Approx(1449.6f * scale));
        CHECK(r[1] == doctest::Approx(525.6f * scale));
        CHECK(r[2] == doctest::Approx(403.2f * scale));
        CHECK(r[3] == doctest::Approx(446.4f * scale));
        CHECK(r[0] + r[2] < 1920 * scale);
        CHECK(r[1] + r[3] < 1080 * scale);
    }
}

TEST_CASE("ui_chrome menu openings preserve cable and hazard paint") {
    Pixels p = solid(256, 256, 3, 0);
    const size_t cable = (140 * 256 + 170) * 3;
    p.data[cable] = 64;
    REQUIRE(clearMenuMatte("data/art/gui/mcl_mm_1.png", p));
    CHECK(p.data[(140 * 256 + 160) * 4 + 3] == 0);
    CHECK(p.data[(140 * 256 + 170) * 4 + 3] == 255);
    CHECK(p.data[(210 * 256 + 80) * 4 + 3] == 255);
    CHECK(p.data[(40 * 256 + 40) * 4 + 3] == 255);
    CHECK(p.data[(40 * 256 + 140) * 4 + 3] == 255);
    p = solid(256, 256, 3, 0);
    REQUIRE(clearMenuMatte("mcl_mm_2.png", p));
    CHECK(p.data[(40 * 256 + 40) * 4 + 3] == 255);
    CHECK(p.data[(140 * 256 + 40) * 4 + 3] == 255);
    p = solid(256, 256, 3, 0);
    REQUIRE(clearMenuMatte("mcl_mm_3.png", p));
    CHECK(p.data[(100 * 256 + 20) * 4 + 3] == 0);
    CHECK(p.data[(50 * 256 + 140) * 4 + 3] == 255);
    CHECK_FALSE(clearMenuMatte("other.png", p));
}

TEST_CASE("ui_chrome manifest round trip") {
    Manifest m;
    m.sheets["demo"] = makeSheet(false);
    const std::string json = manifestToJson(m);
    Manifest back;
    std::string err;
    REQUIRE(parseManifest(json, back, &err));
    const auto found = back.find("demo");
    REQUIRE(found.has_value());
    const Sheet* s = &found->get();
    CHECK(s->w == 8);
    CHECK(s->h == 6);
    CHECK_FALSE(s->rgba);
    REQUIRE(s->widgets.size() == 2);
    CHECK(s->widgets[1].name == "b");
    CHECK(s->widgets[1].x == 4);
    CHECK(s->widgets[1].hasNine);
    CHECK(s->widgets[1].nine[2] == 1);
    CHECK_FALSE(s->widgets[0].hasNine);
}

TEST_CASE("ui_chrome manifest rejects malformed input") {
    Manifest m;
    CHECK_FALSE(parseManifest("not json", m));
    CHECK_FALSE(parseManifest(R"({"version":2,"sheets":{}})", m));
    CHECK_FALSE(parseManifest(R"({"version":1,"sheets":{"s":{"size":[4,4],"widgets":{"w":{"rect":[2,2,4,4]}}}}})", m));
    CHECK(m.sheets.empty());
}

TEST_CASE("ui_chrome composeSheet pastes widgets at their rects, uncovered texels stay keyed") {
    const Sheet s = makeSheet(false);
    auto load = [](const Sheet&, const Widget& w, Pixels& px) { px = solid(w.w, w.h, 3, w.name == "a" ? 10 : 20); return true; };
    Pixels out;
    REQUIRE(composeSheet(s, load, out));
    REQUIRE(out.w == 8);
    REQUIRE(out.channels == 3);
    auto at = [&](int x, int y, int c) { return out.data[(size_t(y) * 8 + x) * 3 + c]; };
    CHECK(at(0, 0, 0) == 10);
    CHECK(at(3, 2, 1) == 10);
    CHECK(at(4, 3, 2) == 20);
    CHECK(at(7, 5, 0) == 20);
    CHECK(at(7, 0, 0) == 255);   // uncovered: magenta key
    CHECK(at(7, 0, 1) == 0);
    CHECK(at(7, 0, 2) == 255);
}

TEST_CASE("ui_chrome composeSheet pastes the padded ring: authored neighbours, clamped at the sheet edge") {
    Sheet s = makeSheet(false);
    s.pad = 1;
    // widget "a" (0,0,4,3) -> file covers x [0,5) y [0,4); widget "b" (4,3,4,3) -> x [3,8) y [2,6)
    auto load = [&](const Sheet& sh, const Widget& w, Pixels& px) {
        const PaddedRect pr = paddedRect(sh, w);
        px = solid(pr.x1 - pr.x0, pr.y1 - pr.y0, 3, w.name == "a" ? 10 : 20);
        return true;
    };
    Pixels out;
    REQUIRE(composeSheet(s, load, out));
    auto at = [&](int x, int y) { return out.data[(size_t(y) * 8 + x) * 3]; };
    CHECK(at(4, 0) == 10);    // right ring of "a"
    CHECK(at(0, 3) == 10);    // bottom ring of "a"
    CHECK(at(3, 2) == 20);    // top-left ring of "b" (written after "a" in widget order)
    CHECK(at(7, 0) == 255);   // beyond every ring: key
    // a file sized to the bare rect no longer fits a padded sheet
    CHECK_FALSE(composeSheet(s, [](const Sheet&, const Widget& w, Pixels& px) { px = solid(w.w, w.h, 3, 1); return true; }, out));
    // pad round-trips through the manifest
    Manifest m;
    m.sheets.emplace(s.key, s);
    Manifest back;
    REQUIRE(parseManifest(manifestToJson(m), back));
    CHECK(back.sheets.at("demo").pad == 1);
}

TEST_CASE("ui_chrome composeSheet fails whole when a widget is missing or the wrong size") {
    const Sheet s = makeSheet(true);
    Pixels out;
    CHECK_FALSE(composeSheet(s, [](const Sheet&, const Widget&, Pixels&) { return false; }, out));
    CHECK(out.data.empty());
    CHECK_FALSE(composeSheet(s, [](const Sheet&, const Widget&, Pixels& px) { px = solid(2, 2, 4, 1); return true; }, out));
}

TEST_CASE("ui_chrome stored PNG decodes to the same pixels") {
    Pixels p = solid(70, 40, 3, 0);   // > 65535 bytes of raw data: several stored blocks
    for (size_t i = 0; i < p.data.size(); ++i) p.data[i] = uint8_t((i * 7 + 3) & 0xff);
    const auto png = encodePngStored(p);
    REQUIRE(!png.empty());
    auto dec = mc2tex::decodeMemory(png.data(), png.size(), "test.png", gos_Texture_Alpha, 0);
    REQUIRE(dec.has_value());
    CHECK(dec->width == 70);
    CHECK(dec->height == 40);
    // the platform decoder may widen RGB to RGBA (alpha 255); the colour channels must be exact
    const int dch = dec->layout == mc2tex::PixelLayout::RGB8 ? 3 : 4;
    bool same = dec->pixels.size() == size_t(70) * 40 * dch;
    for (size_t i = 0; same && i < size_t(70) * 40; ++i)
        for (int c = 0; c < 3; ++c) same = same && dec->pixels[i * dch + c] == p.data[i * 3 + c];
    CHECK(same);
    Pixels q = solid(5, 4, 4, 0);
    for (size_t i = 0; i < q.data.size(); ++i) q.data[i] = uint8_t(i * 13 + 1);
    const auto png4 = encodePngStored(q);
    auto dec4 = mc2tex::decodeMemory(png4.data(), png4.size(), "test4.png", gos_Texture_Alpha, 0);
    REQUIRE(dec4.has_value());
    CHECK(dec4->pixels == q.data);
}

TEST_CASE("ui_chrome sheetKeyFromPath") {
    CHECK(sheetKeyFromPath("data/art/gui/mcui_universal1.tga") == "mcui_universal1");
    CHECK(sheetKeyFromPath("DATA\\ART\\GUI\\MCL_MB_4.PNG") == "mcl_mb_4");
    CHECK(sheetKeyFromPath("data/art/mechicons/x.png").empty());
    CHECK(sheetKeyFromPath("data/art/gui/sub/x.tga").empty());
}

TEST_CASE("ui_chrome nine-slice geometry") {
    std::array<Quad, 9> q;
    REQUIRE(nineSliceQuads(32, 24, {4, 3, 5, 6}, 10.f, 20.f, 100.f, 50.f, q));
    // corners keep their texel size
    CHECK(q[0].dx1 - q[0].dx0 == 4.f);
    CHECK(q[0].dy1 - q[0].dy0 == 3.f);
    CHECK(q[2].dx1 - q[2].dx0 == 5.f);
    CHECK(q[8].dy1 - q[8].dy0 == 6.f);
    // outer bounds are the destination rect
    CHECK(q[0].dx0 == 10.f);
    CHECK(q[0].dy0 == 20.f);
    CHECK(q[8].dx1 == 110.f);
    CHECK(q[8].dy1 == 70.f);
    // centre stretches both ways and samples the middle of the source
    CHECK(q[4].dx0 == 14.f);
    CHECK(q[4].dx1 == 105.f);
    CHECK(q[4].sx0 == 4.f);
    CHECK(q[4].sx1 == 27.f);
    CHECK(q[4].sy0 == 3.f);
    CHECK(q[4].sy1 == 18.f);
    // top edge stretches in x only; left edge in y only
    CHECK(q[1].dy1 - q[1].dy0 == 3.f);
    CHECK(q[3].dx1 - q[3].dx0 == 4.f);
    // tiling: neighbouring quads share edges, source covers the whole image
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col) {
            CHECK(q[row * 3 + col].dx1 == q[row * 3 + col + 1].dx0);
            CHECK(q[row * 3 + col].sx1 == q[row * 3 + col + 1].sx0);
        }
    CHECK(q[8].sx1 == 32.f);
    CHECK(q[8].sy1 == 24.f);
    // page scale: corners grow with the page, the source rects do not
    REQUIRE(nineSliceQuads(32, 24, {4, 3, 5, 6}, 0.f, 0.f, 200.f, 100.f, q, 2.f, 3.f));
    CHECK(q[0].dx1 == 8.f);
    CHECK(q[0].dy1 == 9.f);
    CHECK(q[8].dx0 == 190.f);
    CHECK(q[8].dy0 == 82.f);
    CHECK(q[0].sx1 == 4.f);
    // degenerate inputs
    CHECK_FALSE(nineSliceQuads(32, 24, {20, 3, 12, 6}, 0, 0, 100, 50, q));   // no centre
    CHECK_FALSE(nineSliceQuads(32, 24, {4, 3, 5, 6}, 0, 0, 8, 50, q));       // destination narrower than corners
}

namespace {
bool pngSize(const std::filesystem::path& p, int& w, int& h) {
    std::ifstream f(p, std::ios::binary);
    unsigned char b[24] = {};
    if (!f.read(reinterpret_cast<char*>(b), 24)) return false;
    if (b[0] != 0x89 || b[1] != 'P') return false;
    w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
    return true;
}
}  // namespace

TEST_CASE("ui_chrome checked-in data/art/ui manifest: every widget file exists at its rect size") {
    const std::filesystem::path root = std::filesystem::path(MC2_REPO_ROOT) / "data" / "art" / "ui";
    std::ifstream in(root / "manifest.json", std::ios::binary);
    REQUIRE(in.good());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Manifest m;
    std::string err;
    REQUIRE_MESSAGE(parseManifest(json, m, &err), err);
    CHECK(m.sheets.size() > 50);
    size_t widgets = 0;
    for (const auto& [key, s] : m.sheets)
        for (const auto& w : s.widgets) {
            ++widgets;
            int pw = 0, ph = 0;
            const auto f = root / key / (w.name + ".png");
            INFO(f.string());
            REQUIRE(pngSize(f, pw, ph));
            const PaddedRect pr = paddedRect(s, w);
            CHECK(pw == pr.x1 - pr.x0);
            CHECK(ph == pr.y1 - pr.y0);
        }
    CHECK(widgets > 1000);
}
