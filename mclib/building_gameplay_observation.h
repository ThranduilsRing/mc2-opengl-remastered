#pragma once
#include <ostream>

// Serialize the cells produced by terrain and LOS marking, including permanent rubble.
template<class TerrainCells, class RuntimeCells>
void writeBuildingGameplayState(std::ostream& out, const TerrainCells& terrain,
                               RuntimeCells& runtime, int width)
{
    for (size_t i = 0; i < terrain.size(); ++i) {
        const auto& cell = terrain[i];
        if (!cell.passable || cell.gate || cell.specialType || cell.specialID || cell.lineOfSight)
            out << i / width << ',' << i % width << ',' << cell.passable << ',' << cell.gate
                << ',' << static_cast<int>(cell.specialType) << ',' << cell.specialID << ',' << cell.lineOfSight << ';';
    }
    out << "/los=";
    for (size_t i = 0; i < runtime.size(); ++i)
        if (runtime[i].getLocalHeight())
            out << i / width << ',' << i % width << ',' << runtime[i].getLocalHeight() << ';';
    out << "/end";
}
