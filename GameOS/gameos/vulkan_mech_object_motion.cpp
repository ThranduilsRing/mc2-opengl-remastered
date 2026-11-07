// Per-object motion inputs for the mech island (docs/plans/2026-11-06-temporal-aa.md, Slice D state).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>
#include <glm/glm.hpp>
#include "../../RenderCore/FramePackets.h"
#include "vulkan_mech_island.h"

namespace {
struct PrevRoot { float p[3]; uint32_t frame; };
}

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

// Writes the per-instance render-space root translation since the previous frame into tail (vec4 per instance).
// The root is the actor's first bone (row3.xyz, Stuff space); an actor's instances share one baseBoneOffset.
// First-seen actors and jumps (teleport) get zero, which leaves the camera vector in place.
void mc2_mech_stage_object_motion(const Mc2MechFrameFeed& feed, void* tailMem) {
    float* const tail = static_cast<float*>(tailMem);
    static std::unordered_map<uint32_t, PrevRoot> prevRoots;
    static uint32_t frame = 0;
    ++frame;
    const auto* inst = static_cast<const uint8_t*>(feed.instances64);
    const auto* bones = static_cast<const float*>(feed.bones64);
    std::memset(tail, 0, static_cast<size_t>((feed.instanceCount + 3u) / 4u) * 64u);
    std::vector<std::pair<uint32_t, PrevRoot>> updates;
    for (uint32_t i = 0; i < feed.instanceCount; ++i) {
        uint32_t baseBone = 0, id = 0;
        std::memcpy(&baseBone, inst + i * 64u + 4u, 4);
        std::memcpy(&id, inst + i * 64u + 48u, 4);
        if (!id || !bones || static_cast<uint64_t>(baseBone) >= feed.boneCount) continue;
        const float* root = bones + static_cast<size_t>(baseBone) * 16u + 12u;
        const auto it = prevRoots.find(id);
        if (it != prevRoots.end() && it->second.frame + 1u == frame) {
            const float dx = root[0] - it->second.p[0], dy = root[1] - it->second.p[1], dz = root[2] - it->second.p[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < 64.0f * 64.0f) {   // mc2ToRender(delta) = (-x, z, y)
                float* t = tail + static_cast<size_t>(i) * 4u;
                t[0] = -dx; t[1] = dz; t[2] = dy;
            }
        }
        updates.emplace_back(id, PrevRoot{{root[0], root[1], root[2]}, frame});
    }
    for (const auto& [id, rootState] : updates) prevRoots[id] = rootState;
    if ((frame & 63u) == 0u)
        for (auto it = prevRoots.begin(); it != prevRoots.end();) it = (it->second.frame + 4u < frame) ? prevRoots.erase(it) : std::next(it);
}
