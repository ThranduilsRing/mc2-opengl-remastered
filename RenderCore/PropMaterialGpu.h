#pragma once
#include "MaterialGpu.h"

namespace RenderCore {
// Native prop table: MaterialGpu texture slots are detail-array layers, never
// gos handles. Albedo is selected by the existing packet feed and is unchanged.
struct PropMaterialGpu {
    MaterialGpu material{};
    float uvScale[2] = {1.0f, 1.0f};
    float normalStrength = 1.0f;
    float aoStrength = 1.0f;
    uint32_t materialClass = 0;
    uint32_t reserved[3]{};
};
static_assert(sizeof(PropMaterialGpu) == 64);
static_assert(offsetof(PropMaterialGpu, uvScale) == 32);
static_assert(offsetof(PropMaterialGpu, materialClass) == 48);
}
