// VEG-CEMENT-1: the coverage-to-density curve, the weed rule and the class-3 union
// (offline, header-only logic).
#include "doctest.h"
#include "GameAdapters/vegetation_hardsurface.h"
#include "GameOS/gameos/vegetation_cutout.h"
#include "GameAdapters/vegetation_budget.h"
#include "GameAdapters/vegetation_clumps.h"
#include <filesystem>
#include <fstream>

using namespace mc2::vegetation;

TEST_CASE("VegClumps/deterministic per-map blue-noise centres") {
    const auto seed = mapSeed("mc2_01");
    CHECK(seed != mapSeed("mc2_02"));
    int accepted=0, changed=0;
    for (int y=-12;y<12;++y) for (int x=-12;x<12;++x) {
        const auto a = clumpCandidate(x,y,seed), b = clumpCandidate(x,y,mapSeed("mc2_01"));
        CHECK(a.x==b.x); CHECK(a.y==b.y); CHECK(a.priority==b.priority);
        changed += a.x != clumpCandidate(x,y,mapSeed("mc2_02")).x;
        if (!clumpCentreAccepted(x,y,seed)) continue;
        ++accepted;
        for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
            if ((!dx&&!dy) || !clumpCentreAccepted(x+dx,y+dy,seed)) continue;
            const auto n=clumpCandidate(x+dx,y+dy,seed);
            CHECK((n.x-a.x)*(n.x-a.x)+(n.y-a.y)*(n.y-a.y) >= 0.65f*0.65f);
        }
    }
    CHECK(accepted>200); CHECK(accepted<576); CHECK(changed>550);
}

TEST_CASE("VegClumps/layered composition and sparse singles") {
    for (int x=0;x<256;++x) {
        const ClumpRecipe recipe{9,1,0,12};
        const auto a=makeClump(x,4,mapSeed("mc2_01"),recipe,0.5f,0,false);
        const auto b=makeClump(x,4,mapSeed("mc2_01"),recipe,0.5f,0,false);
        CHECK(a.count==b.count);
        int trees=0,bushes=0,grass=0;
        for (int i=0;i<a.count;++i) {
            const auto& m=a.members[static_cast<size_t>(i)];
            const auto& n=b.members[static_cast<size_t>(i)];
            CHECK(m.x==n.x); CHECK(m.y==n.y); CHECK(m.frame==n.frame); CHECK(m.scale==n.scale);
            const float r=std::sqrt(m.x*m.x+m.y*m.y);
            if (m.frame>=6) { ++trees; CHECK(r==0); CHECK(m.scale>=15); }
            else if (m.woody) { ++bushes; CHECK(r<=9*0.36f); CHECK(m.scale>=5); CHECK(m.scale<9); }
            else { ++grass; CHECK(r>=9*0.3f); CHECK(r<=9); CHECK(m.scale<5); }
        }
        CHECK(trees==1); CHECK(bushes>=2); CHECK(bushes<=5); CHECK(grass>=6);
        CHECK(a.count<=22);
        const auto single=makeClump(x,4,42,recipe,0.5f,0,true);
        REQUIRE(single.count==1); CHECK_FALSE(single.members[0].woody);
        const auto steep=makeClump(x,4,42,recipe,0.5f,40,false);
        for (int i=0;i<steep.count;++i) CHECK(steep.members[static_cast<size_t>(i)].frame<6);
    }
}

TEST_CASE("VegClumps/member masks reject roads water buildings stock trees and slope") {
    for (bool woody : {false,true}) {
        CHECK(clumpSurfaceAllowed(true,false,false,0,0,woody));
        CHECK_FALSE(clumpSurfaceAllowed(false,false,false,0,0,woody));
        CHECK_FALSE(clumpSurfaceAllowed(true,true,false,0,0,woody));
        CHECK_FALSE(clumpSurfaceAllowed(true,false,true,0,0,woody));
        CHECK_FALSE(clumpSurfaceAllowed(true,false,false,1,0,woody));
        CHECK_FALSE(clumpSurfaceAllowed(true,false,false,0.15f,0,woody));
        CHECK_FALSE(clumpSurfaceAllowed(true,false,false,0,60,woody));
    }
    CHECK_FALSE(clumpSurfaceAllowed(true,false,false,0,36,true));
    CHECK(clumpSurfaceAllowed(true,false,false,0,36,false));
}

TEST_CASE("VegClumps/load shipped biome recipes and reject unsafe mod recipes") {
    const auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    for (const char* name : {"grasslands","grasslands_dense","snow","desert","swamp"}) {
        std::ifstream input(root/"data/vegetation/schemas"/(std::string(name)+".json"));
        REQUIRE(input.good());
        CHECK(readClumpRecipe(nlohmann::json::parse(input,nullptr,false)).has_value());
    }
    for (const char* bad : {"null", "{}", "{\"clump\":[]}",
         "{\"clump\":{\"radius\":999,\"tree_chance\":1,\"moisture_bias\":0,\"grass\":12}}",
         "{\"clump\":{\"radius\":9,\"tree_chance\":\"oops\",\"moisture_bias\":0,\"grass\":12}}"}) {
        CHECK_FALSE(readClumpRecipe(nlohmann::json::parse(bad)).has_value());
    }
    const ClumpRecipe recipeWet{9,0,0,16};
    const auto dry=makeClump(1,2,3,recipeWet,0,0,false), wet=makeClump(1,2,3,recipeWet,1,0,false);
    CHECK(wet.count>dry.count);
}

TEST_SUITE("VegHardSurface") {

TEST_CASE("cover density falls smoothly and monotonically with coverage") {
    CHECK(coverToDensityMul(0.0f) == doctest::Approx(1.0f));
    CHECK(coverToDensityMul(kHardFalloffLo) == doctest::Approx(1.0f));
    CHECK(coverToDensityMul(kHardFalloffHi) == doctest::Approx(0.0f));
    CHECK(coverToDensityMul(1.0f) == doctest::Approx(0.0f));
    float prev = 1.0f;
    for (int i = 0; i <= 100; ++i) {
        const float m = coverToDensityMul(float(i) / 100.0f);
        CHECK(m <= prev + 1e-6f);
        // No staircase: adjacent 1% coverage steps never jump by more than 4%.
        CHECK(prev - m < 0.04f);
        prev = m;
    }
}

TEST_CASE("weeds need real hard surface and favour cracks and edges") {
    CHECK(weedKeepProbability(0.49f, 1.0f) == 0.0f);
    CHECK(weedKeepProbability(0.5f, 0.0f) > 0.0f);
    CHECK(weedKeepProbability(1.0f, 1.0f) == doctest::Approx(kWeedDensity));
    CHECK(weedKeepProbability(1.0f, 1.0f) > weedKeepProbability(1.0f, 0.0f));
    // Interior of a pad (flat coverage, away from seams) stays sparse.
    CHECK(weedKeepProbability(1.0f, crackTerm(0.0f, 50.0f)) < 0.5f * kWeedDensity);
}

TEST_CASE("crack term: coverage gradient and seams") {
    CHECK(crackTerm(0.0f, 50.0f) == 0.0f);
    CHECK(crackTerm(0.1f, 50.0f) == doctest::Approx(1.0f));
    CHECK(crackTerm(0.0f, 1.0f) > 0.0f);
}

TEST_CASE("producer follows analytic pad edge instead of skipped overlay alpha") {
    const auto edge = [](int r, int c) -> uint32_t {
        if (r == 1 && c == 1) return 0xc0000000u;
        return r >= 0 && r <= 1 && c >= 0 && c <= 1 ? 0x80000000u : 0u;
    };
    CHECK(cementWordCoverage(1,1,0.1f,0.1f,0.0f,edge) == 1.0f);
    CHECK(cementWordCoverage(1,1,0.4f,0.4f,1.0f,edge) == 0.0f);
    CHECK(renderedHardSurfaceCoverage(cementWordCoverage(1,1,0.4f,0.4f,1,edge), 1, false) == 0);
    CHECK(renderedHardSurfaceCoverage(cementWordCoverage(1,1,0.4f,0.4f,1,edge), 0.7f, true) == doctest::Approx(0.7f));
    const auto own = [](int, int) { return 0xc0800000u; };
    CHECK(cementWordCoverage(0,0,0.1f,0.1f,0.3f,own) == doctest::Approx(0.3f));
    const auto solid = [](int, int) { return 0x80000000u; };
    CHECK(renderedHardSurfaceCoverage(cementWordCoverage(0,0,0.5f,0.5f,0,solid), 0.3f, true) == 1);
    CHECK((kWeedFlagBit & 0x3fu) == 0);
}

TEST_CASE("producer retains cement beneath dirt-road and rough overlays") {
    const auto slab = [](int, int) { return 0x80000000u; };
    const auto dirt = [](int, int) { return 0u; };
    for (float overlayAlpha : {0.0f, 0.5f, 1.0f}) {
        const float cement = cementWordCoverage(0,0,0.5f,0.5f,0,slab);
        CHECK(renderedHardSurfaceCoverage(cement, overlayAlpha, false) == 1);
        CHECK(renderedHardSurfaceCoverage(cementWordCoverage(0,0,0.5f,0.5f,0,dirt), overlayAlpha, false) == 0);
    }
}

TEST_CASE("placement samples coverage and keeps weeds sparse away from seams") {
    const auto flat = [](float, float) { return 1.0f; };
    const auto open = [](float, float) { return 0.0f; };
    const auto transition = [](float, float) { return 0.3f; };
    const auto zeroRoll = [](float, float) { return 0.0f; };
    const auto highRoll = [](float, float) { return 0.99f; };
    CHECK(hardSurfacePlacement(64, 64, 128, open, highRoll) == HardSurfacePlacement::Cover);
    CHECK(hardSurfacePlacement(64, 64, 128, transition, zeroRoll) == HardSurfacePlacement::TransitionCover);
    CHECK(hardSurfacePlacement(64, 64, 128, flat, zeroRoll) == HardSurfacePlacement::Weed);
    CHECK(hardSurfacePlacement(64, 64, 128, flat, highRoll) == HardSurfacePlacement::Reject);

    // The cover roll loses; a 5% weed roll survives at a seam but not inside a pad.
    const auto rolls = [] { return [call = 0](float, float) mutable { return call++ == 0 ? 0.99f : 0.05f; }; };
    CHECK(hardSurfacePlacement(64, 64, 128, flat, rolls()) == HardSurfacePlacement::Reject);
    CHECK(hardSurfacePlacement(0, 64, 128, flat, rolls()) == HardSurfacePlacement::Weed);
}

}

TEST_CASE("vegetation mips preserve hard-test coverage without making empty texels opaque") {
    std::vector<uint8_t> rgba(64 * 4);
    for (size_t i = 0; i < 64; ++i) rgba[i * 4 + 3] = uint8_t(i);
    mc2::vegetation::preserveCutoutAlpha(rgba, 0.5f);
    CHECK(mc2::vegetation::cutoutCoverage(rgba) == doctest::Approx(0.5f));
    CHECK(rgba[3] == 0);
    std::fill(rgba.begin(), rgba.end(), uint8_t(0));
    mc2::vegetation::preserveCutoutAlpha(rgba, 0.5f);
    CHECK(mc2::vegetation::cutoutCoverage(rgba) == 0.0f);
}

TEST_CASE("vegetation colour dilation keeps alpha and filled texels intact") {
    std::vector<uint8_t> rgba(3 * 3 * 4, 0);
    rgba[16] = 120; rgba[17] = 80; rgba[18] = 40; rgba[19] = 255;
    mc2::vegetation::dilateCutoutRgb(rgba, 3, 3);
    CHECK(rgba[16] == 120);
    CHECK(rgba[20] == 120);
    CHECK(rgba[21] == 80);
    CHECK(rgba[22] == 40);
    CHECK(rgba[23] == 0);
    CHECK(mc2::vegetation::cutoutCoverage(rgba) == doctest::Approx(1.0f / 9.0f));
}

TEST_CASE("vegetation packed population retains map precision and foliage roles") {
    using namespace mc2::vegetation;
    for (uint32_t flags : {0u, 3u, 0x32u, kWeedFlagBit | 0x13u}) {
        const auto p = packInstance(-131071.125f, 65535.75f, 231.375f, 6.27f, 5.625f, flags, 0.4375f);
        CHECK(p.x == -131071.125f);
        CHECK(p.y == 65535.75f);
        const auto hs = glm::unpackHalf2x16(p.heightScale);
        CHECK(hs.x == doctest::Approx(231.375f));
        CHECK(hs.y == doctest::Approx(5.625f));
        CHECK((p.appearance >> 24) == flags);
        CHECK(glm::unpackHalf2x16(p.appearance & 65535u).x == 0.4375f);
        CHECK(std::abs(float((p.appearance >> 16) & 255u) * (6.28318530718f / 256) - 6.27f) < 0.013f);
    }
    CHECK(kVisibleCapacity * 8ull == 4ull * 1024 * 1024);
}

TEST_CASE("VegCards/a missing card degrades per layer, not all-or-nothing") {
    const bool all[8] = {true,true,true,true,true,true,true,true};
    const auto full = cardLayerSources(all);
    for (int i = 0; i < 8; ++i) CHECK(full[i] == i);
    // Seed install: grass present, the four newer cards absent.
    const bool grassOnly[8] = {true,true,true,true,false,false,false,false};
    const auto s = cardLayerSources(grassOnly);
    for (int i = 0; i < 4; ++i) CHECK(s[i] == i);
    for (int i = 4; i < 8; ++i) CHECK(s[i] >= 0);
    // One shrub missing takes its group neighbour.
    const bool one[8] = {true,true,true,true,false,true,true,true};
    CHECK(cardLayerSources(one)[4] == 5);
    const bool none[8] = {};
    CHECK(cardLayerSources(none).empty());
}
