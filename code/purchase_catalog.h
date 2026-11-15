#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mc2purchase {
inline bool available(bool campaignUnlocked, std::string_view source) {
    return campaignUnlocked || !source.empty();
}

// Package registration owns imported chassis; otherwise use the winning CSV provider.
inline std::string source(std::string_view package, std::string_view csvOwner) {
    return std::string(package.empty() ? csvOwner : package);
}

struct Item {
    std::string source, name;
    float tonnage = 0;
};
struct Group {
    std::string source;
    std::vector<std::size_t> items;
    bool open = false;
};

inline std::vector<Group> group(const std::vector<Item>& items, std::string_view campaignSource) {
    std::vector<Group> groups;
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& candidate) {
            return candidate.source == items[i].source;
        });
        if (g == groups.end()) {
            groups.push_back({items[i].source, {}, items[i].source == campaignSource});
            g = groups.end() - 1;
        }
        g->items.push_back(i);
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        return a.source < b.source;
    });
    for (auto& g : groups)
        std::stable_sort(g.items.begin(), g.items.end(), [&](std::size_t a, std::size_t b) {
            if (items[a].tonnage != items[b].tonnage) return items[a].tonnage > items[b].tonnage;
            return items[a].name < items[b].name;
        });
    return groups;
}

// Engine adapter, kept out of the pure grouping/availability rules above.
std::string chassisSource(std::string_view csvPath);
}
