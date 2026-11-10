#include "doctest.h"
#include "terrain_normal_load.h"
#include <array>
#include <optional>
#include <span>
#include <utility>
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

TEST_CASE("TerrainNormalLoad/grass slot takes the turf normal unless modded or rejected") {
    constexpr int kTurf = 100;
    const auto run = [](bool turfPresent, bool modded, bool acceptTurf, std::array<int, 6>& out, std::vector<std::pair<int, bool>>& loads) {
        return mc2::terrain::loadCookedNormals(std::span(out), modded,
            [&](int mat, bool turf) -> std::optional<int> {
                loads.emplace_back(mat, turf);
                if (turf) return turfPresent ? std::optional<int>(kTurf) : std::nullopt;
                return mat;
            },
            [&](std::span<const int> layers) { return acceptTurf || layers[1] != kTurf; });
    };
    std::array<int, 6> out{};
    std::vector<std::pair<int, bool>> loads;
    REQUIRE(run(true, false, true, out, loads));
    CHECK(out == std::array<int, 6>{0, kTurf, 2, 3, 4, 5});
    CHECK(loads[1] == std::pair<int, bool>{1, true});
    CHECK(loads.size() == 6u);                         // the turf replaces slot 1, no second read

    loads.clear();
    REQUIRE(run(false, false, true, out, loads));      // no turf cooked: the slot's own sidecar
    CHECK(out == std::array<int, 6>{0, 1, 2, 3, 4, 5});

    loads.clear();
    REQUIRE(run(true, true, true, out, loads));        // a mod's mat1_normal wins: the turf is never read
    CHECK(out[1] == 1);
    for (const auto& l : loads) CHECK_FALSE(l.second);

    loads.clear();
    REQUIRE(run(true, false, false, out, loads));      // the array rejects the turf: slot 1 reloads its own layer
    CHECK(out[1] == 1);
    CHECK(loads.back() == std::pair<int, bool>{1, false});

    CHECK_FALSE(mc2::terrain::loadCookedNormals(std::span(out), false,
        [](int mat, bool) -> std::optional<int> { return mat == 3 ? std::nullopt : std::optional<int>(mat); },
        [](std::span<const int>) { return true; }));    // a missing layer fails the cooked array
}
