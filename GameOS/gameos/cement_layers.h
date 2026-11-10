// CEMENT-ARRAY-1: slice the CPU cement catalog atlas (grid x grid cells of tile x tile RGBA8, cell k at
// column k % grid, row k / grid -- the layout the CPU classifiers and the hard-surface field read) into
// layer-major texels for a texture array: layer k is exactly cell k, so a cement word's layer index
// stays the old cell index. Header-only so the doctest tier covers it without the engine.
#ifndef MC2_CEMENT_LAYERS_H
#define MC2_CEMENT_LAYERS_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mc2terrain {

// Returns layers * tile * tile * 4 bytes, or empty when the arguments do not describe the atlas
// (atlasSide must be grid * tile, layers must fit in grid * grid).
inline std::vector<uint8_t> cementAtlasToLayers(const uint8_t* rgba, int atlasSide, int grid, int layers) {
    std::vector<uint8_t> out;
    if (!rgba || grid <= 0 || layers <= 0 || atlasSide <= 0 || atlasSide % grid != 0) return out;
    if (layers > grid * grid) return out;
    const size_t tile = static_cast<size_t>(atlasSide / grid);
    const size_t side = static_cast<size_t>(atlasSide);
    out.resize(static_cast<size_t>(layers) * tile * tile * 4u);
    for (int k = 0; k < layers; ++k) {
        const size_t col = static_cast<size_t>(k % grid), row = static_cast<size_t>(k / grid);
        uint8_t* dst = out.data() + static_cast<size_t>(k) * tile * tile * 4u;
        for (size_t y = 0; y < tile; ++y)
            std::memcpy(dst + y * tile * 4u, rgba + ((row * tile + y) * side + col * tile) * 4u, tile * 4u);
    }
    return out;
}

}  // namespace mc2terrain

#endif
