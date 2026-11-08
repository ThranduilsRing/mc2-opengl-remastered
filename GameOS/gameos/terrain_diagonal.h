// terrain_diagonal.h -- the one rule that picks a terrain quad's triangle diagonal. Raster (vulkan_terrain_island_island.cpp patch meshes) and the RT
// terrain tiles (vk_rt_scene_math.cpp) both call it on the raw visual-bake heights of the quad's corners, so a ray and the raster hit the same triangle
// (a saddle quad split the other way differs by up to half the corner spread). Without a bake both keep the TL-BR diagonal. Picking stays on the coarse
// CPU path (TerrainQuad / the 128-unit grid), which does not see the bake. Pure, header-only; doctest in tests/unit/test_vk_rt_scene.cpp.
#ifndef MC2_TERRAIN_DIAGONAL_H
#define MC2_TERRAIN_DIAGONAL_H

#include <cmath>

namespace mc2terrain {

// Corners: TL = (x, y), TR = (x+1, y), BL = (x, y+1), BR = (x+1, y+1) in grid order (y grows south). True = split along TR-BL (the flatter diagonal),
// false = TL-BR (also the tie).
inline bool quadSplitsTrBl(float hTL, float hTR, float hBL, float hBR) { return std::fabs(hTR - hBL) < std::fabs(hTL - hBR); }

}  // namespace mc2terrain

#endif  // MC2_TERRAIN_DIAGONAL_H
