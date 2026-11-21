// Per-object motion inputs for the mech island (previous-frame bone palettes per actor).
#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>
#include <glm/glm.hpp>
#include "../../RenderCore/FramePackets.h"
#include "mech_motion_history.h"
#include "vulkan_mech_island.h"

extern uint32_t g_mc2RenderFrame;   // mclib/tgl.cpp: render-frame clock, once per presented frame

// Main-view previous-from-current clip matrix (column-major, GL clip), identity when the camera did not move.
void mc2_mech_prev_from_cur_clip(float out[16]) {
    const auto mv = RenderCore::framePackets().copyMainView();
    glm::mat4 cur, prev;
    std::memcpy(&cur, mv.uniforms.worldToClipGL, sizeof(cur));
    std::memcpy(&prev, mv.uniforms.previousWorldToClipGL, sizeof(prev));
    glm::mat4 m(1.0f);
    if (std::memcmp(&cur, &prev, sizeof(cur)) != 0 && std::abs(glm::determinant(cur)) > 0.0f) m = prev * glm::inverse(cur);
    std::memcpy(out, &m, sizeof(m));
}

// Writes last frame's bone palette into tail (boneCount bones, indexed like the current palette) so the VS can skin each vertex
// with both poses: root rotation, root translation and limb animation all fall out of the pose difference. The history rules
// (first-seen, frame gap, bone-count change, 64-unit teleport) live in mech_motion_history.h.
void mc2_mech_stage_object_motion(const Mc2MechFrameFeed& feed, void* tailMem) {
    static MechMotionHistory history;
    history.stage(feed.instances64, feed.instanceCount, static_cast<const float*>(feed.bones64), feed.boneCount, feed.motionKeys,
                  g_mc2RenderFrame, static_cast<float*>(tailMem));
}
