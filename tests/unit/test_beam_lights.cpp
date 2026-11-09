#include "doctest.h"
#include "../../GameOS/gameos/beam_lights.h"

using namespace mc2::beam_lights;
namespace { bool visible(const float*) { return true; } }

TEST_CASE("beam lights space samples along the segment and keep beam color") {
    Pool pool;
    pool.add({{0, 0, 20}, {220, 0, 20}, 0xff0000, 1, false}, visible);
    REQUIRE(pool.count == 4);
    for (unsigned i = 0; i < pool.count; ++i) {
        CHECK(pool.lights[i].position[0] == doctest::Approx(27.5f + 55.0f * i));
        CHECK(pool.lights[i].position[2] == 20);
        CHECK(pool.lights[i].color[0] == doctest::Approx(0.12));
        CHECK(pool.lights[i].color[1] == 0);
        CHECK(pool.lights[i].color[2] == 0);
    }
}

TEST_CASE("beam lights merge nearby samples, cap a frame, reject offscreen lights") {
    Pool pool;
    const Segment beam{{0, 0, 0}, {220, 0, 0}, 0x00ff00, 1, false};
    pool.add(beam, visible);
    pool.add(beam, visible);
    CHECK(pool.count == 4);
    CHECK(pool.lights[0].color[1] == doctest::Approx(0.12));
    pool.add(beam, [](const float*) { return false; });
    CHECK(pool.count == 4);
    for (unsigned j = 1; j < 100; ++j) {
        auto next = beam;
        next.start[1] = next.end[1] = j * 100.0f;
        pool.add(next, visible);
    }
    CHECK(pool.count == kBudget);
}

TEST_CASE("beam lights fade out with lifetime and impact is short brighter and at endpoint") {
    Pool pool;
    pool.add({{0, 0, 0}, {100, 0, 0}, 0x80c0ff, 0.08f, true}, visible);
    REQUIRE(pool.count == 1);
    CHECK(pool.lights[0].position[0] == 100);
    CHECK(pool.lights[0].color[2] == doctest::Approx(0.15));
    Pool full;
    full.add({{0, 0, 0}, {100, 0, 0}, 0x80c0ff, 0.16f, true}, visible);
    CHECK(full.lights[0].color[2] == doctest::Approx(0.30));
    Pool expired;
    for (float remaining : {0.0f, -0.1f})
        expired.add({{0, 0, 0}, {100, 0, 0}, 0xffffff, remaining, false}, visible);
    CHECK(expired.count == 0);
    Pool faded;
    faded.add({{0, 0, 0}, {100, 0, 0}, 0x0000ff, 0.08f, false}, visible);
    CHECK(faded.lights[0].color[2] == doctest::Approx(0.06));
}
