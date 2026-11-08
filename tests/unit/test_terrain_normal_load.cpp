#include "doctest.h"
#include "terrain_normal_load.h"
#include <array>
#include <vector>

TEST_CASE("TerrainNormalLoad/displacement files never gate normals or change CPU alpha") {
    const std::array<unsigned char, 16> rgba{1,2,3,0, 4,5,6,127, 7,8,9,255, 10,11,12,42};
    std::array<unsigned char, 4> reference{};
    for (bool displacementPresent : {false, true}) {
        for (bool cooked : {false, true}) {
            std::array<unsigned char, 4> alpha{};
            std::vector<int> reads;
            CHECK(mc2::terrain::loadNormalSources(cooked, [&](int mat) {
                reads.push_back(mat);
                if (mat == 2) mc2::terrain::copyDisplacementAlpha(rgba, alpha);
                return true;
            }));
            CHECK(alpha == std::array<unsigned char, 4>{0,127,255,42});
            if (displacementPresent) CHECK(alpha == reference);
            reference = alpha;
            if (cooked) CHECK(reads == std::vector<int>{2});
            else CHECK(reads == std::vector<int>{0,1,2,3,4,5,6,7,8});
        }
    }
    CHECK_FALSE(mc2::terrain::loadNormalSources(false, [](int mat) { return mat != 1; }));
    CHECK_FALSE(mc2::terrain::loadNormalSources(true, [](int) { return false; }));
}
