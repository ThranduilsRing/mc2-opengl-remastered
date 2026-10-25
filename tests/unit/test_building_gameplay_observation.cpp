#include "doctest.h"
#include "../../mclib/building_gameplay_observation.h"
#include <array>
#include <sstream>

TEST_CASE("building_gameplay_observation/gate wall rubble and wrong map write")
{
    struct TerrainCell { bool passable = true, gate = false; int specialType = 0, specialID = 0, lineOfSight = 0; };
    struct Cell { int height = 0; int getLocalHeight() const { return height; } };
    std::array<TerrainCell, 2> terrain{};
    std::array<Cell, 2> runtime{};
    auto observe = [&] {
        std::ostringstream out;
        writeBuildingGameplayState(out, terrain, runtime, 2);
        return out.str();
    };
    for (int type : {1, 2}) { // Gate/wall intact cell beside permanent destroyed rubble.
        terrain[0] = {true, type == 1, type, 7, 4};
        terrain[1] = {false, false, 0, 0, 2};
        runtime[1].height = 2;
        const auto correct = observe();
        CHECK(correct.find("0,1,0,0,0,0,2;") != std::string::npos);
        terrain[1].passable = true; // Deliberately wrong permanent-rubble map write.
        CHECK(observe() != correct);
        terrain[1].passable = false;
        runtime[1].height = 3; // Deliberately wrong actual LOS write.
        CHECK(observe() != correct);
    }
}
