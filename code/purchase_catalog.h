#pragma once
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "objective_text.h"

namespace mc2purchase {
inline bool mountInstalled(std::string_view type, bool campaign, bool units) {
    return type != "campaign" && type != "dependency" && !campaign && (type == "assets" || units);
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

template<class File>
void saveInventoryIdentity(File& file, std::string_view pack, std::string_view name,
    std::string_view chassis, bool designer, const char* pilot = nullptr) {
    file.writeIdString("Chassis", std::string(chassis).c_str());
    saveIdentity(file, pack, name);
    if (pilot) file.writeIdString("Pilot", pilot);
    file.writeIdBoolean("Designer", designer);
}

// Prefer every saved variant key; old saves/catalogs retain the first package/name match.
template<class File, class Range, class Source>
auto loadInventoryVariant(File& file, Range& variants, Source source) {
    const auto name = objective_text::readPlain(file, "Variant").value_or(std::string());
    const auto pack = objective_text::readPlain(file, "PurchasePack");
    const auto chassis = objective_text::readPlain(file, "Chassis");
    bool designer = false;
    const bool hasDesigner = file.readIdBoolean("Designer", designer) == 0;
    auto fallback = variants.end();
    for (auto it = variants.begin(); it != variants.end(); ++it) {
        const auto& variant = **it;
        const auto candidatePack = source(variant.getFileName());
        if (!identityMatches(candidatePack, variant.getName(), pack, name)) continue;
        if (fallback == variants.end()) fallback = it;
        if ((!hasDesigner || variant.isDesignerMech() == designer) &&
            (!chassis || identityMatches(candidatePack, variant.getFileName(), pack, *chassis)))
            return it;
    }
    return fallback;
}

struct Unlock {
    std::string chassis;
    long mission = 0;
};

inline int missionNumber(std::string_view path) {
    // Custom campaigns keep their authored stock lists and get the starting additions.
    const auto slash = path.find_last_of("/\\");
    path.remove_prefix(slash == path.npos ? 0 : slash + 1);
    if (path.size() != 14 || path.substr(0, 8) != "purchase" || path.substr(10) != ".fit" ||
        path[8] < '0' || path[8] > '9' || path[9] < '0' || path[9] > '9') return 1;
    const int n = (path[8] - '0') * 10 + path[9] - '0';
    return n >= 1 && n <= 24 ? n : 0;
}

inline bool unlocked(std::string_view chassis, int mission, const std::vector<Unlock>& table) {
    return std::any_of(table.begin(), table.end(), [&](const Unlock& row) {
        return row.chassis == chassis && mission >= row.mission && row.mission > 0;
    });
}


// Package registration owns imported chassis; otherwise use the winning CSV provider.
inline std::string source(std::string_view package, std::string_view csvOwner) {
    return std::string(package.empty() ? csvOwner : package);
}

struct Item {
    std::string source, name;
    float tonnage = 0;
    bool vehicle = false;
};
inline bool purchasable(bool vehicle, bool purchaseVehicles) {
    return !vehicle || purchaseVehicles;
}

inline long vehiclePrice(long ratedPrice, float tons, long perTon, long minimum) {
    return ratedPrice > 0 ? ratedPrice : std::max(minimum, static_cast<long>(tons * perTon));
}
struct Group {
    std::string source;
    std::vector<std::size_t> items;
    bool open = false;
    bool vehicle = false;
};

// Index validation is independent of openness (F7 also visits closed groups).
inline std::vector<std::size_t> validItems(const Group& group, std::size_t itemCount) {
    std::vector<std::size_t> valid;
    for (const auto i : group.items)
        if (i < itemCount) valid.push_back(i);
    return valid;
}

inline std::vector<Group> group(const std::vector<Item>& items, std::string_view campaignSource) {
    std::vector<Group> groups;
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& candidate) {
            return candidate.source == items[i].source && candidate.vehicle == items[i].vehicle;
        });
        if (g == groups.end()) {
            groups.push_back({items[i].source, {}, !items[i].vehicle || items[i].source == campaignSource, items[i].vehicle});
            g = groups.end() - 1;
        }
        g->items.push_back(i);
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        if (a.vehicle != b.vehicle) return a.vehicle < b.vehicle;
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
