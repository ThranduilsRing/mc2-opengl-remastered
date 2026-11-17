#include "doctest.h"
#include "../../GameOS/gameos/vulkan_terrain_spec_key.h"
#include "../../RenderCore/gpu_abi/GpuTerrainFrameUBO.h"
#include <atomic>
#include <memory>
#include <set>
#include <thread>

using namespace mc2::terrain;
using Pipe = std::unique_ptr<int>;
using Table = TerrainSpecVariants<Pipe>;
static TerrainFrameUBO shipping() {
    TerrainFrameUBO u{};
    u.colormapReady = u.detailReady = u.visualMode = 2;
    u.useCement = u.useTransitionMask = u.lightingEnabled = u.enableShadows = u.detailEnabled = u.shorelineOn = 1;
    u.concreteDetail = 3;
    return u;
}
TEST_CASE("terrain_spec/key separates every shader state and round trips") {
    auto u = shipping();
    const auto key = terrainSpecKey(u);
    CHECK(key.values() == std::array<int32_t,12>{{2,1,1,0,1,1,1,2,3,2,1,0}});
    int32_t TerrainFrameUBO::* fields[] = {&TerrainFrameUBO::colormapReady,
        &TerrainFrameUBO::useCement, &TerrainFrameUBO::useTransitionMask, &TerrainFrameUBO::cementDiagConnect,
        &TerrainFrameUBO::lightingEnabled, &TerrainFrameUBO::enableShadows, &TerrainFrameUBO::detailEnabled,
        &TerrainFrameUBO::detailReady, &TerrainFrameUBO::concreteDetail, &TerrainFrameUBO::visualMode,
        &TerrainFrameUBO::shorelineOn, &TerrainFrameUBO::shorelineHasMask, &TerrainFrameUBO::csmDebugMode};
    for (auto field : fields) { auto changed = u; changed.*field ^= 1; CHECK(terrainSpecKey(changed) != key); }
    CHECK(terrainSpecKey(u, true) != key);
    for (unsigned i = 0; i < 3; ++i) {
        std::array<uint32_t, 3> formats{}; formats[i] = 1;
        CHECK(terrainSpecKey(u, false, formats) != key);
    }
    auto v = u;
    for (int lighting : {0,1}) for (int detail : {0,1}) {
        v = u; v.lightingEnabled = lighting; v.detailEnabled = detail;
        auto w = v; w.enableShadows = 0; CHECK(terrainSpecKey(v) != terrainSpecKey(w));
        w = v; w.concreteDetail = 0; CHECK(terrainSpecKey(v) != terrainSpecKey(w));
    }
    for (auto field : fields) for (int value : {-1,4}) {
        if (field == &TerrainFrameUBO::csmDebugMode) continue; // full signed 32-bit domain
        v = u; v.*field = value; CHECK_FALSE(terrainSpecKey(v).valid());
    }
    v = u; v.csmDebugMode = -1; CHECK(terrainSpecKey(v).valid()); CHECK(terrainSpecKey(v) != key);
    std::set<uint64_t> keys;
    for (int color = 0; color < 3; ++color) for (int detail = 0; detail < 3; ++detail)
    for (int concrete : {0,1,3}) for (int visual : {0,2}) {
        v = u; v.colormapReady = color; v.detailReady = detail; v.concreteDetail = concrete; v.visualMode = visual;
        const auto k = terrainSpecKey(v); const auto a = k.values();
        uint32_t bits = 0;
        for (unsigned i = 0; i < a.size(); ++i) bits |= static_cast<uint32_t>(a[i]) << (2*i);
        CHECK(bits == k.bits); keys.insert(k.bits);
    }
    CHECK(keys.size() == 54);
}
static auto retire = [](Pipe&& p) { p.reset(); };
template<class Compile> static Pipe* tick(Table& t, TerrainSpecKey k, Compile compile) {
    return t.select(k, true, compile, retire);
}
template<class Compile> static void stable(Table& t, TerrainSpecKey k, Compile compile) {
    for (int i = 0; i < 30; ++i) tick(t, k, compile);
}
// Poll only on the test thread; production select never waits for a compile.
template<class Compile> static Pipe* finish(Table& t, TerrainSpecKey k, Compile compile) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Pipe* result = nullptr;
    do { std::this_thread::yield(); result = tick(t, k, compile); }
    while (t.compiling() && std::chrono::steady_clock::now() < deadline);
    REQUIRE_FALSE(t.compiling());
    return result;
}
TEST_CASE("terrain_spec/readiness flaps both views including during compile") {
    for (bool reflection : {false,true}) {
        (void)reflection; // independent per-island tables in the two views
        Table t; auto u = shipping(); auto ready = terrainSpecKey(u);
        u.enableShadows = 0; auto unready = terrainSpecKey(u);
        std::atomic<int> calls{0};
        auto compile = [&] { ++calls; return std::make_unique<int>(7); };
        for (int i = 0; i < 29; ++i) CHECK(tick(t, ready, compile) == nullptr);
        CHECK(calls == 0);
        tick(t, ready, compile); REQUIRE(finish(t, ready, compile));
        CHECK(calls == 1);
        CHECK(tick(t, unready, compile) == nullptr);
        REQUIRE(tick(t, ready, compile)); CHECK(calls == 1);
        std::promise<void> release; auto gate = release.get_future().share();
        auto blocked = [&, gate] { ++calls; gate.wait(); return std::make_unique<int>(8); };
        stable(t, unready, blocked); REQUIRE(t.compiling());
        REQUIRE(tick(t, ready, compile));
        release.set_value(); REQUIRE(finish(t, ready, compile));
        REQUIRE(tick(t, unready, compile)); CHECK(calls == 2);
        REQUIRE(tick(t, ready, compile)); CHECK(calls == 2);
    }
}
TEST_CASE("terrain_spec/sticky compile failure and stale generation") {
    const auto k = terrainSpecKey(shipping());
    SUBCASE("failure stays generic") {
        Table t; std::atomic<int> calls{0};
        auto fail = [&]() -> Pipe { ++calls; return {}; };
        stable(t, k, fail); CHECK(finish(t, k, fail) == nullptr); CHECK(t.failed());
        stable(t, k, fail); CHECK(calls == 1);
    }
    SUBCASE("stale completion retired") {
        Table t; std::promise<void> release; auto gate = release.get_future().share();
        unsigned retired = 0;
        auto countRetire = [&](Pipe&& p) { REQUIRE(p); CHECK(*p == 1); ++retired; p.reset(); };
        auto compile = [gate] { gate.wait(); return std::make_unique<int>(1); };
        stable(t, k, compile); t.invalidate(countRetire); CHECK(retired == 0); release.set_value();
        // Disable new launches while draining the stale completion.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do { t.select(k, false, compile, countRetire); std::this_thread::yield(); }
        while (t.compiling() && std::chrono::steady_clock::now() < deadline);
        REQUIRE_FALSE(t.compiling()); CHECK_FALSE(t.failed());
        CHECK(retired == 1);
        CHECK(tick(t, k, compile) == nullptr);
    }
}
TEST_CASE("terrain_spec/teardown drains compiling worker before input destruction") {
    auto input = std::make_shared<int>(42); std::weak_ptr<int> weak = input;
    auto table = std::make_unique<Table>();
    std::promise<void> release; auto gate = release.get_future().share();
    auto compile = [input, gate] { gate.wait(); return std::make_unique<int>(*input); };
    stable(*table, terrainSpecKey(shipping()), compile);
    auto destroy = std::async(std::launch::async, [t = std::move(table)]() mutable { t.reset(); });
    CHECK(destroy.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    release.set_value(); destroy.get(); CHECK(*input == 42); CHECK_FALSE(weak.expired());
}
TEST_CASE("terrain_spec/four entry LRU retires only the least recently used") {
    Table t; int calls = 0, retired = 0;
    auto compile = [&] { return std::make_unique<int>(++calls); };
    auto select = [&](TerrainSpecKey k) { return t.select(k, true, compile, [&](Pipe&& p) { ++retired; p.reset(); }); };
    for (uint32_t i = 1; i <= 4; ++i) { stable(t, {i}, compile); REQUIRE(finish(t, {i}, compile)); }
    REQUIRE(select({1})); CHECK(calls == 4);
    // The fifth completion uses the owner retirement callback.
    for (int i = 0; i < 30; ++i) select({5});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (t.compiling() && std::chrono::steady_clock::now() < deadline) { select({5}); std::this_thread::yield(); }
    REQUIRE_FALSE(t.compiling()); CHECK(retired == 1);
    REQUIRE(select({1})); CHECK(select({2}) == nullptr); CHECK(calls == 5);
    CHECK(t.select({1}, false, compile, retire) == nullptr);
}

TEST_CASE("terrain_spec/invalidation retires cached entries and invalid keys stay generic") {
    Table t; int retired = 0;
    auto compile = [] { return std::make_unique<int>(1); };
    const auto key = terrainSpecKey(shipping());
    stable(t, key, compile); REQUIRE(finish(t, key, compile));
    t.invalidate([&](Pipe&& p) { ++retired; p.reset(); });
    CHECK(retired == 1); CHECK(tick(t, key, compile) == nullptr);
    const TerrainSpecKey invalid{uint64_t{1} << 25};
    stable(t, invalid, compile); CHECK_FALSE(t.compiling()); CHECK(tick(t, invalid, compile) == nullptr);
}
