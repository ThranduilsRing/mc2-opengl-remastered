#pragma once
// Previous-frame bone palettes per mech actor (engine-free; the mech island feeds it, tests/unit/test_mech_motion_history.cpp pins it).
// An actor is a run of bones sharing one baseBoneOffset (key = per-instance motion key). stage() fills `tail` (boneCount bones, 16 floats
// each, indexed like the current palette) with last frame's pose so the VS can skin each vertex with both poses.
// An actor gets its CURRENT pose (camera vector only) when first seen, when it was not staged on the render frame before, when its
// bone count changed, or when its root moved 64 units or more (teleport).
#include <algorithm>
#include <compare>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

class MechMotionHistory {
public:
    // instances64: 64-byte instances, baseBoneOffset at byte 4; bones: 16 floats per bone; keys: one per instance (empty = everything
    // stays current); tail: room for the whole palette.
    void stage(std::span<const uint8_t> instances64, std::span<const float> bones, std::span<const uint32_t> keys, uint32_t renderFrame,
               std::span<float> tail) {
        const uint32_t boneCount = static_cast<uint32_t>(bones.size() / 16u);
        if (!boneCount || tail.size() < bones.size()) return;
        const uint32_t instanceCount = static_cast<uint32_t>(std::min(instances64.size() / 64u, keys.size()));
        std::memcpy(tail.data(), bones.data(), static_cast<size_t>(boneCount) * 64u);   // default: previous pose == current pose
        const uint8_t* inst = instances64.data();
        struct ActorSpan { uint32_t base, key; auto operator<=>(const ActorSpan&) const = default; };
        std::vector<ActorSpan> actors;
        if (!keys.empty()) {
            for (uint32_t i = 0; i < instanceCount; ++i) {
                uint32_t baseBone = 0;
                std::memcpy(&baseBone, inst + i * 64u + 4u, 4);
                if (keys[i] && baseBone < boneCount) actors.push_back({baseBone, keys[i]});
            }
        }
        std::sort(actors.begin(), actors.end());
        actors.erase(std::unique(actors.begin(), actors.end(), [](const ActorSpan& a, const ActorSpan& b) { return a.base == b.base; }), actors.end());
        std::vector<std::pair<uint32_t, Palette>> updates;
        for (size_t a = 0; a < actors.size(); ++a) {
            const uint32_t base = actors[a].base;
            const uint32_t end = (a + 1 < actors.size()) ? actors[a + 1].base : boneCount;
            const float* cur = bones.data() + static_cast<size_t>(base) * 16u;
            const size_t floats = static_cast<size_t>(end - base) * 16u;
            if (const auto it = pal_.find(actors[a].key); it != pal_.end()) {
                const auto& [prevKey, prev] = *it;
                if (prev.frame + 1u == renderFrame && prev.bones.size() == floats) {
                    const float* p = prev.bones.data();
                    const float dx = cur[12] - p[12], dy = cur[13] - p[13], dz = cur[14] - p[14];   // root translation (row3, Stuff space)
                    if (dx * dx + dy * dy + dz * dz < 64.0f * 64.0f) std::memcpy(tail.data() + static_cast<size_t>(base) * 16u, p, floats * sizeof(float));
                }
            }
            updates.emplace_back(actors[a].key, Palette{std::vector<float>(cur, cur + floats), renderFrame});
        }
        for (auto& [key, p] : updates) pal_[key] = std::move(p);
        if ((renderFrame & 63u) == 0u)
            std::erase_if(pal_, [now = renderFrame](const auto& kv) { const auto& [key, pal] = kv; return pal.frame + 4u < now; });
    }
    void reset() { pal_.clear(); }
    size_t actorCount() const { return pal_.size(); }

private:
    struct Palette { std::vector<float> bones; uint32_t frame = 0; };
    std::unordered_map<uint32_t, Palette> pal_;
};
