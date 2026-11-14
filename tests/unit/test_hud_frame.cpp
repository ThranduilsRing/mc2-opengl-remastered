// Trigger paths: code/controlgui{,_init,_render}.{h,cpp}.
#include "doctest.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace {
using DWORD = uint32_t;
struct Vertex { float x = 0, y = 0, u = 0, v = 0; DWORD argb = 0xffffffff; };
struct StaticInfo { unsigned long textureHandle = 0; Vertex location[4]; };
struct ForceGroupBar { inline static StaticInfo* s_coverIcon = nullptr; };
namespace RenderCore {
struct KtxImage { int width = 2, height = 2, vkFormat = 37; std::vector<uint8_t> pixels; };
constexpr uint32_t kKtxMaxEdge = 16384;
}
namespace mc2::textures {
constexpr DWORD kTextureAssetLimit = 0x40000000;
std::vector<std::shared_ptr<RenderCore::KtxImage>> images;
std::vector<DWORD> uploaded;
DWORD atlasEdge = 0;
auto image(DWORD handle) { return images.at(handle); }
DWORD fromRawPixels(DWORD* pixels, int, int, DWORD edge) {
    atlasEdge = edge;
    uploaded.assign(pixels, pixels + static_cast<size_t>(edge) * edge);
    return 10;
}
}
constexpr int gos_Texture_Alpha = 0;
namespace mc2_diag { bool tagRequested(const char*) { return false; } }
float scale = 1.f;
float gos_GetHudScale() { return scale; }
struct { int screenWidth = 1920, screenHeight = 1080; } Environment;
struct ControlGui {
    std::vector<StaticInfo> staticInfos = std::vector<StaticInfo>(23);
    int staticCount = 23;
    std::unique_ptr<StaticInfo> frameAtlas;
    std::vector<Vertex> frameVertices;
    long frameSplit = 0;
    float frameScale = -1;
    int frameWidth = 0, frameHeight = 0;
    void bakeFrame();
};
#define ZoneScopedN(...)
#define MC2_LOG(...)
#include "hud_frame_bake.inc"
#undef ZoneScopedN
#undef MC2_LOG
}

TEST_CASE("HUD frame: atlas preserves pixels, rotated UVs, order and dynamic exclusion") {
    mc2::textures::images.resize(3);
    for (int id : {1, 2}) {
        auto image = std::make_shared<RenderCore::KtxImage>();
        image->pixels = {1,2,3,255, 4,5,6,128, 7,8,9,0, 10,11,12,255};
        mc2::textures::images[id] = image;
    }
    ControlGui gui;
    ForceGroupBar::s_coverIcon = &gui.staticInfos[20];
    for (auto& info : gui.staticInfos) {
        info.textureHandle = 1;
        info.location[0] = {0,900,0.05f,0.05f};
        info.location[1] = {0,902,0.05f,1.05f};
        info.location[2] = {2,902,1.05f,1.05f};
        info.location[3] = {2,900,1.05f,0.05f};
    }
    gui.staticInfos[18].location[2] = gui.staticInfos[18].location[0];
    gui.staticInfos[20].textureHandle = 999; // unavailable dynamic image must not be decoded
    gui.staticInfos[21].textureHandle = gui.staticInfos[22].textureHandle = 2;
    std::swap(gui.staticInfos[21].location[0].u, gui.staticInfos[21].location[2].u);
    for (int height : {1080,2160}) {
        Environment.screenHeight = height;
        Environment.screenWidth = height * 16 / 9;
        for (auto& info : gui.staticInfos) {
            info.location[0].y = info.location[3].y = static_cast<float>(height - 100);
            info.location[1].y = info.location[2].y = static_cast<float>(height - 98);
        }
        gui.staticInfos[18].location[2] = gui.staticInfos[18].location[0];
        for (float hudScale : {1.f,.85f,.4f}) {
            scale = hudScale;
            gui.bakeFrame();
            REQUIRE(gui.frameAtlas);
            CHECK(gui.frameScale == hudScale);
            CHECK(gui.frameWidth == Environment.screenWidth);
            CHECK(gui.frameHeight == height);
            CHECK(gui.frameSplit == 19 * 4);
            CHECK(gui.frameVertices.size() == 21 * 4);
            CHECK(mc2::textures::atlasEdge == 4);
            CHECK(mc2::textures::uploaded[0] == 0xff010203);
            CHECK(mc2::textures::uploaded[1] == 0x80040506);
            CHECK(mc2::textures::uploaded[2] == 0xff010203);
            size_t output = 0;
            for (int i = 0; i < gui.staticCount; ++i) {
                if (i == 18 || i == 20) continue;
                const float offset = i >= 21 ? 2.f : 0.f;
                for (const auto& original : gui.staticInfos[i].location) {
                    const auto& baked = gui.frameVertices[output++];
                    CHECK(baked.x == original.x);
                    CHECK(baked.y == original.y);
                    CHECK(baked.argb == original.argb);
                    CHECK(baked.u * 4 == doctest::Approx(original.u * 2 + offset));
                    CHECK(baked.v * 4 == doctest::Approx(original.v * 2));
                }
            }
        }
    }
    // The dynamic cover can occupy a different layout slot.
    std::swap(gui.staticInfos[7], gui.staticInfos[20]);
    ForceGroupBar::s_coverIcon = &gui.staticInfos[7];
    gui.bakeFrame();
    REQUIRE(gui.frameAtlas);
    CHECK(gui.frameSplit == 7 * 4);
    CHECK(gui.frameVertices.size() == 21 * 4);
    gui.staticInfos[0].location[0].y = gui.staticInfos[0].location[1].y = 0;
    gui.staticInfos[0].location[2].y = gui.staticInfos[0].location[3].y = 2;
    gui.bakeFrame();
    CHECK_FALSE(gui.frameAtlas); // mixed scaling-band layouts use original draws
    gui.staticInfos[0].location[0].y = gui.staticInfos[0].location[3].y = 2060;
    gui.staticInfos[0].location[1].y = gui.staticInfos[0].location[2].y = 2062;
    mc2::textures::images[1]->vkFormat = 145;
    gui.bakeFrame();
    CHECK_FALSE(gui.frameAtlas);
}
