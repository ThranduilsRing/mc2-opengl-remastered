// tests/unit/test_terrain_visual_smooth.cpp
// Load-time visual height: input (gameplay) heights are const, flats barely move, a terraced face melts.
#include "doctest.h"
#include "gameos/terrain_visual_smooth.h"
#include <cmath>
#include <vector>

TEST_CASE("visual smooth: terraced wall melts, flats stay, gameplay input untouched, corners bounded")
{
    const int N = 24;
    std::vector<float> e(static_cast<size_t>(N) * N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
            e[static_cast<size_t>(x) + static_cast<size_t>(y) * N] = (x < 10) ? 0.0f : (x < 14 ? 200.0f : 400.0f);  // two 200 wu risers
    const std::vector<float> before = e;
    const auto built = mc2terrain::buildSmoothedVisualHeight(e.data(), N, -1.0e9f);
    REQUIRE(built.has_value());
    const std::vector<float>& vh = *built;
    CHECK(e == before);
    const int V = (N - 1) * 4 + 1;
    REQUIRE(vh.size() == static_cast<size_t>(V) * V);
    const int row = V / 2;
    CHECK(std::fabs(vh[static_cast<size_t>(row) * V + 4]) < 4.5f);   // far flat: within the flat drift
    float maxStep = 0.0f;                                            // baseline riser is 200 wu in one cell (50 wu per fine step)
    for (int fx = 30; fx < 46; ++fx)
        maxStep = std::max(maxStep, std::fabs(vh[static_cast<size_t>(row) * V + fx + 1] - vh[static_cast<size_t>(row) * V + fx]));
    CHECK(maxStep < 45.0f);
    for (int cx = 0; cx < N; ++cx)                                   // drift stays inside the cliff limit
        CHECK(std::fabs(vh[static_cast<size_t>(row) * V + cx * 4] - before[static_cast<size_t>(cx) + static_cast<size_t>(row / 4) * N]) <= 56.0f + 1e-3f);
}
