#pragma once
// F6 T3: shared terrain-admission mode flag consumed by terrain.cpp (slimReduce
// loop) and quad.cpp (TerrainQuad::setupTextures water-corner sites).
// MC2_TERRAIN_ADMISSION_LEGACY=1 -> Legacy (projectZ fallback; red-band reference).
// Default: Modern (frustum-plane test via Camera::quadAabbInFrustum).
// Lambda-static idempotent latch.
//
// User-validated mc2_10 60s 2026-05-22 with Modern: no red-band, no false-
// rejects, quadSetupTextures Tracy bucket 1.75ms -> 1.65ms.
#include "Gates/gate.h"

namespace mc2_terrain_admission {
inline bool isModern() {
    static const bool s = !mc2::gate::get<"MC2_TERRAIN_ADMISSION_LEGACY">();
    return s;
}
} // namespace mc2_terrain_admission
