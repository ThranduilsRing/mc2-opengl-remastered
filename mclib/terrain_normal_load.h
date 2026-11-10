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
// GRASS-GROUND-NORMAL: the cooked grass layer (slot 1, MAT_LAYER_GRASS) prefers the top-down turf normal
// (tools/mc2texcook/cook_grass_turf.py) unless a mod replaces mat1_normal. A turf the array rejects
// (another size or format) falls back to the slot's own sidecar. load(mat, turf) -> optional layer;
// accept(layers) composes the array. Returns whether the array was accepted.
inline constexpr const char* kGrassTurfSource = "data/terrain_layers/grass_turf_normal.tga";  // loads its .ktx2 sidecar
template<class Layer, class Load, class Accept>
bool loadCookedNormals(std::span<Layer> out, bool grassModded, Load load, Accept accept) {
    bool turf = false;
    for (std::size_t mat = 0; mat < out.size(); ++mat) {
        auto layer = (mat == 1 && !grassModded) ? load(1, true) : decltype(load(0, false)){};
        if (layer) turf = true;
        else layer = load(static_cast<int>(mat), false);
        if (!layer) return false;
        out[mat] = std::move(*layer);
    }
    if (accept(std::span<const Layer>(out))) return true;
    if (!turf) return false;
    auto own = load(1, false);
    if (!own) return false;
    out[1] = std::move(*own);
    return accept(std::span<const Layer>(out));
}
inline void copyDisplacementAlpha(std::span<const unsigned char> rgba,
                                  std::span<unsigned char> alpha) {
    for (std::size_t i = 0; i < alpha.size(); ++i) alpha[i] = rgba[i * 4 + 3];
}
}
