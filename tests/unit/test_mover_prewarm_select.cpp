#include "doctest.h"
#include "RenderCore/MoverPrewarmSelect.h"

using namespace RenderCore;

TEST_CASE("mover prewarm: pass 2 takes only non-home movers near a home mover") {
    const PrewarmMover movers[] = {
        {0, 0, true}, {100, 0, true},        // home force
        {2000, 0, false},                    // near the force: drawn in frame 1
        {2600, 0, false},                    // within radius of the second home mover only (not the first)
        {9000, 9000, false},                 // far enemy: lazy
        {-2559, 0, false},                   // just inside
        {-2561, 0, false},                   // just outside
    };
    const auto picked = prewarmNearHome(movers);
    CHECK(picked == std::vector<size_t>{2, 3, 5});
}

TEST_CASE("mover prewarm: no home mover selects nothing") {
    const PrewarmMover movers[] = {{0, 0, false}, {1, 1, false}};
    CHECK(prewarmNearHome(movers).empty());
}
