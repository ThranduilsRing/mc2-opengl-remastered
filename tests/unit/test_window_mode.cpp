#include "doctest.h"
#include "window_mode.h"

// Trigger paths: window_mode.h, gameos_graphics_capi.cpp, gos_render.cpp.
TEST_CASE("window_mode: briefing canvas and focus cycle retain fullscreen") {
    bool fullscreen = graphics::requested_fullscreen(false, true, false);
    REQUIRE(fullscreen);
    for (int transition = 0; transition < 4; ++transition) {
        // prefs/briefing/results use false,false when already fullscreen.
        fullscreen = graphics::requested_fullscreen(fullscreen, false, false);
        CHECK(fullscreen);
    }
    fullscreen = graphics::requested_fullscreen(fullscreen, false, true);
    CHECK_FALSE(fullscreen);
    CHECK_FALSE(graphics::requested_fullscreen(fullscreen, false, false));
    CHECK(graphics::requested_fullscreen(fullscreen, true, false));
}
