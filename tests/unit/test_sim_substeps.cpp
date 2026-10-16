#include "doctest.h"
#include "../../GameOS/gameos/sim_substeps.h"

#include <limits>

using mc2_sim_substeps::stepsPerFrame;

TEST_CASE("sim substeps: only the unfocused harness clock after mission_ready runs ahead") {
    CHECK(stepsPerFrame(4.0f, true, true, false) == 4);
    CHECK(stepsPerFrame(4.0f, false, true, false) == 1);   // wall-clock play: the dt multiplier path owns speed
    CHECK(stepsPerFrame(4.0f, true, false, false) == 1);   // loading and menus stay 1x
    CHECK(stepsPerFrame(4.0f, true, true, true) == 1);     // a focused window never outruns its input
}

TEST_CASE("sim substeps: seed rounds down and clamps") {
    CHECK(stepsPerFrame(1.0f, true, true, false) == 1);
    CHECK(stepsPerFrame(0.25f, true, true, false) == 1);
    CHECK(stepsPerFrame(1.99f, true, true, false) == 1);
    CHECK(stepsPerFrame(2.5f, true, true, false) == 2);
    CHECK(stepsPerFrame(3.0f, true, true, false) == 3);
    CHECK(stepsPerFrame(9.0f, true, true, false) == 4);
    CHECK(stepsPerFrame(std::numeric_limits<float>::quiet_NaN(), true, true, false) == 1);
}
