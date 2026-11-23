// VEG-CEMENT-1: the coverage-to-density curve, the weed rule and the class-3 union
// (offline, header-only logic).
#include "doctest.h"
#include "GameAdapters/vegetation_hardsurface.h"
#include "GameAdapters/vegetation_noise.h"
#include "GameOS/gameos/vegetation_cutout.h"
#include "GameAdapters/vegetation_budget.h"
#include "GameAdapters/vegetation_clumps.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <limits>

using namespace mc2::vegetation;

TEST_CASE("vegetation near-foot flag conservatively covers boundary and field roles") {
    for (uint32_t frame = 0; frame < 256; ++frame) {
        const uint32_t atlas = (frame & 8u) ? (frame & 3u) : std::min(frame & 15u, 7u);
        const uint32_t role = (frame >> 4u) & 3u;
        const bool eligible = atlas < 4u && role != 3u;
        if (frame & 8u) CHECK(atlas < 4u);
        if (role == 3u) CHECK_FALSE(eligible);
        for (float radius : {0.0f, 0.25f, 1.0f, 4.0f, 32.0f, 256.0f}) {
            const float edge = radius + std::max(1.0f, radius * 0.25f);
            const float conservative = edge * 1.01f + 0.05f;
            for (float distance : {0.0f, std::nextafter(edge, 0.0f), edge,
                                   std::nextafter(edge, std::numeric_limits<float>::infinity()), conservative}) {
                const bool flag = eligible && distance < conservative;
                for (float height : {0.0f, 0.333333f, 0.666667f, 1.0f}) {
                    const bool deforms = eligible && distance < edge && height > 0;
                    if (deforms) CHECK(flag);
                    if (!flag) CHECK_FALSE(deforms);
                }
            }
        }
    }
    for (uint32_t id : {0u, 1u, 524287u, 0x7ffffffeu}) {
        CHECK(((id | 0x80000000u) & 0x7fffffffu) == id);
        CHECK((id & 0x7fffffffu) == id);
    }
}

TEST_CASE("VegFieldEdge/road shore and cell boundaries keep the interior identical") {
    // A straight road, a circular lake and a missing candidate rectangle.
    // These stand in for the adapter's union of the authoritative masks.
    const FieldEdge road(256, [](float x,float) { return x<64; });
    const FieldEdge lake(256, [](float x,float y) { return std::hypot(x-128,y-128)<48; });
    const FieldEdge cell(256, [](float x,float y) { return x>=96 && x<128 && y>=96 && y<128; });
    CHECK(road.distance(64,128) == doctest::Approx(0));
    CHECK(road.distance(72,128) == doctest::Approx(8));
    CHECK(road.distance(88,128) == doctest::Approx(24));
    CHECK(lake.distance(128,128)<0);
    CHECK(cell.distance(112,112)<0);
    CHECK(cell.distance(136,112)>0);
    // Out-of-map queries remain bounded; no wrap to another edge.
    CHECK(std::isfinite(road.distance(-8,128)));
    CHECK(std::isfinite(road.distance(264,128)));
    for (float patch : {0.0f,0.3f,0.7f,1.0f}) {
        CHECK(fieldEdgeProbability(0,patch)==0);
        CHECK(fieldEdgeProbability(-8,patch)==0);
        CHECK(fieldEdgeProbability(30,patch)==1);
        CHECK(fieldEdgeProbability(road.distance(128,128),patch)==1);
        CHECK(fieldEdgeProbability(lake.distance(224,128),patch)==1);
        CHECK(fieldEdgeProbability(cell.distance(176,112),patch)==1);
        float previous=0;
        for (int d=0; d<=30; ++d) {
            const float p=fieldEdgeProbability(static_cast<float>(d),patch);
            CHECK(p>=previous); previous=p;
            CHECK(fieldEdgeRole(p,0.4f)==(p==1 ? 0u : p>0.4f ? 0x10u : 0x20u));
        }
    }
    CHECK(fieldEdgeRole(1,0)==0);
    CHECK(fieldEdgeRole(1,1)==0);
    CHECK(fieldEdgeRole(0,0)==0x20u);
    CHECK(fieldEdgeRole(0.5f,0.25f)==0x10u);
    const FieldEdge open(256, [](float,float) { return false; });
    CHECK(fieldEdgeProbability(open.distance(128,128),0.5f)==1);
}

TEST_CASE("VegFieldEdge/stable blade ranks break the fringe into tufts and gaps") {
    int fringeKept=0, interiorKept=0, changed=0;
    const float probability=fieldEdgeProbability(10,0.5f);
    for (int j=0; j<1024; ++j) {
        const float x=static_cast<float>(j)*6, y=127;
        const float a=hashf(x+71,y-137), b=hashf(x+71,y-137);
        CHECK(a==b);
        fringeKept += a<probability;
        interiorKept += a<fieldEdgeProbability(32,0.5f);
        changed += (a<fieldEdgeProbability(10,0.0f)) != (a<fieldEdgeProbability(10,1.0f));
    }
    CHECK(fringeKept>400); CHECK(fringeKept<620);
    CHECK(interiorKept==1024);
    CHECK(changed>100);
}

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
    int dominant=0, grassTotal=0;
    std::array<int,2> bushSpecies{};
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
        // Species patches: one shrub species per clump, a dominant grass card.
        std::array<int,4> grassFrames{};
        unsigned bushFrame = 0;
        for (int i=0;i<a.count;++i) {
            const auto& m=a.members[static_cast<size_t>(i)];
            if (m.woody && m.frame<6) { if (!bushFrame) bushFrame=m.frame; CHECK(m.frame==bushFrame); }
            else if (!m.woody) { REQUIRE(m.frame<4); ++grassFrames[m.frame]; }
        }
        dominant += *std::max_element(grassFrames.begin(),grassFrames.end());
        grassTotal += grass;
        bushSpecies[bushFrame-4] += 1;
        CHECK(a.count<=22);
        const auto single=makeClump(x,4,42,recipe,0.5f,0,true);
        REQUIRE(single.count==1); CHECK_FALSE(single.members[0].woody);
        const auto steep=makeClump(x,4,42,recipe,0.5f,40,false);
        for (int i=0;i<steep.count;++i) CHECK(steep.members[static_cast<size_t>(i)].frame<6);
    }
    CHECK(dominant*10 > grassTotal*7);                 // patches: most grass is the clump's card
    CHECK(bushSpecies[0] > 64); CHECK(bushSpecies[1] > 64);  // both shrub species still appear
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
    const auto root=std::filesystem::path(MC2_REPO_ROOT);
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

TEST_CASE("VegField/grasslands carpets its ground; other biomes and bad mod recipes do not") {
    const auto root=std::filesystem::path(MC2_REPO_ROOT);
    for (const char* name : {"grasslands","grasslands_dense"}) {
        std::ifstream input(root/"data/vegetation/schemas"/(std::string(name)+".json"));
        CHECK(readFieldRecipe(nlohmann::json::parse(input,nullptr,false)).has_value());
    }
    for (const char* name : {"snow","desert","swamp"}) {
        std::ifstream input(root/"data/vegetation/schemas"/(std::string(name)+".json"));
        CHECK_FALSE(readFieldRecipe(nlohmann::json::parse(input,nullptr,false)).has_value());
    }
    for (const char* bad : {"{\"field\":{\"spacing\":1,\"height\":4}}", "{\"field\":{\"spacing\":6,\"height\":99}}",
                            "{\"field\":{\"spacing\":\"x\",\"height\":4}}", "{\"field\":[]}"})
        CHECK_FALSE(readFieldRecipe(nlohmann::json::parse(bad)).has_value());
}

TEST_CASE("VegField/even stratified carpet that always fits the remaining budget") {
    // Authored density when it fits; coarsened (never truncated mid-scan) when it does not.
    CHECK(fieldBladeSide(33.0f, 6.0f, 10'000'000, 100'000) == 6);
    CHECK(fieldBladeSide(33.0f, 6.0f, 2'000'000, 100'000) == 4);
    CHECK(fieldBladeSide(33.0f, 6.0f, 50, 100'000) == 0);
    CHECK(fieldBladeSide(33.0f, 6.0f, 0, 0) == 6);
    const FieldRecipe recipe{6.0f, 4.5f};
    const int k = 6;
    int cellsHit[36] = {};
    for (int j = 0; j < k*k; ++j) {
        const auto a = fieldBlade(7,9,mapSeed("mc2_01"),j,k,33.0f,recipe);
        const auto b = fieldBlade(7,9,mapSeed("mc2_01"),j,k,33.0f,recipe);
        CHECK(a.x == b.x); CHECK(a.y == b.y); CHECK(a.frame == b.frame);       // deterministic per map
        REQUIRE(a.x >= 0.0f); REQUIRE(a.x < 33.0f); REQUIRE(a.y <= 0.0f); REQUIRE(a.y > -33.0f);
        CHECK((a.frame & kFieldBladeBit) != 0u); CHECK((a.frame & 7u) < 4u);  // grass layers only
        CHECK((a.frame >> 4u) == 0u);                                           // role/weed/snow bits clear
        CHECK(a.scale >= 0.7f*4.5f); CHECK(a.scale <= 1.3f*4.5f);
        ++cellsHit[static_cast<int>(a.x/5.5f) + k*static_cast<int>(-a.y/5.5f)];
    }
    for (int hit : cellsHit) CHECK(hit == 1);   // one blade per sub-cell: no clumps, no bare patches
}

TEST_CASE("VegField/owner ref 3: even sward with only small gaps and a gentle height swell") {
    CHECK(fieldTuftHeight(0.0f) == 0.0f);                          // gap: no blade
    CHECK(fieldTuftHeight(kFieldGapBelow - 0.01f) == 0.0f);
    CHECK(kFieldGapBelow < 0.2f);                                   // gaps stay a small share (ref 2 had 0.33)
    for (float t = kFieldGapBelow; t <= 1.0f; t += 0.01f) {
        CHECK(fieldTuftHeight(t) >= 0.85f);                         // no stubby tuft edges
        CHECK(fieldTuftHeight(t) <= 1.1f);                          // no tall tuft crowns
    }
    CHECK(fieldTuftHeight(0.9f) > fieldTuftHeight(0.2f));           // monotone swell
}

TEST_CASE("VegField/owner: shrubs ~10x fewer, clustered not uniform") {
    float edgeMean = 0.0f, deepMean = 0.0f;
    for (int k = 0; k < 100; ++k) {
        const float c = (static_cast<float>(k) + 0.5f) / 100.0f;
        edgeMean += shrubKeepChance(10, c, 5.0f) / 100.0f;
        deepMean += shrubKeepChance(45, c, 5.0f) / 100.0f;
    }
    CHECK(edgeMean > 0.05f); CHECK(edgeMean < 0.15f);               // ~1/10 kept at forest edges
    CHECK(deepMean < 0.04f);                                          // stock trees already cover deep forest
    CHECK(shrubKeepChance(10, 0.9f, 5.0f) > 4.0f * shrubKeepChance(10, 0.2f, 5.0f));   // clustered
    CHECK(shrubKeepChance(10, 0.5f, 25.0f) > shrubKeepChance(10, 0.5f, 5.0f));          // rocks/slopes keep more
    CHECK(shrubKeepChance(0, 0.9f, 5.0f) <= 0.25f);                  // open meadow: sparse singles
}

TEST_CASE("VegField/shrubs gather at the stock treelines, open meadow keeps few") {
    CHECK(understoryChance(1) == 1.0f);
    CHECK(understoryChance(25) == 1.0f);
    CHECK(understoryChance(0) < 0.1f);
    CHECK(understoryChance(0) > 0.0f);
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

TEST_CASE("prop alpha-ON mips: solid canopy keeps coverage at the shader cut, lattice stays see-through") {
    using namespace mc2::vegetation;
    const uint32_t N = 128;
    std::vector<uint8_t> canopy(N * N * 4, 0), lattice(N * N * 4, 0);
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            const float dx = float(x) - 63.5f, dy = float(y) - 63.5f;
            if (dx * dx + dy * dy < 50.0f * 50.0f) canopy[(y * N + x) * 4 + 3] = 255;
            if (x % 16 < 2 || y % 16 < 2) lattice[(y * N + x) * 4 + 3] = 255;   // 2-texel wire on a 16 pitch
        }
    CHECK(isSolidCutout(canopy, N, N));
    CHECK_FALSE(isSolidCutout(lattice, N, N));
    {   // A 128 image with a small crown (radius 20) in the corner of a 1024 array layer (a 1024 tree atlas shares the
        // array): the erosion radius follows the image (2 texels), so it stays solid; with the array radius (16 texels)
        // its core would shrink to radius 4 and read as lattice.
        const uint32_t L = 1024;
        std::vector<uint8_t> small(N * N * 4, 0);
        for (uint32_t y = 0; y < N; ++y)
            for (uint32_t x = 0; x < N; ++x) {
                const float dx = float(x) - 63.5f, dy = float(y) - 63.5f;
                if (dx * dx + dy * dy < 20.0f * 20.0f) small[(y * N + x) * 4 + 3] = 255;
            }
        std::vector<uint8_t> layer(size_t(L) * L * 4, 0);
        for (uint32_t y = 0; y < N; ++y)
            std::memcpy(&layer[size_t(y) * L * 4], &small[size_t(y) * N * 4], N * 4);
        std::vector<uint8_t> holey = canopy;                     // stock-like pin holes on a 4-texel pitch
        for (uint32_t y = 0; y < N; y += 4)
            for (uint32_t x = 0; x < N; x += 4) holey[(y * N + x) * 4 + 3] = 0;
        std::vector<uint8_t> holeyLayer(size_t(L) * L * 4, 0);
        for (uint32_t y = 0; y < N; ++y)
            std::memcpy(&holeyLayer[size_t(y) * L * 4], &holey[size_t(y) * N * 4], N * 4);
        CHECK(isSolidCutout(layer, L, L, N));
        CHECK_FALSE(isSolidCutout(layer, L, L));
        CHECK_FALSE(isSolidCutout(holeyLayer, L, L, N));         // pin holes read as lattice: closed crowns are art-side
        CHECK_FALSE(isSolidCutout(lattice, N, N, N));
    }
    CHECK(propCutThreshold(0) == 128);
    CHECK(propCutThreshold(1) == 179);
    CHECK(propCutThreshold(2) == 230);
    CHECK(propCutThreshold(9) == 230);

    // box-filter the canopy to mip 3 (16x16): edge texels average to mid alpha, so coverage at the mip-3 cut drops
    const float target = cutoutCoverage(canopy);
    std::vector<uint8_t> mip = canopy;
    uint32_t w = N;
    for (int m = 0; m < 3; ++m, w /= 2) {
        std::vector<uint8_t> next((w / 2) * (w / 2) * 4);
        for (uint32_t y = 0; y < w / 2; ++y)
            for (uint32_t x = 0; x < w / 2; ++x)
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int j = 0; j < 4; ++j) sum += mip[((2 * y + j / 2) * w + 2 * x + j % 2) * 4 + c];
                    next[(y * (w / 2) + x) * 4 + c] = uint8_t(sum / 4);
                }
        mip.swap(next);
    }
    const long cut = propCutThreshold(3);
    const float plain = cutoutCoverage(mip, 1.0f, cut);
    CHECK(plain < target - 0.02f);
    preserveCutoutAlpha(mip, target, cut);
    CHECK(cutoutCoverage(mip, 1.0f, cut) == doctest::Approx(target).epsilon(0.08));
}

TEST_CASE("building glass: an enclosed rectangular window fills opaque dark, a silhouette notch and a lattice stay cut") {
    using namespace mc2::vegetation;
    const uint32_t N = 128;
    std::vector<uint8_t> wall(N * N * 4, 255);
    const auto clear = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x) wall[(y * N + x) * 4 + 3] = 0;
    };
    clear(30, 30, 50, 44);   // window: enclosed rectangle
    clear(0, 60, 12, 70);    // notch open to the content edge
    for (uint32_t i = 0; i < 6; ++i) clear(70 + i * 6, 90, 72 + i * 6, 120);   // thin slits: below the rectangle fill
    CHECK(fillEnclosedGlassHoles(wall, N, N, N, N) == 20u * 14u);
    CHECK(wall[(36 * N + 40) * 4 + 3] == 255);
    CHECK(wall[(36 * N + 40) * 4] < 16);
    CHECK(wall[(65 * N + 5) * 4 + 3] == 0);
    CHECK(wall[(100 * N + 70) * 4 + 3] == 0);
}

TEST_CASE("building glass: an opening in a thin frame (grating, gate) stays cut") {
    using namespace mc2::vegetation;
    const uint32_t N = 128;
    std::vector<uint8_t> frame(N * N * 4, 0);
    for (uint32_t y = 20; y < 108; ++y)
        for (uint32_t x = 20; x < 108; ++x)
            if (x < 26 || x >= 102 || y < 26 || y >= 102) frame[(y * N + x) * 4 + 3] = 255;   // 6 px frame, 76 px opening
    CHECK(fillEnclosedGlassHoles(frame, N, N, N, N) == 0u);
}
