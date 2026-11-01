// Terrain value fields; native TerrainObjectData remains the legacy reader ABI.
#pragma once
#include "terrobj.h"
#include "object_value_codec.h"

namespace mc2::save {
inline void terrainObjectFields(Archive& a, TerrainObjectData& v,
                                std::vector<short>* extendedCells = nullptr) {
    gameObjectFields(a, v);
    a(v.damage, v.vertexNumber, v.blockNumber, v.pitchAngle, v.fallRate, v.powerSupply);
    for (auto& cell : v.cellFootprint) a(cell);
    for (auto& p : v.vectorFootprint) a(p.x, p.y, p.z);
    a(v.numSubAreas0, v.numSubAreas1);
    if (!a.good()) return;
    if (v.numSubAreas0 < 0 || v.numSubAreas0 > MAX_SPECIAL_SUB_AREAS ||
        v.numSubAreas1 < 0 || v.numSubAreas1 > MAX_SPECIAL_SUB_AREAS) {
        a.reject("terrain_subareas"); return;
    }
    for (int i = 0; i < v.numSubAreas0; ++i) a(v.subAreas0[i]);
    for (int i = 0; i < v.numSubAreas1; ++i) a(v.subAreas1[i]);
    a(v.listID, v.numCellsCovered);
    if (!a.good()) return;
    const auto count = v.numCellsCovered * 2;
    // The legacy ABI holds only 40 pairs. Tagged terrain/building records
    // own the complete counted sequence for larger mod footprints.
    if (count > std::size(v.cellsCovered)) {
        if (!extendedCells) { a.reject("terrain_cells"); return; }
        if (a.reading()) {
            if (!a.allocate(count, sizeof(short))) return;
            extendedCells->resize(count);
        }
        if (extendedCells->size() != count) { a.reject("terrain_cells"); return; }
        for (auto& cell : *extendedCells) a(cell);
        if (a.reading() && a.good())
            std::copy_n(extendedCells->begin(), std::size(v.cellsCovered), v.cellsCovered);
    } else {
        if (extendedCells && a.reading()) extendedCells->clear();
        for (int i = 0; i < count; ++i) a(v.cellsCovered[i]);
    }
    if (a.reading()) {
        std::fill(std::begin(v.subAreas0) + v.numSubAreas0, std::end(v.subAreas0), 0);
        std::fill(std::begin(v.subAreas1) + v.numSubAreas1, std::end(v.subAreas1), 0);
        if (count <= std::size(v.cellsCovered))
            std::fill(std::begin(v.cellsCovered) + count, std::end(v.cellsCovered), 0);
    }
}
// Derived: inactive subarea and cell-pair tails -> 0; only the counted entries
// describe occupancy. Runtime arrays are allocated by TerrainObject::Load.
inline constexpr auto terrainTag = fourcc('T', 'E', 'R', 'R');
struct TerrainRecord {
    TerrainObjectData value{};
    std::vector<short> cells;
    void serialize(Archive& a) { terrainObjectFields(a, value, &cells); }
};
}
