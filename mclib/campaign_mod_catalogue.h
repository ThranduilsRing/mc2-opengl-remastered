#pragma once
#include "campaign_mods.h"
#include <filesystem>
#include <fstream>
#include "../3rdparty/include/nlohmann/json.hpp"
namespace mc2::mods {
inline std::set<std::string> files(const std::filesystem::path& root, std::string_view id, std::set<std::string>& visited) {
    std::set<std::string> result;
    if (!visited.emplace(id).second) return result;
    const auto dir = root / id;
    std::ifstream stream(dir / "mod.json");
    auto json = nlohmann::json::parse(stream, nullptr, false);
    if (json.is_object() && json.contains("dependencies") && json["dependencies"].is_array())
        for (const auto& dep : json["dependencies"]) if (dep.is_string()) {
            const auto ids = split(dep.get<std::string>());
            if (ids.size() == 1) result.merge(files(root, ids.front(), visited));
        }
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir / "data", ec), end; it != end && !ec; it.increment(ec)) {
        if (it->path().filename().string().starts_with('.')) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        auto key = it->path().lexically_relative(dir).generic_string();
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        result.insert(std::move(key));
    }
    return result;
}
inline std::vector<Addon> catalogue(const std::filesystem::path& root) {
    std::vector<Addon> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const auto id = it->path().filename().string();
        if (split(id).size() != 1 || id.starts_with('.')) continue;
        std::ifstream stream(it->path() / "mod.json");
        const auto json = nlohmann::json::parse(stream, nullptr, false);
        if (!json.is_object()) continue;
        const auto type = json.value("type", std::string{});
        if (type == "campaign" || !json.value("campaign", std::string{}).empty()) continue;
        // Texture/model/effect-only packs belong to graphics. Bundled art stays automatic.
        if (id == "mc2-industrial-kit" || id == "mc2-vehicle-kit" || type == "textures" || type == "texture") continue;
        std::set<std::string> visited;
        auto keys = files(root, id, visited);
        const bool gameplay = json.contains("env") || std::any_of(keys.begin(), keys.end(), [](const auto& key) {
            return key.starts_with("data/objects/") || key.starts_with("data/missions/") || key.starts_with("data/defs/equipment/") ||
                   key.starts_with("data/defs/units/") || key.starts_with("data/pilots/") || key.starts_with("data/purchase/");
        });
        if (!gameplay) continue;
        out.push_back({id, json.value("name", id), json.value("description", std::string{}), std::move(keys), std::vector<std::string>(visited.begin(), visited.end())});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return out;
}
} // namespace mc2::mods
