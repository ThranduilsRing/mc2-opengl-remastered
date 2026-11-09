#pragma once
#include <array>
#include <cstdint>

namespace mc2veg {
// Keep the fragment set at four storage buffers, Vulkan's minimum floor.
// Water does not read cement words: it needs the visual bake in that slot.
constexpr std::array<uint32_t, 9> groundLightingBindings(bool waterBake) {
    return {0, 2, 3, waterBake ? 10u : 4u, 11, 12, 13, 16, 19};
}
}
