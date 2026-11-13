#include "RenderCore/PropRecordCache.h"
#include "doctest.h"
#include <array>
#include <optional>

TEST_CASE("prop_record_cache: lighting and animation do not rebuild type runs") {
    struct Instance { uint32_t typeID, light, transform; };
    std::array<Instance, 3> data{{{1, 0, 0}, {1, 0, 0}, {4, 0, 0}}};
    PropTypeGeneration types;
    PropInstanceGeneration bytes;
    auto feed = [&] { return std::span<const Instance>(data); };
    CHECK(types.update(feed()) == 1);
    CHECK(bytes.update(7, std::as_bytes(feed())) == 1);
    for (uint32_t frame = 1; frame <= 100; ++frame) {
        data[1].light = frame;
        data[2].transform = frame;
        CHECK(types.update(feed()) == 1);
        CHECK(bytes.update(7, std::as_bytes(feed())) == frame + 1);
    }
    data[1].typeID = 4; // LOD change with the same instance count
    CHECK(types.update(feed()) == 2);
    CHECK(types.update(feed().first(2)) == 3); // visibility / destruction
    std::swap(data[0], data[1]);
    CHECK(types.update(feed().first(2)) == 4); // ordering matters
    CHECK(types.update(std::span<const Instance>{}) == 5);
    CHECK(types.update(std::span<const Instance>{}) == 5);
}

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
