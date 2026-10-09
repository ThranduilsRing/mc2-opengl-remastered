// TERRAIN-MATERIAL-SINGLE-SOURCE: mclib/terrain_material.h is the one terrain-type ->
// PBR material table (quad.cpp, terrain.cpp x2, gos_terrain_indirect.cpp and
// gos_terrain_water_stream.cpp all forward to it).
#include "doctest.h"
#include "../../mclib/terrain_material.h"

TEST_CASE("terrain_material: full 0..20 table incl. type 20 = Concrete") {
    // 0=Rock 1=Grass 2=Dirt 3=Concrete, indexed by raw terrainType.
    const unsigned expect[21] = {
        0, 0, 2, 1, 2, 0, 0, 0, 1, 1,   // 0..9
        3, 0, 1, 3, 3, 3, 3, 3, 3, 3,   // 10..19
        3                               // 20 (END_CEMENT_TYPE): water_stream used to say Rock
    };
    for (uint32_t t = 0; t <= 20; ++t)
        CHECK_MESSAGE(mc2terrain::terrainTypeToMaterial(t) == expect[t], "terrainType ", t);
    CHECK(mc2terrain::terrainTypeToMaterial(200) == mc2terrain::kMatRock);
}
