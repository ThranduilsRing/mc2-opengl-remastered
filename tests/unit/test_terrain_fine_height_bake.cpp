// The terrain-owned filtered height field the fine-normal fetch reads: one baked cell per fragment tap, exactly
// cliffVisualHeight at interior, border (p = -1 and p = side) and cliff cells.
#include "doctest.h"
#include "gameos/terrain_fine_height_bake.h"

#include <cstdint>
#include <vector>

TEST_CASE("terrain fine height bake equals per-tap cliffVisualHeight on the apron, border and cliff cells") {
    const int V = 37;
    std::vector<float> raw(static_cast<size_t>(V) * V);
    uint32_t s = 12345u;
    const auto rnd = [&s] { s = s * 1664525u + 1013904223u; return static_cast<float>(s >> 8) / 16777216.0f; };
    for (int y = 0; y < V; ++y)
        for (int x = 0; x < V; ++x)
            raw[static_cast<size_t>(y) * V + x] = 10.0f * rnd() + ((x / 6 + y / 5) % 3 == 0 ? 90.0f : 0.0f); // plateaus: rise > 24 across their edges
    std::vector<float> baked(terrain_fine_height::apronCount(V));
    terrain_fine_height::bake(raw.data(), V, baked.data());
    const auto tap = [&](int x, int y) { return raw[static_cast<size_t>(std::clamp(y, 0, V - 1)) * V + std::clamp(x, 0, V - 1)]; };
    int cliff = 0, border = 0;
    for (int y = -1; y <= V; ++y)
        for (int x = -1; x <= V; ++x) {
            const float expect = terrain_fine_height::cliffVisualHeight(tap, x, y);
            REQUIRE(baked[static_cast<size_t>(y + 1) * (V + 2) + (x + 1)] == expect);
            cliff += expect != tap(x, y);
            border += (x < 0 || y < 0 || x >= V || y >= V);
        }
    CHECK(cliff > 50);               // the filtered branch was exercised
    CHECK(border == 4 * V + 4);      // p = -1 and p = side rings
}
