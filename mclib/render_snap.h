// render_snap.h - UNIT-VISUAL-SNAP-2: render-only lift of unit effect anchors and the selection box.
// The caller passes TerrainRuntime::visualHeightDelta(position) (0 when MC2_UNIT_VISUAL_SNAP is off or no bake).
// Inputs are copies of node/box coordinates; the sim position and the node getters are never touched.
#pragma once
#include <span>
#include "math/glm_config.h"

namespace mc2snap {
// MC2 space: z is up. Returns the lifted copy of a node position (effect anchor).
inline glm::vec3 liftAnchorZ(glm::vec3 p, float delta) { p.z += delta; return p; }
// MC2 space: z is up (selection box corners).
inline void liftBoxZ(std::span<glm::vec3> box, float delta) { for (auto& c : box) c.z += delta; }
}
