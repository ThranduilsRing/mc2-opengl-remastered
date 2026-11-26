#pragma once
#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mc2::mods {
inline std::vector<std::string> split(std::string_view ids) {
    std::vector<std::string> out;
    while (!ids.empty()) {
        const auto end = ids.find_first_of(",;");
        auto id = ids.substr(0, end);
        if (!id.empty() && id != "-" && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-@.") == std::string_view::npos && id != "." && id != "..")
            out.emplace_back(id);
        ids = end == std::string_view::npos ? std::string_view{} : ids.substr(end + 1);
    }
    return out;
}
inline bool selected(std::string_view ids, std::string_view id) {
    const auto list = split(ids);
    return std::find(list.begin(), list.end(), id) != list.end();
}
inline void toggle(std::string& ids, std::string_view id) {
    auto list = split(ids);
    if (selected(ids, id)) std::erase(list, id);
    else list.emplace_back(id);
    ids.clear();
    for (const auto& item : list) { if (!ids.empty()) ids += ','; ids += item; }
}
struct Addon {
    std::string id, name, description;
    std::set<std::string> files;
    std::vector<std::string> layers;
};
inline std::string conflict(const Addon& candidate, const std::vector<Addon>& addons, std::string_view selection) {
    for (const auto& other : addons) {
        if (other.id == candidate.id || !selected(selection, other.id)) continue;
        for (const auto& key : candidate.files)
            if (other.files.contains(key)) return other.name + ": " + key;
    }
    return {};
}
// Preserve graphics/base layers; gameplay layers are owned entirely by campaign rules.
inline std::string dependencies(std::string_view mounted, std::string_view selection, const std::vector<Addon>& addons) {
    std::string result;
    auto append = [&](const std::string& id) { if (!result.empty()) result += ','; result += id; };
    for (const auto& id : split(selection)) if (!id.starts_with('@')) {
        if (!selected(result, id)) append(id);
        for (const auto& addon : addons) if (addon.id == id)
            for (const auto& layer : addon.layers) if (!selected(result, layer)) append(layer);
    }
    for (const auto& id : split(mounted))
        if (!selected(result, id) && std::none_of(addons.begin(), addons.end(), [&](const auto& a) { return a.id == id; })) append(id);
    return result;
}
} // namespace mc2::mods
