// Loading-screen detail lines: the text is a pure function of its values.
#include "doctest.h"
#include "../../code/loading_detail.h"
using mc2::loaddetail::format;
using mc2::loaddetail::Info;

TEST_CASE("loading detail: stage, memory and settings lines") {
    Info i;
    i.stage = "Terrain textures";
    i.hasProgress = true;
    i.progress = 48.5f;
    i.ramUsed = 10522669824ull;
    i.ramTotal = 34359738368ull;
    i.vramUsed = 3ull << 29;
    i.vramBudget = 2ull << 30;
    i.width = 3840;
    i.height = 2160;
    i.preset = "High";
    CHECK(format(i) ==
          "Terrain textures 48%\n"
          "RAM   9.8 / 32.0 GB\n"
          "VRAM  1.5 / 2.0 GB\n"
          "3840x2160  Preset High\nQuality High  Shadows High  Textures Ultra  AA SMAA 1x");
}

TEST_CASE("loading detail: unknown values read n/a and a level past its table falls back to the first") {
    Info i;
    i.quality = 99;
    i.shadows = -1;
    i.textures = 7;
    i.antiAliasing = 4;
    CHECK(format(i) ==
          "Mission\n"
          "RAM   n/a\n"
          "VRAM  n/a\n"
          "Preset Custom\nQuality Potato  Shadows Potato  Textures Low  AA FSR 3.1");
    i.hasProgress = true;
    i.progress = 250.f;
    CHECK(format(i).substr(0, 13) == "Mission 100%\n");
    i.vramBudget = 2ull << 30;
    CHECK(format(i).find("VRAM  n/a\n") != std::string::npos);
    i.vramUsed = 0;
    CHECK(format(i).find("VRAM  0.0 / 2.0 GB\n") != std::string::npos);
}
