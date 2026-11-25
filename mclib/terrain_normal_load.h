#pragma once
#include <cstddef>
#include <span>
#include <utility>

namespace mc2::terrain {
// Layer 2's original alpha is simulation data, even when the GPU uses BC7.
// Displacement TGAs have no consumer and must never gate normal textures.
template<class LoadTga>
bool loadNormalSources(bool cooked, LoadTga loadTga) {
    if (cooked) return loadTga(2);
    for (int mat = 0; mat < 9; ++mat)
        if (!loadTga(mat)) return false;
    return true;
}
// GRASS-GROUND-NORMAL / TERRAIN-NORMAL-UNIFORM: the cooked grass layer (slot 1, MAT_LAYER_GRASS) prefers the top-down turf normal
// (tools/mc2texcook/cook_grass_turf.py) and the dirt layer (slot 2, MAT_LAYER_DIRT) prefers the sand/gravel normal
// (tools/mc2texcook/cook_dirt_sand.py, the stock dirt normal is nearly flat), each unless a mod replaces that slot's matN_normal.
// A replacement the array rejects (another size or format) falls back to the slots' own sidecars. load(mat, alt) -> optional
// layer; accept(layers) composes the array. Returns whether the array was accepted.
inline constexpr const char* kGrassTurfSource = "data/terrain_layers/grass_turf_normal.tga";  // loads its .ktx2 sidecar
inline constexpr const char* kDirtSandSource = "data/terrain_layers/dirt_sand_normal.tga";    // loads its .ktx2 sidecar
// Layer 6 (MAT_LAYER_FOREST) is the optional under-forest ground (tools/mc2texcook/cook_forest_floor.py): `out` may hold a 7th slot,
// loaded through load(6, true) only; when absent or rejected by the array the array keeps its six layers (the shader checks the depth).
inline constexpr const char* kForestFloorSource = "data/terrain_layers/forest_floor_normal.tga";  // loads its .ktx2 sidecar
inline constexpr std::size_t kRequiredNormalLayers = 6;
template<class Layer, class Load, class Accept>
bool loadCookedNormals(std::span<Layer> out, bool grassModded, bool dirtModded, Load load, Accept accept) {
    bool alt = false;
    const std::size_t required = out.size() < kRequiredNormalLayers ? out.size() : kRequiredNormalLayers;
    for (std::size_t mat = 0; mat < required; ++mat) {
        const bool want = (mat == 1 && !grassModded) || (mat == 2 && !dirtModded);
        auto layer = want ? load(static_cast<int>(mat), true) : decltype(load(0, false)){};
        if (layer) alt = true;
        else layer = load(static_cast<int>(mat), false);
        if (!layer) return false;
        out[mat] = std::move(*layer);
    }
    bool forest = false;
    if (out.size() > kRequiredNormalLayers) {
        auto layer = load(static_cast<int>(kRequiredNormalLayers), true);
        if (layer) { out[kRequiredNormalLayers] = std::move(*layer); forest = true; }
    }
    if (forest && accept(std::span<const Layer>(out.data(), kRequiredNormalLayers + 1))) return true;
    if (accept(std::span<const Layer>(out.data(), required))) return true;
    if (!alt) return false;
    for (std::size_t mat = 1; mat <= 2 && mat < out.size(); ++mat) {
        auto own = load(static_cast<int>(mat), false);
        if (!own) return false;
        out[mat] = std::move(*own);
    }
    return accept(std::span<const Layer>(out.data(), required));
}
inline void copyDisplacementAlpha(std::span<const unsigned char> rgba,
                                  std::span<unsigned char> alpha) {
    for (std::size_t i = 0; i < alpha.size(); ++i) alpha[i] = rgba[i * 4 + 3];
}
}
