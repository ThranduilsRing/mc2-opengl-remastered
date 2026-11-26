// tests/unit/test_terrain_visual_smooth.cpp
// Load-time visual height: input (gameplay) heights are const, flats barely move, a terraced face melts.
#include "doctest.h"
#include "gameos/terrain_visual_smooth.h"
#include <cmath>
#include <vector>
#include <stdexcept>

TEST_CASE("visual smooth: worker exceptions return to the caller") {
    CHECK_THROWS_AS(mc2terrain::parallelBands(128, [](int first, int) {
        if (first > 0 || std::thread::hardware_concurrency() < 8) throw std::runtime_error("worker");
    }), std::runtime_error);
}

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
    float maxDrift = 0.0f;
    for (int cx = 0; cx < N; ++cx)                                   // drift stays inside the cliff limit
        maxDrift = std::max(maxDrift, std::fabs(vh[static_cast<size_t>(row) * V + cx * 4] - before[static_cast<size_t>(cx) + static_cast<size_t>(row / 4) * N]));
    MESSAGE("terraced wall maximum coarse-vertex drift (wu): ", maxDrift);
    CHECK(maxDrift <= 72.0f + 1e-3f);
}

TEST_CASE("ravine weathering: only blocked cells move, passable cell centres keep their height, mask untouched")
{
    const int N = 32, C = N * 3, V = (N - 1) * 4 + 1;
    std::vector<float> e(static_cast<size_t>(N) * N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)   // diagonal staircase wall, 300 wu high
            e[static_cast<size_t>(x) + static_cast<size_t>(y) * N] = 100.0f * static_cast<float>(std::clamp(x + y / 2 - 19, 0, 3));
    std::vector<unsigned char> blocked(static_cast<size_t>(C) * C, 0);
    for (int r = 0; r < C; ++r)
        for (int c = 0; c < C; ++c) {
            const int tx = std::min(c / 3, N - 2), ty = std::min(r / 3, N - 2);
            const float a = e[tx + ty * N], b = e[tx + 1 + ty * N], d = e[tx + (ty + 1) * N], f = e[tx + 1 + (ty + 1) * N];
            if (std::max({a, b, d, f}) - std::min({a, b, d, f}) > 90.0f) blocked[static_cast<size_t>(r) * C + c] = 1;
        }
    const std::vector<unsigned char> maskBefore = blocked;
    const auto plain = mc2terrain::buildSmoothedVisualHeight(e.data(), N, -1.0e9f);
    CHECK(mc2terrain::buildSmoothedVisualHeight(e.data(), N, -1.0e9f, nullptr, -1) == plain); // missing/non-square MOVE fallback
    const auto carved = mc2terrain::buildSmoothedVisualHeight(e.data(), N, -1.0e9f, &blocked, C);
    REQUIRE(plain.has_value());
    REQUIRE(carved.has_value());
    CHECK(blocked == maskBefore);
    auto at = [&](const std::vector<float>& f, float x, float y) {
        const int ix = std::min(static_cast<int>(x), V - 2), iy = std::min(static_cast<int>(y), V - 2);
        const float u = x - ix, v = y - iy;
        const size_t i = static_cast<size_t>(ix) + static_cast<size_t>(iy) * V;
        return (f[i] * (1 - u) + f[i + 1] * u) * (1 - v) + (f[i + V] * (1 - u) + f[i + V + 1] * u) * v;
    };
    float worstPassable = 0.0f, maxMoved = 0.0f;
    for (int r = 0; r < C - 3; ++r)
        for (int c = 0; c < C - 3; ++c) {
            const float x = (c + 0.5f) * 4.0f / 3.0f, y = (r + 0.5f) * 4.0f / 3.0f, dh = std::fabs(at(*carved, x, y) - at(*plain, x, y));
            if (blocked[static_cast<size_t>(r) * C + c]) maxMoved = std::max(maxMoved, dh);
            else worstPassable = std::max(worstPassable, dh);
        }
    CHECK(worstPassable < 0.5f);
    CHECK(maxMoved > 30.0f);   // the staircase inside the blocked band really is reshaped
}
