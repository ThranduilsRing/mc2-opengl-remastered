#include "doctest.h"
#include "terrain_cache_key.h"

// Trigger: mclib/terrain_cache_key.h or mclib/terrtxm.cpp.
TEST_CASE("terrain composite cache reuses unchanged inputs and invalidates edits") {
    auto key = [](mc2::save::Bytes tile, std::string_view recipe = "txm-composite-v1",
                  mc2::save::Bytes mask = {7}, mc2::save::Bytes fit = {9}) {
        mc2::TerrainCacheKey inputs(recipe);
        inputs.add("terrain.fit", fit);
        inputs.add("64/cement.tga", { });
        inputs.add("64mask/mask.tga", mask);
        inputs.add("256overlays/runway0000.tga", tile);
        return inputs.key();
    };
    const auto cached = key({1, 2, 3});
    CHECK(cached == key({1, 2, 3})); // Same bytes reuse, even after a timestamp-only touch.
    CHECK(cached != key({1, 8, 3})); // Same size/mtime content edit still rebuilds.
    CHECK(cached != key({1, 2, 3, 4}));
    CHECK(cached != key({1, 2, 3}, "txm-composite-v2"));
    CHECK(cached != key({1, 2, 3}, "txm-composite-v1", {8}));
    CHECK(cached != key({1, 2, 3}, "txm-composite-v1", {7}, {10}));
    CHECK(cached != key({})); // Removed source.
}
