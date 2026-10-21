#pragma once
#include "static_shadow_spec.h"
#include <array>
#include <bit>
#include <cstdint>

namespace mc2terrain {
// Caster destruction changes generation; sun edits change the matrix; craters change heightVersion.
// Bitwise parameter identity remains stable even for nonfinite input.
struct StaticVisibilityDependencies {
    uint64_t generation = 0, heightVersion = 0;
    int size = 0;
    std::array<uint32_t,18> projection{};
    bool operator==(const StaticVisibilityDependencies&) const = default;
};
inline StaticVisibilityDependencies staticVisibilityDependencies(const StaticShadowSpec& spec, uint64_t heightVersion) {
    StaticVisibilityDependencies key;
    key.generation = spec.generation; key.heightVersion = heightVersion; key.size = spec.size;
    for (size_t i=0; i<16; ++i) key.projection[i] = std::bit_cast<uint32_t>(spec.lightSpace[i]);
    key.projection[16] = std::bit_cast<uint32_t>(spec.biasFactor);
    key.projection[17] = std::bit_cast<uint32_t>(spec.biasUnits);
    return key;
}
} // namespace mc2terrain
