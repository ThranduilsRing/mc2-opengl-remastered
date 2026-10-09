// terrain_material.h -- SINGLE source of truth for MC2 terrain-type (0..20) ->
// PBR material index (0=Rock, 1=Grass, 2=Dirt, 3=Concrete).
// Previously copied in quad.cpp, terrain.cpp (x2), gos_terrain_indirect.cpp and
// gos_terrain_water_stream.cpp; the water copy lacked type 20 (END_CEMENT_TYPE,
// terrtxm.h:44) so it resolved Rock where every other consumer resolved Concrete.
#pragma once
#include <cstdint>

namespace mc2terrain {

inline constexpr uint8_t kMatRock = 0;
inline constexpr uint8_t kMatGrass = 1;
inline constexpr uint8_t kMatDirt = 2;
inline constexpr uint8_t kMatConcrete = 3;

constexpr uint8_t terrainTypeToMaterial(uint32_t terrainType) noexcept {
    switch (terrainType) {
        case 3:  // Moss
        case 8:  // ForestFloor
        case 9:  // Grass
        case 12: // Slimy
            return kMatGrass;
        case 2:  // Mud
        case 4:  // Dirt
            return kMatDirt;
        case 10: // Concrete
        case 13: case 14: case 15: case 16: // Cement 2-5
        case 17: case 18: case 19:          // Cement 6-8
        case 20:                            // END_CEMENT_TYPE
            return kMatConcrete;
        default:
            return kMatRock; // Mountain, Cliff, Ash, Tundra, Water, None, ...
    }
}

static_assert(terrainTypeToMaterial(20) == kMatConcrete, "type 20 is Concrete everywhere");
static_assert(terrainTypeToMaterial(10) == kMatConcrete && terrainTypeToMaterial(19) == kMatConcrete, "");
static_assert(terrainTypeToMaterial(9) == kMatGrass && terrainTypeToMaterial(4) == kMatDirt, "");
static_assert(terrainTypeToMaterial(0) == kMatRock && terrainTypeToMaterial(21) == kMatRock, "");

} // namespace mc2terrain
