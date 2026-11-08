#pragma once
#include <cstddef>
#include <span>

namespace mc2::terrain {
// Layer 2's original alpha is simulation data, even when the GPU uses BC7.
// Displacement TGAs have no consumer and must never gate normal textures.
template<class LoadTga>
bool loadNormalSources(bool cooked, LoadTga&& loadTga) {
    if (cooked) return loadTga(2);
    for (int mat = 0; mat < 9; ++mat)
        if (!loadTga(mat)) return false;
    return true;
}
inline void copyDisplacementAlpha(std::span<const unsigned char> rgba,
                                  std::span<unsigned char> alpha) {
    for (std::size_t i = 0; i < alpha.size(); ++i) alpha[i] = rgba[i * 4 + 3];
}
}
