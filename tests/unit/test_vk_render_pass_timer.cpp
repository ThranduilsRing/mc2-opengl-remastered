// The tick-to-millisecond step of the VK pass timer: a TIMESTAMP counter keeps only its valid bits, so a pass that
// crosses the counter's wrap must still read as its short forward duration.
#include "doctest.h"

#include "vk_render_pass_timer.h"

using vk_render_pass_timer::TicksToMs;

TEST_CASE("vk pass timer: ticks scale by the device period") {
    CHECK(TicksToMs(1000, 3000, ~0ull, 1.0) == doctest::Approx(0.002));     // 2,000 ticks of 1 ns
    CHECK(TicksToMs(0, 1000000, ~0ull, 40.0) == doctest::Approx(40.0));      // 1e6 ticks of 40 ns = 40 ms
}

TEST_CASE("vk pass timer: an end stamp past the counter wrap reads as the forward distance") {
    const uint64_t mask36 = (1ull << 36) - 1ull;           // a 36-bit counter (timestampValidBits = 36)
    const uint64_t begin = mask36 - 99;                     // 100 ticks before the wrap
    const uint64_t end = 400;                               // 400 ticks after it
    CHECK(TicksToMs(begin, end, mask36, 1.0) == doctest::Approx(500.0 / 1.0e6));
}

TEST_CASE("vk pass timer: bits above the valid range are ignored") {
    const uint64_t mask32 = 0xFFFFFFFFull;
    CHECK(TicksToMs(0xABC00000010ull, 0xDEF00000110ull, mask32, 1.0) == doctest::Approx(256.0 / 1.0e6));
}
