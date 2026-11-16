#include "doctest.h"
#include "../../code/world/entity_registry.h"
#include "../../code/world/spatial_grid.h"
#include "../../mclib/dmovemgr.h"
#include <limits>

namespace {
struct Object { virtual ~Object() = default; };
struct Mover : Object {};
struct Building : Object {};
}

TEST_CASE("WorldRegistry/slot order, typed lookup and recycled lifetimes") {
    mc2::world::EntityRegistry<Object> registry;
    auto** slots = registry.reset(4);
    Mover mover;
    Building building;
    slots[1] = &mover;
    slots[3] = &building;
    auto first = registry.id(1);
    CHECK(first.slot == 1);
    CHECK(registry.resolve<Mover>(first) == &mover);
    CHECK(registry.resolve<Building>(first) == nullptr);
    CHECK_FALSE(registry.id(0));
    CHECK_FALSE(registry.id(2));
    CHECK(registry.get(4) == nullptr);
    registry.invalidate(1, &building); // Wrong owner cannot invalidate a slot.
    CHECK(registry.resolve(first) == &mover);
    registry.invalidate(1, &mover);
    CHECK(registry.resolve(first) == nullptr);
    CHECK(registry.id(1) != first);
    CHECK(registry.get(3) == &building);
    auto recycled = registry.id(1);
    slots = registry.reset(4); // Load keeps slot numbers, never runtime IDs.
    slots[1] = &mover;
    CHECK(registry.resolve(recycled) == nullptr);
    mc2::world::EntityRegistry<Object> other;
    other.reset(4)[1] = &mover;
    CHECK(other.resolve(registry.id(1)) == nullptr);
    auto loaded = registry.id(1);
    registry.clear();
    CHECK(registry.resolve(loaded) == nullptr);
}

TEST_CASE("WorldGrid/conservative oracle and legacy order across negative cells") {
    mc2::world::SpatialGrid grid;
    struct Point { float x, y; };
    std::vector<Point> points;
    uint32_t rng = 7;
    auto coordinate = [&] { rng = rng * 1664525u + 1013904223u; return float(int(rng % 40000) - 20000); };
    for (unsigned i = 0; i < 1024; ++i) {
        points.push_back({coordinate(), coordinate()});
        grid.add(points.back().x, points.back().y, i, {1, i + 1, 1});
    }
    grid.finish();
    for (int q = 0; q < 100; ++q) {
        const float x = coordinate(), y = coordinate(), radius = float(q * 35);
        auto result = grid.query(x, y, radius);
        std::vector<bool> included(points.size());
        for (size_t j = 0; j < result.size(); ++j) {
            if (j) CHECK(result[j - 1].ordinal < result[j].ordinal);
            included[result[j].ordinal] = true;
            CHECK(result[j].id.slot == result[j].ordinal + 1);
        }
        for (size_t i = 0; i < points.size(); ++i) {
            const float dx = points[i].x - x, dy = points[i].y - y;
            if (dx * dx + dy * dy <= radius * radius) CHECK(included[i]);
        }
    }
    CHECK(grid.query(0, 0, 10).size() < points.size() / 10);
    CHECK(grid.query(0, 0, std::numeric_limits<float>::infinity()).size() == points.size());
    grid.clear();
    CHECK(grid.query(0, 0, 0).empty());
}

TEST_CASE("WorldGrid/boundaries, invalid coordinates and stable tie order") {
    mc2::world::SpatialGrid grid;
    for (unsigned i = 0; i < 30; ++i) grid.add(-512, 512, 29 - i, {1, 30 - i, 1});
    grid.add(std::numeric_limits<float>::quiet_NaN(), 0, 30, {});
    grid.finish();
    auto hits = grid.query(-512, 512, 0);
    REQUIRE(hits.size() == 31);
    for (unsigned i = 0; i < hits.size(); ++i) CHECK(hits[i].ordinal == i);
    CHECK(grid.query(1e30f, 0, 1).size() == 31);
    CHECK(grid.query(0, 0, -1).size() == 31);
}

TEST_CASE("WorldGrid/queryInto reuses dirty scratch with identical candidates and order") {
    mc2::world::SpatialGrid grid;
    for (unsigned i = 0; i < 64; ++i)
        grid.add(float(int(i % 8) * 512 - 2048), float(int(i / 8) * 512 - 2048), 63 - i, {1, i + 1, 1});
    grid.finish();
    std::vector<mc2::world::SpatialGrid::Candidate> scratch(7, {999, {9, 9, 9}});
    for (int q = 0; q < 64; ++q) {
        if (q == 56) grid.clear();
        const float x = q % 4 == 0 ? 1000000.0f + q : float(q * 73 - 2048);
        const float y = float(q * 41 - 1024);
        const float radius = q % 4 == 1 ? -1.0f : float(q * 13);
        const auto expected = grid.query(x, y, radius);
        grid.queryInto(x, y, radius, scratch);
        REQUIRE(scratch.size() == expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            CHECK(scratch[i].ordinal == expected[i].ordinal);
            CHECK(scratch[i].id == expected[i].id);
        }
    }
}

TEST_CASE("WorldGrid/1024 movers, 300 ticks, deterministic twice") {
    static_assert(MAX_MOVERS == 1024);
    auto run = [] {
        struct M { float x, y; };
        std::vector<M> movers;
        uint32_t rng = 12345;
        auto next = [&] { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        for (int i = 0; i < MAX_MOVERS; ++i) movers.push_back({float(next() % 6000), float(next() % 6000)});
        uint64_t hash = 1469598103934665603ull;
        mc2::world::SpatialGrid grid;
        for (int tick = 0; tick < 300; ++tick) {
            grid.clear();
            for (unsigned i = 0; i < movers.size(); ++i) {
                movers[i].x += float(int(next() % 9) - 4);
                movers[i].y += float(int(next() % 9) - 4);
                grid.add(movers[i].x, movers[i].y, i, {1, i + 1, 1});
            }
            grid.finish();
            for (unsigned i = 0; i < movers.size(); i += 8)
                for (auto& c : grid.query(movers[i].x, movers[i].y, 300.0f)) {
                    CHECK(c.ordinal < unsigned(MAX_MOVERS));
                    hash = (hash ^ c.ordinal) * 1099511628211ull;
                }
        }
        return hash;
    };
    const uint64_t a = run(), b = run();
    CHECK(a == b);
}
