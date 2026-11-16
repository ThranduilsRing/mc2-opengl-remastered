#pragma once
#include <cstdint>
#include <vector>

namespace mc2 {
class LauncherLightBudget {
    struct Entry {
        std::int32_t watchId;
        unsigned count;
    };
    std::vector<Entry> entries;
    std::uint32_t frameStamp = ~0u;

public:
    bool allow(std::uint32_t frame, std::int32_t watchId, unsigned limit) {
        if (frameStamp != frame) {
            frameStamp = frame;
            entries.clear();
        }
        for (auto& entry : entries)
            if (entry.watchId == watchId)
                return entry.count++ < limit;
        entries.push_back({watchId, 0});
        return entries.back().count++ < limit;
    }
};
} // namespace mc2
