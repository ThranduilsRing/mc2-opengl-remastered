// Trigger: code/screen_layer_order.h (logistics screen layer order).
#include "doctest.h"

#include "screen_layer_order.h"

using mc2::ui::predecessorMayDrawBehind;

TEST_CASE("screen layer order: an ImGui predecessor never draws behind a legacy screen") {
    CHECK_FALSE(predecessorMayDrawBehind(/*current*/ false, /*other*/ true));  // pilot ready over the loading doors
    CHECK(predecessorMayDrawBehind(false, false));
    CHECK(predecessorMayDrawBehind(true, true));
    CHECK(predecessorMayDrawBehind(true, false));
}
