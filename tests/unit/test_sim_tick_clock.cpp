#include "doctest.h"
#include "../../code/sim_tick_clock.h"
#include "../../code/sim_speed.h"

#include <limits>

using mc2::sim::TickClock;
using mc2::sim::kTickSeconds;

TEST_CASE("sim speed: plus/minus keys step and clamp") {
    using mc2::sim::speedKeyStep;
    for (long key : {long(KEY_ADD), long(KEY_EQUALS), long(0x01000000 | KEY_EQUALS)}) {
        CHECK(speedKeyStep(1.f, key, false) == 2.f);
        CHECK(speedKeyStep(2.f, key, false) == 4.f);
        CHECK(speedKeyStep(4.f, key, false) == 4.f);
        CHECK(speedKeyStep(0.5f, key, false) == 1.f);
        CHECK(speedKeyStep(1.f, key, true) == 1.f);
    }
    for (long key : {long(KEY_MINUS), long(KEY_SUBTRACT)}) {
        CHECK(speedKeyStep(4.f, key, false) == 2.f);
        CHECK(speedKeyStep(2.f, key, false) == 1.f);
        CHECK(speedKeyStep(1.f, key, false) == 0.5f);
        CHECK(speedKeyStep(0.5f, key, false) == 0.5f);
        CHECK(speedKeyStep(1.f, key, true) == 1.f);
    }
    CHECK(speedKeyStep(1.f, KEY_PRIOR | 0x100, false) == 1.f);
    CHECK(speedKeyStep(1.f, KEY_NEXT | 0x100, false) == 1.f);
}

TEST_CASE("sim speed: cycle, clamp and multiplayer disable") {
    using namespace mc2::sim;
    for (float speed : {0.5f, 1.f, 2.f, 4.f}) {
        const float next = speedCycle(speed, false, false);
        CHECK(next == (speed == 4.f ? 0.5f : speed * 2.f));
        CHECK(speedCycle(next, true, false) == speed);
        CHECK(speedCycle(speed, false, true) == speed);
        CHECK(speedCycle(speed, true, true) == speed);
        CHECK(speedSet(speed, 2.f, true) == speed);
        CHECK(speedEffective(speed, true) == 1.f);
        CHECK(speedEffective(speed, false) == speed);
    }
    CHECK(speedCycle(0.25f, false, false) == 0.5f);
    CHECK(speedCycle(1.5f, false, false) == 2.f);
    CHECK(speedCycle(1.5f, true, false) == 1.f);
    CHECK(speedSet(1.f, -1.f, false) == 0.25f);
    CHECK(speedSet(1.f, 0.5f, false) == 0.5f);
    CHECK(speedSet(1.f, 9.f, false) == 4.f);
}

namespace {
// Total ticks run over `seconds` of real time at a constant frame rate.
int run(double fps, double seconds, double speed) {
    TickClock c;
    int total = 0;
    const int frames = static_cast<int>(fps * seconds + 0.5);
    for (int i = 0; i < frames; ++i) total += c.advance(1.0 / fps, speed);
    return total;
}
}

TEST_CASE("tick clock: tick count over ten seconds does not depend on frame rate") {
    for (double fps : {11.0, 12.0, 15.0, 20.0, 30.0, 60.0, 75.0, 144.0, 165.0, 240.0, 1000.0})
        CHECK(run(fps, 10.0, 1.0) == 300);
    CHECK(run(5.0, 10.0, 1.0) == 150);
}

TEST_CASE("tick clock: game speed scales ticks per second, not tick length") {
    CHECK(run(60.0, 10.0, 2.0) == 600);
    CHECK(run(60.0, 10.0, 4.0) == 1200);
    CHECK(run(144.0, 10.0, 0.5) == 150);
    CHECK(run(144.0, 10.0, 0.25) == 75);
    CHECK(run(30.0, 10.0, 4.0) == 1200);   // 4 ticks per frame, none clamped
}

TEST_CASE("tick clock: exact 30 fps is one tick per frame") {
    TickClock c;
    for (int i = 0; i < 100; ++i) CHECK(c.advance(kTickSeconds, 1.0) == 1);
}

TEST_CASE("tick clock: hitches pause play and preserve phase at every speed") {
    TickClock c, phased, fast, slow;
    CHECK(c.advance(1.0, 1.0) == 3);
    int ticks = 0;
    for (int i = 0; i < 60; ++i) ticks += c.advance(1.0 / 60.0, 1.0);
    CHECK(ticks == 30);
    phased.advance(kTickSeconds / 2, 1.0); CHECK(phased.advance(1.0, 1.0) == 3);
    CHECK(phased.alpha() == doctest::Approx(0.5)); CHECK(fast.advance(1.0, 4.0) == 12);
    CHECK(slow.advance(1.0, 0.25) == 0); CHECK(slow.alpha() == doctest::Approx(0.75));
    CHECK(slow.advance(kTickSeconds, 0.25) == 1);
    CHECK(mc2::sim::maxTicksPerFrame(4.0) == 12); CHECK(mc2::sim::maxTicksPerFrame(0.25) == 3);
    CHECK(mc2::sim::maxTicksPerFrame(1.5) == 5);
}

TEST_CASE("tick clock: periodic 200 ms stalls discard excess time") {
    TickClock c;
    int ticks = 0;
    for (int second = 0; second < 10; ++second) {
        const int batch = c.advance(0.2, 1.0); CHECK(batch <= 3); ticks += batch;
        for (int frame = 0; frame < 48; ++frame) {
            const int step = c.advance(1.0 / 60.0, 1.0);
            CHECK(step <= 3); ticks += step;
        }
    }
    CHECK(ticks == 270);
}

TEST_CASE("tick clock: a 20 ms tick yields and bounds the remaining debt") {
    TickClock c;
    const int reserved = c.advance(0.1, 1.0);
    CHECK(reserved == 3);
    CHECK_FALSE(mc2::sim::catchUpSpent(0.007));
    CHECK(mc2::sim::catchUpSpent(0.008));
    CHECK(mc2::sim::catchUpSpent(0.020));
    c.defer(2); CHECK(c.alpha() == 1.0);
    CHECK(c.advance(kTickSeconds, 1.0) == 3);
    c.defer(30);
    CHECK(c.advance(0.1, 1.0) == 3);
    CHECK(c.advance(1.0 / 60.0, 1.0) <= 1);
    c.reset(); CHECK(c.advance(1.0, 4.0) == 12);
    c.defer(11); CHECK(c.advance(1.0 / 60.0, 1.0) <= 3); CHECK(c.advance(1.0 / 60.0, 1.0) <= 1);
}
TEST_CASE("tick clock: random hitch respects batch and interpolation bounds") {
    for (double speed : {0.25, 0.5, 1.0, 1.5, 2.0, 4.0})
        for (int fps : {30, 60, 144, 240}) {
            TickClock c; const int hitch = (0x5eedu * 1664525u + 1013904223u) % (10 * fps);
            for (int frame = 0; frame < 10 * fps; ++frame) {
                CHECK(c.advance(frame == hitch ? 1.0 : 1.0 / fps, speed) <= mc2::sim::maxTicksPerFrame(speed));
                CHECK((c.alpha() >= 0.0 && c.alpha() <= 1.0));
            }
        }
}
TEST_CASE("tick clock: degenerate input runs nothing") {
    TickClock c;
    CHECK(c.advance(0.0, 1.0) == 0);
    CHECK(c.advance(-1.0, 1.0) == 0);
    CHECK(c.advance(0.1, 0.0) == 0);
    CHECK(c.advance(0.1, std::numeric_limits<double>::quiet_NaN()) == 0);
}
