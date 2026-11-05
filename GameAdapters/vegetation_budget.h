#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../mclib/math/glm_config.h"
#include "../3rdparty/glm/glm/packing.hpp"
#include "../RenderCore/GpuResidencyPolicy.h"

namespace mc2::vegetation {
// XY retain full map precision; height/scale and seed use half floats. Yaw is
// an 8-bit turn, and the atlas/role/weed byte is retained verbatim.
struct PackedInstance {
    float x, y;
    uint32_t heightScale, appearance;
};
static_assert(sizeof(PackedInstance) == 16);
inline PackedInstance packInstance(float x, float y, float z, float yaw, float scale, uint32_t atlas, float seed) {
    const uint32_t turn = static_cast<uint32_t>(std::lround(yaw * (256.0f / 6.28318530718f))) & 255u;
    const uint32_t seedHalf = glm::packHalf2x16(glm::vec2(seed, 0)) & 65535u;
    return {x, y, glm::packHalf2x16(glm::vec2(z, scale)), seedHalf | (turn << 16u) | ((atlas & 255u) << 24u)};
}
constexpr uint32_t kVisibleCapacity = RenderCore::vegetationVisibleCapacity;   // one model: RenderCore/GpuResidencyPolicy.h
constexpr uint64_t kResidentBudget = RenderCore::vegetationResidentCeiling;
// Regions remain 32 B; even a one-instance region must fit the device range.
inline uint32_t storagePopulationCapacity(uint32_t requested, uint64_t maxStorageRange) {
    return static_cast<uint32_t>(std::min(static_cast<uint64_t>(requested), maxStorageRange / 32u));
}

// Reserve the cap across the whole map before visiting cells. Stopping a dense
// north-to-south scan at the cap can leave the camera's entire region empty.
inline float placementStep(float mapSpan, float authoredStep, int density, int capacity) {
    const int maxClump = std::max(6, density / 2);
    const int side = (std::max)(1, static_cast<int>(std::sqrt(static_cast<double>((std::max)(capacity, 1)) / maxClump)));
    return (std::max)(authoredStep, mapSpan / static_cast<float>(side + 1));
}
}
