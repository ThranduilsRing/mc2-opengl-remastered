#pragma once
#include <cstddef>
#include <vector>

namespace mc2mechanim {
// Presentation storage is rebuilt independently of restored clip state.
inline void sizeModelDelta(std::vector<float>& palette, std::size_t jointCount) {
    const auto count = jointCount * 16;
    if (palette.size() != count) palette.resize(count, 0.0f);
}
}
