// render_snap.h - UNIT-VISUAL-SNAP-2: render-only lift of unit effect anchors and the selection box.
// The caller passes TerrainRuntime::visualHeightDelta(position) (0 when MC2_UNIT_VISUAL_SNAP is off or no bake).
// Inputs are copies of node/box coordinates; the sim position is never touched. The draw transform carries the lift, so the
// unit node getters unlift what they read from it (unliftNodeZ) and stay sim-true.
#pragma once
#include <span>
#include "math/glm_config.h"

namespace mc2snap {
// MC2 space: z is up. Returns the lifted copy of a node position (effect anchor).
inline glm::vec3 liftAnchorZ(glm::vec3 p, float delta) { p.z += delta; return p; }
// Inverse for node getters that read the lifted draw transform, so every getter result is sim-true in every pass. The zero
// vector is "node not found" (getters then fall back to position) and passes through unchanged.
inline glm::vec3 unliftNodeZ(glm::vec3 p, float delta)
{
	if (p.x == 0.0f && p.y == 0.0f && p.z == 0.0f) return p;
	p.z -= delta;
	return p;
}
// MC2 space: z is up (selection box corners).
inline void liftBoxZ(std::span<glm::vec3> box, float delta) { for (auto& c : box) c.z += delta; }
}
