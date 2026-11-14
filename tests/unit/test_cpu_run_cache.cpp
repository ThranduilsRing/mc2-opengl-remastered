#include "doctest.h"
#include "../../GameOS/gameos/light_pool_culling.h"
#include "../../GameOS/gameos/mech_run_cache.h"
#include <array>

TEST_CASE("cpu_run_cache: grid invalidates every packed light field and lifecycle") {
    using namespace gos_terrain_lighting;
    std::array<GpuTerrainLight, 2> lights{};
    LightGridInputs cache;
    CHECK(cache.update(lights));
    for (int tileSet = 0; tileSet < 1000; ++tileSet) CHECK_FALSE(cache.update(lights));
    lights[0].position[0] = 4; // light moved; camera tile changes alone above are irrelevant
    CHECK(cache.update(lights));
    lights[0].lightType = 2;
    CHECK(cache.update(lights));
    lights[0].farDistance = 10;
    CHECK(cache.update(lights));
    lights[0].color[1] = 1;
    CHECK(cache.update(lights));
    lights[0].intensity = 2;
    CHECK(cache.update(lights));
    CHECK(cache.update(std::span<const GpuTerrainLight>(lights).first(1))); // removal
    CHECK(cache.update(lights)); // addition
    cache = {};
    CHECK(cache.update(lights)); // same bytes after mission reload
    CHECK(cache.update({}));
    CHECK_FALSE(cache.update({}));
}

TEST_CASE("cpu_run_cache: mech runs retain live texture and packet invalidation") {
    struct Call {
        uint32_t packet, texHandle, aoTexHandle, normalTexHandle, base, count;
        bool operator==(const Call&) const = default;
    };
    std::array<Call, 2> calls{{{7, 1, 2, 3, 0, 1}, {8, 1, 2, 3, 1, 1}}};
    MechRunCache<Call> cache;
    uint64_t texture = 10;
    uint32_t lookups = 0;
    const auto resolve = [&](uint32_t slot) { ++lookups; return texture + slot; };
    const auto update = [&] { return cache.update(std::span<const Call>(calls), 1, resolve); };
    CHECK(update());
    lookups = 0;
    CHECK_FALSE(update());
    CHECK(lookups == 3); // one live lookup per unique texture, rather than per packet
    for (int pose = 0; pose < 1000; ++pose) CHECK_FALSE(update()); // bones are a separate feed
    ++texture;
    CHECK(update()); // live paint / AO / normal replacement, same slot
    ++calls[0].packet;
    CHECK(update()); // pose/LOD selects different packets
    ++calls[0].count;
    CHECK(update()); // mech added
    --calls[0].count;
    CHECK(update()); // mech destroyed
    CHECK(cache.update(std::span<const Call>(calls).first(1), 1, resolve));
    CHECK(update());
    CHECK(cache.update(std::span<const Call>(calls), 2, resolve)); // geometry / map reload
    calls = {{{7, 9, 3, 9, 0, 1}, {8, 1, 0, UINT32_MAX, 1, 1}}};
    lookups = 0;
    CHECK(update());
    CHECK(lookups == 3); // unordered slots, duplicates and optional sentinels
    CHECK(cache.texture(1) == texture + 1);
    CHECK(cache.texture(3) == texture + 3);
    CHECK(cache.texture(9) == texture + 9);
    std::swap(calls[0], calls[1]);
    CHECK(update()); // packet order matters, texture insertion order does not
    CHECK_FALSE(update());
    CHECK(cache.update({}, 1, resolve)); // remove every texture
    lookups = 0;
    CHECK_FALSE(cache.update({}, 1, resolve));
    CHECK(lookups == 0);
    CHECK(update()); // repopulate retained storage
}
