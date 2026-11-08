#include "RenderCore/PropRecordCache.h"
#include "doctest.h"
#include <array>
#include <optional>

TEST_CASE("prop_record_cache: scene includes visibility, destruction and same-size edits") {
    PropInstanceGeneration scene;
    std::array<std::byte, 2> instances{};
    CHECK(scene.update(7, instances) == 1);
    CHECK(scene.update(7, instances) == 1);
    instances[1] = std::byte{1}; // transient transform, tint or LOD, same count
    CHECK(scene.update(7, instances) == 2);
    CHECK(scene.update(8, instances) == 3); // registry destruction, even with equal bytes
    CHECK(scene.update(8, std::span(instances).first(1)) == 4); // visibility
    CHECK(scene.update(8, {}) == 5);
    CHECK(scene.update(8, {}) == 5);
    CHECK(scene.update(0, instances) == 6); // registry reset on reload
}

TEST_CASE("prop_record_cache: each retired frame slot catches up independently") {
    const PropRecordCacheKey initial{1, 2, 3};
    CHECK_FALSE((initial == PropRecordCacheKey{2, 2, 3}));
    CHECK_FALSE((initial == PropRecordCacheKey{1, 3, 3})); // texture/material replacement
    CHECK_FALSE((initial == PropRecordCacheKey{1, 2, 4})); // map/resource replacement
    std::optional<PropRecordCacheKey> slots[2];
    CHECK(slots[0] != initial);
    slots[0] = initial; // only after this slot's core + late tickets retire
    CHECK(slots[0] == initial);
    CHECK(slots[1] != initial);
    slots[1] = initial;
    const PropRecordCacheKey reload{1, 2, 4};
    slots[0] = reload;
    CHECK(slots[0] == reload);
    CHECK(slots[1] != reload); // other in-flight slot keeps its old buffers
    CHECK(slots[1] == initial);
}
