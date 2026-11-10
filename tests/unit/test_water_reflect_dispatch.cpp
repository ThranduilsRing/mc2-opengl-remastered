// tests/unit/test_water_reflect_dispatch.cpp
// WATER-REFLECT-TIER: the reflection pass's 2-D dispatch math (GameOS/gameos/water_reflect_dispatch.h). The old 1-D dispatch turned
// reflection off above maxComputeWorkGroupCount[0] tiles (65535 on a spec-minimum device = ~4.2 MP, i.e. 4K).
#include "doctest.h"
#include "water_reflect_dispatch.h"

using namespace mc2vk;

namespace {
constexpr uint32_t kSpecMinGroups = 65535u;   // Vulkan spec minimum maxComputeWorkGroupCount[i]
constexpr uint32_t kMaxTiles = 1u << 20;      // NativeProducers::kMaxTiles
constexpr uint32_t tilesFor(uint32_t w, uint32_t h) { return ((w + 7) / 8) * ((h + 7) / 8); }
}

TEST_CASE("water reflect dispatch: empty and small counts stay one row")
{
    CHECK(reflectionDispatchDims(0).x == 0);
    CHECK(reflectionDispatchDims(0).y == 0);
    CHECK(reflectionDispatchDims(1).x == 1);
    CHECK(reflectionDispatchDims(1).y == 1);
    CHECK(reflectionDispatchDims(kReflectRowTiles).x == kReflectRowTiles);
    CHECK(reflectionDispatchDims(kReflectRowTiles).y == 1);
    CHECK(reflectionDispatchDims(kReflectRowTiles + 1).y == 2);
}

TEST_CASE("water reflect dispatch: grid always covers the count with < one row of padding")
{
    for (uint32_t n : {1u, 7u, 1023u, 1024u, 1025u, 65535u, 65536u, 518400u, kMaxTiles}) {
        const ReflectDispatch d = reflectionDispatchDims(n);
        CHECK(static_cast<uint64_t>(d.x) * d.y >= n);
        CHECK(reflectionPadTiles(n) < kReflectRowTiles);
        CHECK(static_cast<uint64_t>(d.x) * d.y == n + reflectionPadTiles(n));
    }
}

TEST_CASE("water reflect dispatch: a spec-minimum device (65535) keeps reflection on at 1080p, 4K and 8K")
{
    const uint32_t cap = reflectionMaxTiles(kMaxTiles, kSpecMinGroups);
    CHECK(cap == kMaxTiles);   // 65535 rows x 1024 = 67M >> the tile-list capacity
    struct Res { uint32_t w, h; } const res[] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {5120, 2160}, {7680, 4320}};
    for (const Res& r : res) {
        const uint32_t n = tilesFor(r.w, r.h);
        const ReflectDispatch d = reflectionDispatchDims(n);
        CAPTURE(r.w); CAPTURE(r.h);
        CHECK(n <= cap);                       // the per-frame tile check no longer turns reflection off
        CHECK(d.x <= kSpecMinGroups);          // both dispatch axes within the spec minimum
        CHECK(d.y <= kSpecMinGroups);
    }
    CHECK(tilesFor(7680, 4320) == 518400u);
    CHECK(reflectionDispatchDims(518400u).x == 1024u);
    CHECK(reflectionDispatchDims(518400u).y == 507u);
}

TEST_CASE("water reflect dispatch: the retired 1-D limit would have refused 4K (regression pin)")
{
    // Old rule: tiles <= maxComputeWorkGroupCount[0] = 65535 -> 4K (129600 tiles) was off.
    CHECK(tilesFor(3840, 2160) > kSpecMinGroups);
    CHECK(tilesFor(3840, 2160) <= reflectionMaxTiles(kMaxTiles, kSpecMinGroups));
}

TEST_CASE("water reflect dispatch: a device with few Y groups caps the tile count, never the row width")
{
    CHECK(reflectionMaxTiles(kMaxTiles, 100u) == 100u * kReflectRowTiles);
    CHECK(reflectionMaxTiles(kMaxTiles, 1u << 30) == kMaxTiles);
}
