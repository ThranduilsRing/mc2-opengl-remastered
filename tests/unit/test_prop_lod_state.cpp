#include "RenderCore/PropLodState.h"
#include "doctest.h"
#include <cstdint>
#include <random>
#include <unordered_map>

TEST_CASE("prop_lod_state: flat map matches std::unordered_map under insert, erase and grow") {
    PropLodStateMap flat;
    std::unordered_map<uint64_t, uint8_t> ref;
    std::mt19937_64 rng(12345);
    for (int step = 0; step < 200000; ++step) {
        const uint64_t key = ((rng() % 5000) << 8) | (rng() % 4);   // dense low bits like (position hash << 8) ^ typeID
        const int op = static_cast<int>(rng() % 4);
        if (op == 0) { flat.erase(key); ref.erase(key); }
        else { const uint8_t v = static_cast<uint8_t>(rng() % 3); flat[key] = v; ref[key] = v; }
        if (step % 20000 == 0) {
            CHECK(flat.size() == ref.size());
            for (const auto& [k, v] : ref) { const uint8_t* p = flat.find(k); REQUIRE(p); CHECK(*p == v); }
        }
    }
    CHECK(flat.size() == ref.size());
    for (const auto& [k, v] : ref) { const uint8_t* p = flat.find(k); REQUIRE(p); CHECK(*p == v); }
    CHECK(flat.find(0xFFFFFFFFFFFFull) == nullptr);
    flat.clear();
    CHECK(flat.size() == 0);
    CHECK(flat[7] == 0);
}

TEST_CASE("prop_lod_state: hysteresis band keeps an instance on its LOD at a boundary") {
    const float dist[3] = {0.f, 100.f, 300.f};
    uint8_t lod = 0;
    const std::span<const float> d3(dist);
    propLodAdvance(lod, 105.f, d3, 0.08f); CHECK(lod == 0);   // inside the +8% band
    propLodAdvance(lod, 109.f, d3, 0.08f); CHECK(lod == 1);
    propLodAdvance(lod, 95.f, d3, 0.08f);  CHECK(lod == 1);   // inside the -8% band
    propLodAdvance(lod, 91.f, d3, 0.08f);  CHECK(lod == 0);
    propLodAdvance(lod, 1000.f, d3, 0.08f); CHECK(lod == 2);
}
