#pragma once
// FOOT-GROUNDING-1: visual-only grounding of a drawn pose (render side; the sim never calls this).
// A contact follows the VISUAL surface under itself, not the one under the actor root. Callers sample the surfaces and
// add the result to the draw lift; no sim position is read or written.
#include <algorithm>
#include <span>
#include "math/glm_config.h"

namespace mc2foot {
constexpr float kMinReach = 2.0f;

// How far a contact may leave the surface under the root: the surface may rise or fall at 45 degrees across the
// horizontal distance to the contact, and never less than kMinReach. A steeper step is an edge the contact hangs over.
inline float reach(float horizontalDistance) { return std::max(kMinReach, horizontalDistance); }

// Surface offset (visual under the contact minus visual under the root) a contact follows, limited to its reach.
inline float follow(float offset, float reach) { return std::clamp(offset, -reach, reach); }

// Extra lift for a pose resting on several contacts. offset[i]: the followed surface offset under contact i (world
// units); h[i]: that contact's height above the pose's resting point. Only the most demanding contact decides, so a
// body is lowered when its root sits over higher ground than the parts it rests on (lift < 0).
inline float restLift(std::span<const float> offset, std::span<const float> h)
{
    if (offset.empty() || h.empty()) return 0.0f;
    float lift = -1e30f;
    for (size_t i = 0; i < offset.size() && i < h.size(); ++i) lift = std::max(lift, offset[i] - h[i]);
    return lift;
}
} // namespace mc2foot
