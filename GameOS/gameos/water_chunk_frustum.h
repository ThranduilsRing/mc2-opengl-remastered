#pragma once
// Engine-free clip-plane box test shared by the water visibility walk (gos_terrain_water_stream.cpp). A box is
// "outside" only when all 8 corners lie outside one common clip plane; never "any corner inside" (a large
// triangle can cross the viewport with every vertex outside it).
#include <cmath>
#include <cstdint>
#include <span>

namespace mc2::waterfrustum {

// Bits 0..4: behind the camera, left, right, bottom, top. `mvp` is column-major, row vector x column, as the water pass uploads it.
inline uint32_t outsideMask(const float* m, float x, float y, float z) {
    const float cx = m[0]*x + m[4]*y + m[8]*z + m[12];
    const float cy = m[1]*x + m[5]*y + m[9]*z + m[13];
    const float cw = m[3]*x + m[7]*y + m[11]*z + m[15];
    const float e = std::abs(cw) * 0.0001f + 0.001f;   // conservative at screen edges
    return static_cast<uint32_t>(cw < -e) | (static_cast<uint32_t>(cx < -cw - e) << 1) | (static_cast<uint32_t>(cx > cw + e) << 2)
         | (static_cast<uint32_t>(cy < -cw - e) << 3) | (static_cast<uint32_t>(cy > cw + e) << 4);
}

// True when the XY box [minX,maxX] x [minY,maxY], widened to z = elevation +/- amplitude, is wholly outside one plane.
inline bool boxOutside(const float* m, float minX, float minY, float maxX, float maxY, float elevation, float amplitude) {
    uint32_t outside = 31;
    for (uint32_t i = 0; i < 8; ++i)
        outside &= outsideMask(m, (i & 1) ? maxX : minX, (i & 2) ? maxY : minY, elevation + ((i & 4) ? amplitude : -amplitude));
    return outside != 0;
}

// True at the first chunk (min/maxX, min/maxY, recipes) that has recipes and is not wholly outside. No chunks: false.
template <class Chunks>
bool anyChunkInView(const float* m, const Chunks& chunks, float elevation, float amplitude) {
    for (const auto& c : chunks)
        if (!c.recipes.empty() && !boxOutside(m, c.minX, c.minY, c.maxX, c.maxY, elevation, amplitude)) return true;
    return false;
}

// True when any map vertex is under water (MapData::calcWater bit 0: elevation < [Water] Elevation). A map whose
// water level lies below all its terrain is dry: the original game drew no water on it, so the shallow band up to
// waterElevation (+ShallowDepth) must not admit submerged-sand or visual-wet water either (DarkRain torrin: water
// -1, terrain 0..850, water sheet over 29% of the map).
template <class Vertices>
bool anyWaterVertex(const Vertices& blocks) {
    for (const auto& vertex : blocks)
        if (vertex.water & 1) return true;
    return false;
}

}  // namespace mc2::waterfrustum
