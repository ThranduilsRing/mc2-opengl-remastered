#pragma once
#include "fx_runtime.h"
#include <span>

namespace mc2::fx {
std::span<const glm::vec3> tube_template(uint32_t type);
std::vector<uint16_t> tube_indices(uint32_t type, bool inside_out, size_t profiles);
}
