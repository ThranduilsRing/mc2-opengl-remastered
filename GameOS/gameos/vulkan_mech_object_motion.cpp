// Per-object motion inputs for the mech island (docs/plans/2026-11-06-temporal-aa.md, Slice D state).
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
#include "vulkan_mech_island.h"

namespace {
// One actor's bone palette (16 floats per bone) as of the last frame it was staged.
struct PrevPalette { std::vector<float> bones; uint32_t frame = 0; };
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

// Writes last frame's bone palette into tail (boneCount bones, indexed like the current palette) so the VS can skin each vertex
// with both poses: root rotation, root translation and limb animation all fall out of the pose difference.
// An actor is a run of bones sharing one baseBoneOffset (key = feed.motionKeys). First-seen actors, actors whose bone count changed
// and jumps over 64 units (teleport) get their current pose, which leaves the camera vector in place.
void mc2_mech_stage_object_motion(const Mc2MechFrameFeed& feed, void* tailMem) {
    static std::unordered_map<uint32_t, PrevPalette> prevPalettes;
    static uint32_t frame = 0;
    ++frame;
    const auto* inst = static_cast<const uint8_t*>(feed.instances64);
    const auto* bones = static_cast<const float*>(feed.bones64);
    if (!bones || !feed.boneCount) return;
    float* const tail = static_cast<float*>(tailMem);
    std::memcpy(tail, bones, static_cast<size_t>(feed.boneCount) * 64u);   // default: previous pose == current pose
    // Actor spans: sorted distinct base offsets, each running to the next base (the last to the end of the palette).
    struct ActorSpan { uint32_t base, key; auto operator<=>(const ActorSpan&) const = default; };
    std::vector<ActorSpan> actors;
    if (feed.motionKeys) {
        for (uint32_t i = 0; i < feed.instanceCount; ++i) {
            uint32_t baseBone = 0;
            std::memcpy(&baseBone, inst + i * 64u + 4u, 4);
            if (feed.motionKeys[i] && baseBone < feed.boneCount) actors.push_back({baseBone, feed.motionKeys[i]});
        }
    }
    std::sort(actors.begin(), actors.end());
    actors.erase(std::unique(actors.begin(), actors.end(), [](const ActorSpan& a, const ActorSpan& b) { return a.base == b.base; }), actors.end());
    std::vector<std::pair<uint32_t, PrevPalette>> updates;
    for (size_t a = 0; a < actors.size(); ++a) {
        const uint32_t base = actors[a].base;
        const uint32_t end = (a + 1 < actors.size()) ? actors[a + 1].base : feed.boneCount;
        const float* cur = bones + static_cast<size_t>(base) * 16u;
        const size_t floats = static_cast<size_t>(end - base) * 16u;
        if (const auto it = prevPalettes.find(actors[a].key); it != prevPalettes.end()) {
            const auto& [prevKey, prev] = *it;
            if (prev.frame + 1u == frame && prev.bones.size() == floats) {
                const float* p = prev.bones.data();
                const float dx = cur[12] - p[12], dy = cur[13] - p[13], dz = cur[14] - p[14];   // root translation (row3, Stuff space)
                if (dx * dx + dy * dy + dz * dz < 64.0f * 64.0f) std::memcpy(tail + static_cast<size_t>(base) * 16u, p, floats * sizeof(float));
            }
        }
        updates.emplace_back(actors[a].key, PrevPalette{std::vector<float>(cur, cur + floats), frame});
    }
    for (auto& [key, pal] : updates) prevPalettes[key] = std::move(pal);
    if ((frame & 63u) == 0u)
        std::erase_if(prevPalettes, [now = frame](const auto& kv) { const auto& [key, pal] = kv; return pal.frame + 4u < now; });
}
