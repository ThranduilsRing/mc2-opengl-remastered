#pragma once
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc2purchase {
inline bool mountInstalled(std::string_view type, bool campaign, bool units) {
    return type != "campaign" && !campaign && (type == "assets" || units);
}

template<class File>
void saveIdentity(File& file, std::string_view pack, std::string_view name) {
    file.writeIdString("PurchasePack", std::string(pack).c_str());
    file.writeIdString("Variant", std::string(name).c_str());
}

inline bool identityMatches(std::string_view pack, std::string_view name,
    const std::optional<std::string>& savedPack, std::string_view savedName) {
    const auto same = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
            return lower(x) == lower(y);
        });
    };
    return same(name, savedName) && (!savedPack || same(pack, *savedPack));
}

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
std::string componentSource(int ordinal);
}
