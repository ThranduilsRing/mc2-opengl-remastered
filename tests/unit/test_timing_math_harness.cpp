#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "timing_math.h"

namespace {
namespace tmath = timing_math;

TEST_CASE("timing_math_harness/ticks_freq_zero_is_identity") {

    CHECK((tmath::ticks_to_ms(0, 0) == 0));
    CHECK((tmath::ticks_to_ms(1234, 0) == 1234));
    return;
}

TEST_CASE("timing_math_harness/ticks_windows_formula") {

    CHECK((tmath::ticks_to_ms(10000000ull, 10000000ull) == 1000));

    CHECK((tmath::ticks_to_ms(5000000ull, 10000000ull) == 500));

    CHECK((tmath::ticks_to_ms(3, 1000000ull) == 0));
    CHECK((tmath::ticks_to_ms(1500, 1000ull) == 1500));
    return;
}

TEST_CASE("timing_math_harness/ticks_no_overflow_large") {

    const uint64_t freq = 10000000ull;
    const uint64_t ticks = 36000ull * freq;
    CHECK((tmath::ticks_to_ms(ticks, freq) == 36000000ull));
    return;
}

TEST_CASE("timing_math_harness/presentation_retains_submillisecond_frames") {
    CHECK(tmath::ticks_to_ms_fractional(3, 1000000) == doctest::Approx(0.003));
    CHECK(tmath::ticks_to_ms_fractional(166667, 10000000) == doctest::Approx(16.6667));
    CHECK(tmath::ticks_to_ms_fractional(17, 0) == 17.0);
    // The camera integrates this per-frame time. Integer ms lost 4% at 240 Hz.
    double elapsedMs = 0.0;
    for (int i = 0; i < 240; ++i)
        elapsedMs += tmath::ticks_to_ms_fractional(12500, 3000000);
    CHECK(elapsedMs == doctest::Approx(1000.0));
}

TEST_CASE("timing_math_harness/clamp_first_call_seeds") {

    CHECK((tmath::advance_elapsed_clamp(-1.0, 42.0, 1.0) == 42.0));
    CHECK((tmath::advance_elapsed_clamp(-1.0, 42.0, 0.0) == 42.0));
    return;
}

TEST_CASE("timing_math_harness/clamp_small_gap_passes_through") {

    CHECK((tmath::advance_elapsed_clamp(10.0, 10.5, 1.0) == 10.5));
    return;
}

TEST_CASE("timing_math_harness/clamp_large_gap_caps_catchup") {

    CHECK((tmath::advance_elapsed_clamp(10.0, 15.0, 1.0) == 11.0));
    return;
}

TEST_CASE("timing_math_harness/clamp_disabled_when_maxdelta_nonpositive") {

    CHECK((tmath::advance_elapsed_clamp(10.0, 999.0, 0.0) == 999.0));
    CHECK((tmath::advance_elapsed_clamp(10.0, 999.0, -1.0) == 999.0));
    return;
}

TEST_CASE("timing_math_harness/clamp_monotonic_never_backward") {

    double prev = -1.0;
    const double maxDelta = 1.0;
    const double samples[] = {100.0, 100.5, 108.0 , 108.2, 120.0};
    double last = -1e9;
    for (double now : samples) {
        prev = tmath::advance_elapsed_clamp(prev, now, maxDelta);
        CHECK((prev >= last));
        CHECK((prev <= now + 1e-9));
        last = prev;
    }

    CHECK((last < 120.0));
    return;
}

TEST_CASE("timing_math_harness/demo_intentional_fail" * doctest::should_fail()) {
    CHECK((tmath::ticks_to_ms(10000000ull, 10000000ull) == 9999));
    return;
}

}
