#include "RenderCore/PropRecordCache.h"
#include "doctest.h"
#include <array>
#include <optional>

TEST_CASE("prop_record_cache: population patch preserves packets and absolute ranges") {
    struct Command { uint32_t firstInstance, instanceCount, packet; };
    struct Run { uint32_t first, count, range[2][2], pad[2]; };
    std::vector<Command> commands[2]{{{0, 2, 7}, {0, 2, 8}}, {{0, 2, 9}}};
    const uint32_t starts[]{0, 2};
    Run run{0, 2, {{0, 2}, {2, 1}}, {}};
    patchPropRun(run, commands, starts, 3, 5);
    CHECK(run.first == 3);
    CHECK(run.count == 5);
    CHECK(run.range[1][0] == 2);
    for (const auto& list : commands) for (const auto& cmd : list) {
        CHECK(cmd.firstInstance == 3);
        CHECK(cmd.instanceCount == 5);
    }
    CHECK(commands[0][0].packet == 7);
    CHECK(commands[0][1].packet == 8);
    CHECK(commands[1][0].packet == 9);
}

TEST_CASE("prop_record_cache: count changes preserve ordered packet topology") {
    struct Instance { uint32_t typeID; };
    std::array<Instance, 4> data{{{1}, {1}, {4}, {4}}};
    PropTypeGeneration types;
    auto feed = std::span<const Instance>(data);
    types.update(feed);
    const auto topology = types.topology;
    types.update(feed.first(3)); // destroy one actor, both types remain
    CHECK(types.topology == topology);
    types.update(feed); // add actor of existing type
    CHECK(types.topology == topology);
    types.update(feed.first(2)); // remove last actor of a type
    CHECK(types.topology == topology + 1);
    types.update(feed); // restore type
    CHECK(types.topology == topology + 2);
    data[0].typeID = 4; // reorder runs
    types.update(feed);
    CHECK(types.topology == topology + 3);
}

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

TEST_CASE("prop_record_cache: recipe replacement preserves object ID invalidation") {
    struct Instance { uint32_t typeID; };
    const std::array<Instance, 2> data{{{4}, {4}}};
    PropTypeGeneration types;
    uint32_t recipe = 7, lookups = 0;
    auto identity = [&](uint32_t type) { ++lookups; return (static_cast<uint64_t>(type) << 32) | recipe; };
    CHECK(types.update(std::span<const Instance>(data), identity) == 1);
    CHECK(types.update(std::span<const Instance>(data), identity) == 1);
    CHECK(lookups == 2); // one lookup per run, not per instance
    recipe = 8; // same type/count, new recipe owner after spawn/destruction
    CHECK(types.update(std::span<const Instance>(data), identity) == 2);
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
