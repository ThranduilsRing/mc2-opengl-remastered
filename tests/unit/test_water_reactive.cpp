#include "doctest.h"
#include "../../RenderCore/water_reactive.h"
#include "../../shaders/include/water_reactive.hglsl"
#include <cstring>
#include <limits>
#include <memory>

using namespace mc2::waterreactive;

TEST_CASE("WaterReactive/wet visible sources only and hidden history removal") {
    Buffer b;
    b.submit(1, 1, 0, 0, 20, false, true, false);
    b.submit(2, 1, 0, 0, 20, true, false, false);
    CHECK(b.size() == 0);
    b.submit(1, 1, 0, 0, 20, true, true, false);
    CHECK(b.size() == 1);
    b.submit(1, 1.1f, 0, 0, 20, true, false, false);
    CHECK(b.size() == 0);
    b.submit(1, 1.2f, 0, 0, 20, true, true, false);
    b.submit(1, 1.3f, 0, 0, 20, false, true, false);
    CHECK(b.size() == 1); // A dry exit leaves the last ring to fade.
    b.advance(4.21f);
    CHECK(b.size() == 0);
}

TEST_CASE("WaterReactive/capacity drops oldest and tile lists keep newest") {
    Buffer b;
    for (uintptr_t i = 1; i <= kCapacity + 5; ++i)
        b.submit(i, 1, static_cast<float>(i), -1, 20, true, true, false);
    CHECK(b.size() == kCapacity);
    GpuFrame gpu;
    b.snapshot(1.25f, gpu);
    CHECK(gpu.stamps[0].positionRadiusAge[0] == 6);
    CHECK(gpu.stamps[127].positionRadiusAge[0] == 133);
    CHECK(gpu.stamps[0].positionRadiusAge[3] == doctest::Approx(0.25));
    const auto t = tile(0, -1);
    CHECK(gpu.counts[t] == kPerTile);
    CHECK(gpu.indices[t * kPerTile] == kCapacity - kPerTile);
    for (uint32_t i = 0; i < kTiles; ++i) {
        REQUIRE(gpu.counts[i] <= kPerTile);
        for (uint32_t j = 0; j < gpu.counts[i]; ++j)
            CHECK(gpu.indices[i * kPerTile + j] < kCapacity);
    }
    CHECK(gpu.counts[tile(10, 10)] == 0);
}

TEST_CASE("WaterReactive/three standing mechs keep every live ring in every tile they touch (no tile-edge cut)") {
    // Owner 2026-11-10: the outer rings ended in a straight line along a tile edge. Three standing mechs emit a stamp every
    // 0.55 s for 3 s each (~6 per mech); with 8 slots per tile the oldest were dropped per tile, differently per neighbour.
    Buffer b;
    const float ys[3] = {4015.0f, 4050.0f, 4096.0f};
    for (int step = 0; step <= 30; ++step)
        for (int m = 0; m < 3; ++m)
            b.submit(m + 1, 1.0f + step * 0.1f, 256.0f, ys[m], 13, true, true, false);
    GpuFrame gpu;
    b.snapshot(4.0f, gpu);
    const uint32_t live = b.size();
    CHECK(live >= 15);
    for (int ty = 15; ty <= 16; ++ty)
        CHECK(gpu.counts[tile(1, ty)] == live); // every live stamp reaches both tile rows around y=4096
}

TEST_CASE("WaterReactive/cadence velocity reset invalid input and stationary rings") {
    Buffer b;
    b.submit(1, 1, 0, 0, 20, true, true, true);
    for (int i = 1; i <= 4; ++i)
        b.submit(1, 1 + i * 0.1f, i * 5.0f, 0, 20, true, true, true);
    CHECK(b.size() == 2);
    GpuFrame gpu;
    b.snapshot(1.4f, gpu);
    CHECK(gpu.stamps[1].velocityType[0] == doctest::Approx(50));
    b.submit(1, 1.5f, 2000, 0, 20, true, true, true);
    CHECK(b.size() == 2); // Teleport emits no stretched connecting wake.
    b.submit(2, 1.5f, std::numeric_limits<float>::infinity(), 0, 20, true, true, false);
    CHECK(b.size() == 2);
    b.advance(0); // Replay/mission clock reset.
    CHECK(b.size() == 0);
    b.submit(1, 0, 0, 0, 20, true, true, false);
    for (int i = 1; i <= 7; ++i)
        b.submit(1, i * 0.1f, 0, 0, 20, true, true, false);
    CHECK(b.size() == 2);
    b.clear();
    b.snapshot(1, gpu);
    CHECK(b.size() == 0);
    for (const auto n : gpu.counts) CHECK(n == 0);
}

TEST_CASE("WaterReactive/shared shader ring support symmetry age and pixel fade") {
    const auto sample = [](glm::vec2 p, float age, float pixel = 1.0f) {
        return wr_profile(p, glm::vec2(0), 20, age, 0, pixel);
    };
    CHECK(glm::length(sample({500, 0}, 1)) == 0);
    CHECK(glm::length(sample({50, 0}, 3)) == 0);
    CHECK(glm::length(sample({50, 0}, 0)) == 0);
    CHECK(glm::length(sample({50, 0}, 1, 20)) == 0);
    auto p = sample({53, 0}, 1), n = sample({-53, 0}, 1), y = sample({0, 53}, 1);
    CHECK(std::abs(p.x) > 0.001f);
    CHECK(p.x == doctest::Approx(-n.x));
    CHECK(p.x == doctest::Approx(y.y));
    CHECK(p.y == 0);
    CHECK(p.z == doctest::Approx(n.z));
    // Follow the expanding crest: attenuation is age, independent of radius.
    CHECK(glm::length(sample({20 + 2.8f * 28 + 5, 0}, 2.8f)) < glm::length(p) * 0.05f);
    CHECK(std::isfinite(sample({0, 0}, 1).x));
}

TEST_CASE("WaterReactive/shared shader vehicle wake trails velocity and mirrors") {
    const glm::vec2 v(80, 0);
    const auto ring = wr_profile({-60, 35}, v, 20, 0.5f, 0, 1);
    const auto left = wr_profile({-60, 35}, v, 20, 0.5f, 1, 1);
    const auto right = wr_profile({-60, -35}, v, 20, 0.5f, 1, 1);
    CHECK(glm::length(left - ring) > 0.001f);
    CHECK(left.x == doctest::Approx(right.x));
    CHECK(left.y == doctest::Approx(-right.y));
    CHECK(left.z == doctest::Approx(right.z));
    const auto ahead = wr_profile({60, 35}, v, 20, 0.5f, 1, 1);
    const auto aheadRing = wr_profile({60, 35}, v, 20, 0.5f, 0, 1);
    CHECK(glm::length(ahead - aheadRing) == 0);
    const auto reverse = wr_profile({60, 35}, -v, 20, 0.5f, 1, 1);
    CHECK(reverse.x == doctest::Approx(-left.x));
    CHECK(reverse.y == doctest::Approx(left.y));
}

TEST_CASE("WaterReactive/frozen clock keeps stamps at age zero where the profile is invisible, and stats count the chain") {
    // The visual-capture sweep pauses scenarioTime: a stamp emitted at the pose never ages, and age 0 is faded to nothing.
    mc2::waterreactive::Buffer buffer;
    const int key = 1;
    for (int frame = 0; frame < 120; ++frame)
        buffer.submit(reinterpret_cast<uintptr_t>(&key), 10.0f, 256, 4096, 13, true, true, false);
    mc2::waterreactive::GpuFrame frozen;
    buffer.snapshot(10.0f, frozen);
    CHECK(buffer.size() == 1);
    CHECK(frozen.stamps[0].positionRadiusAge[3] == 0);
    CHECK(glm::length(wr_profile({20, 0}, {0, 0}, 13, 0, 0, 1)) == 0);
    buffer.snapshot(10.4f, frozen);
    CHECK(glm::length(wr_profile({13 + 0.4f * 28, 0}, {0, 0}, 13, 0.4f, 0, 1)) > 0.001f);
    const auto st = buffer.takeStats();
    CHECK(st.submits == 120);
    CHECK(st.wet == 120);
    CHECK(st.vis == 120);
    CHECK(st.wetVisible == 120);
    CHECK(st.emitted == 1);
    CHECK(buffer.takeStats().submits == 0);
}

TEST_CASE("WaterReactive/rings are strong enough to read at the play camera, bounded, and fade out") {
    // Owner 2026-11-10: slope 0.035 / crest 0.028 were barely visible. Scan a radial cut through a standing mech's ring.
    const auto peak = [](float age, float pixel) {
        glm::vec2 best(0.0f);
        for (float r = 0.0f; r < 223.0f; r += 0.25f) {
            const auto v = wr_profile({r, 0}, {0, 0}, 13, age, 0, pixel);
            best = glm::max(best, glm::vec2(std::abs(v.x), v.z));
        }
        return best;
    };
    const auto fresh = peak(0.6f, 1.0f);
    CHECK(fresh.x >= 0.15f);   // at least 4x the old 0.035 peak slope
    CHECK(fresh.y >= 0.15f);   // visible foam weight, was 0.028
    CHECK(fresh.x <= kSlopeMax);
    CHECK(fresh.y <= kCrestMax);
    // Physically plausible: older rings are weaker, and the ring is gone at the lifetime.
    CHECK(peak(2.0f, 1.0f).x < fresh.x * 0.6f);
    CHECK(peak(2.99f, 1.0f).x < fresh.x * 0.02f);
    // Wider rings at the RTS zoom limit fade rather than alias.
    CHECK(peak(0.6f, 18.0f).x == 0);
    // Everything stays zero beyond the CPU tile footprint (no seams at tile borders).
    CHECK(glm::length(wr_profile({224.5f, 0}, {0, 0}, 40, 0.6f, 0, 1)) == 0);
}

TEST_CASE("WaterReactive/overlap saturates smoothly (no flat plateaus) and the crest is thin") {
    // Owner 2026-11-10: posterized milky bands and a blob lattice where neighbouring mechs' rings overlap. A hard clamp made
    // plateaus; the combine must stay strictly increasing and below the cap, and the foam crest must be thinner than the wave.
    float prev = -1.0f;
    for (float s = 0.0f; s <= 3.0f; s += 0.05f) {
        const auto c = wr_combine(glm::vec3(s * 0.3f, -s * 0.3f, s * 0.4f));
        CHECK(c.z > prev);
        CHECK(c.z < kCrestMax);
        CHECK(std::abs(c.x) < kSlopeMax);
        prev = c.z;
    }
    CHECK(wr_combine(glm::vec3(0.05f, 0.0f, 0.05f)).x == doctest::Approx(0.05f).epsilon(0.01));
    int crestSamples = 0, slopeSamples = 0;
    for (float r = 0.0f; r < 223.0f; r += 0.25f) {
        const auto v = wr_profile({r, 0}, {0, 0}, 13, 0.6f, 0, 1);
        crestSamples += v.z > 0.5f * 0.55f * 0.5f;
        slopeSamples += std::abs(v.x) > 0.5f * 0.3f * 0.5f;
    }
    CHECK(crestSamples > 0);
    CHECK(crestSamples < slopeSamples);
}

#include "../../shaders/include/water_shallow.hglsl"
TEST_CASE("WaterShallow/opacity follows the water column: wading stays clear, deep water keeps the full veil") {
    using mc2::watershallow::ws_column_veil;
    CHECK(ws_column_veil(0) == doctest::Approx(0.40f));
    CHECK(ws_column_veil(-5) == doctest::Approx(0.40f));       // land side of the field never exceeds the floor
    CHECK(ws_column_veil(3) * 0.87f < 0.55f);                   // a 3 wu wade: legs/floor clearly visible (the flat sheet was 0.87)
    CHECK(ws_column_veil(16) < 0.95f);                           // a 16 wu wade is still not fully opaque
    CHECK(ws_column_veil(30) > 0.95f);                           // deep water is as opaque as before
    CHECK(ws_column_veil(5) < ws_column_veil(10));              // monotonic
    CHECK(ws_column_veil(10) < ws_column_veil(30));
    CHECK(ws_column_veil(1000) <= 1.0f);
}

TEST_CASE("WaterReactive/snapshot into a dirty frame equals a clean one (the ring slot is written in place)") {
    Buffer b;
    for (uintptr_t i = 1; i <= 40; ++i) b.submit(i, 1, static_cast<float>(i) * 3.0f, -1, 20, true, true, false);
    auto clean = std::make_unique<GpuFrame>();
    auto dirty = std::make_unique<GpuFrame>();
    std::memset(dirty.get(), 0xAB, sizeof(GpuFrame));
    b.snapshot(1.25f, *clean);
    b.snapshot(1.25f, *dirty);
    CHECK(std::memcmp(clean.get(), dirty.get(), sizeof(GpuFrame)) == 0);
}
