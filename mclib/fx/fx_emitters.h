#pragma once
#include "fx_runtime.h"

namespace mc2::fx {
std::array<glm::vec2, 4> card_uvs(const EffectRecord& record, float age, float seed,
    std::string_view index_curve);
}
