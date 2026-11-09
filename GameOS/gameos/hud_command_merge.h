#pragma once
#include <cstdint>
#include <limits>
#include <vector>

namespace mc2vk {
template<class Descriptor>
struct HudNativeCommand {
    uint32_t stream = 0; // 0=triangle color, 1=line color, 2=textured triangles
    uint32_t firstVertex = 0, count = 0;
    Descriptor set{};
    uint32_t width = 0, height = 0, textMode = 0, foreground = 0;
    bool opaque = false, oneInvAlpha = false, oneOne = false;
};

// Preserve submission order, texture serial identity (descriptor), push constants
// and primitive boundaries. Incomplete lists must remain separate draws.
template<class Descriptor>
bool AppendHudCommand(std::vector<HudNativeCommand<Descriptor>>& commands,
                      const HudNativeCommand<Descriptor>& next) {
    if (!commands.empty()) {
        auto& prev = commands.back();
        const uint32_t primitive = next.stream == 1 ? 2u : 3u;
        if (prev.stream == next.stream && prev.set == next.set &&
            prev.width == next.width && prev.height == next.height &&
            prev.textMode == next.textMode && prev.foreground == next.foreground &&
            prev.opaque == next.opaque && prev.oneInvAlpha == next.oneInvAlpha &&
            prev.oneOne == next.oneOne && prev.count % primitive == 0 &&
            next.count % primitive == 0 &&
            static_cast<uint64_t>(prev.firstVertex) + prev.count == next.firstVertex &&
            next.count <= std::numeric_limits<uint32_t>::max() - prev.count) {
            prev.count += next.count;
            return true;
        }
    }
    commands.push_back(next);
    return false;
}
} // namespace mc2vk
