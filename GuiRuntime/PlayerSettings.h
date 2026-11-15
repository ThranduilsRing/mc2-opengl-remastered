#pragma once
#include "Gates/gate_cold.h"
#include "Gates/render_options_cfg.h"
#include "Gates/render_options_legacy_env.h"
#include "../3rdparty/include/nlohmann/json.hpp"
#include <charconv>
#include <cctype>
#include <algorithm>

namespace PlayerFrontEnd {
inline bool contains(std::string_view list, std::string_view id) {
    std::size_t start = 0;
    while (start < list.size()) {
        const auto end = list.find_first_of(",;", start);
        if (list.substr(start, end - start) == id) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}
inline bool packToggleAllowed(bool enabled, std::string_view problem) { return enabled || problem.empty(); }
inline bool packSelected(std::string_view id, std::string_view campaign, std::string_view deps) {
    return id == campaign || contains(deps, id);
}
inline std::string packImportProblem(std::string problem, const nlohmann::json& status, const std::string& id) {
    if (status.contains("sources") && status["sources"].contains(id)) {
        const auto& entry = status["sources"][id];
        if (entry.contains("error") && entry["error"].is_string()) return "Import failed: " + entry["error"].get<std::string>();
    }
    return problem;
}
inline bool campaignStartAllowed(bool importing, bool playable, bool missingBase, std::string_view problem) {
    return !importing && playable && !missingBase && problem.empty();
}
template<class Packs>
std::string pickerDependencies(const Packs& packs, std::string_view campaign, std::string_view base, std::string_view texture) {
    std::string deps;
    auto append = [&](std::string_view id) { if (id.empty() || contains(deps, id)) return; if (!deps.empty()) deps += ','; deps += id; };
    for (const auto& pack : packs)
        if (pack.enabled && pack.id != campaign && pack.kind != "campaign" && pack.kind != "dependency" && pack.group != "texture-quality") append(pack.id);
    append(texture);
    append(base);
    for (const auto& pack : packs) if (pack.enabled || pack.id == campaign)
        for (const auto& dependency : pack.dependencies) append(dependency);
    return deps;
}
inline std::string savedDependencies(const nlohmann::json& settings) {
    auto deps = settings.value("last_addons", std::string{});
    for (const auto* key : {"tex_pack_mod", "last_base"}) {
        const auto id = settings.value(key, std::string{});
        if (id.empty() || contains(deps, id)) continue;
        if (!deps.empty()) deps += ',';
        deps += id;
    }
    return deps;
}
struct CampaignInfo {
    std::string title;
    int missions = 0;
};
inline CampaignInfo campaignInfo(std::string_view source) {
    CampaignInfo result;
    bool campaignSection = false;
    while (!source.empty()) {
        const auto end = source.find('\n');
        auto line = source.substr(0, end);
        source = end == std::string_view::npos ? std::string_view{} : source.substr(end + 1);
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) continue;
        line.remove_prefix(first);
        if (line.front() == '[') {
            campaignSection = line.substr(0, 10) == "[Campaign]";
            if (line.starts_with("[Group") && line.find("Mission") != std::string_view::npos) ++result.missions;
        }
        if (!campaignSection || !line.starts_with("st CampaignName")) continue;
        line.remove_prefix(15);
        const auto equal = line.find_first_not_of(" \t");
        if (equal == std::string_view::npos || line[equal] != '=') continue;
        const auto quote = line.find_first_not_of(" \t", equal + 1);
        if (quote == std::string_view::npos || line[quote] != '"') continue;
        const auto close = line.find('"', quote + 1);
        if (close != std::string_view::npos) result.title = line.substr(quote + 1, close - quote - 1);
    }
    return result;
}
inline std::string campaignTitle(std::string_view name, std::string_view fitTitle, std::string_view folder) {
    return std::string(!name.empty() ? name : !fitTitle.empty() ? fitTitle : folder);
}
inline bool unrecognisedCampaign(std::string_view name, bool hasCampaignFit) {
    return name.empty() && !hasCampaignFit;
}
inline std::string packKind(std::string_view declared, std::string_view id, bool campaign, bool missions) {
    if (!declared.empty()) return std::string(declared);
    if (id.find("-compat") != std::string_view::npos) return "dependency";
    return campaign || missions ? "campaign" : "assets";
}
inline std::string packProblem(const nlohmann::json& manifest, bool manifestExists, bool hasData,
                               bool hasArchives, std::string_view id) {
    if (id.find_first_of(",;") != std::string_view::npos) return "Folder name contains a dependency separator; rename it.";
    if (!hasData) return "No data/ folder; import the extracted campaign folder first.";
    if (manifestExists && !manifest.is_object()) return "Invalid mod.json: expected a JSON object.";
    for (const auto* key : {"id", "name", "type"})
        if (manifest.contains(key) && !manifest[key].is_string()) return std::string("Invalid mod.json: ") + key + " must be text.";
    if (manifest.contains("id") && manifest["id"].get<std::string>() != id)
        return "Manifest id differs from folder name; rename the folder to match its id.";
    if (manifest.contains("type")) {
        const auto type = manifest["type"].get<std::string>();
        if (type != "campaign" && type != "assets" && type != "dependency") return "Unsupported mod.json type; use campaign, assets or dependency.";
    }
    if (manifest.contains("dependencies") && (!manifest["dependencies"].is_array() ||
        std::any_of(manifest["dependencies"].begin(), manifest["dependencies"].end(), [](const auto& v) { return !v.is_string(); })))
        return "Invalid mod.json: dependencies must be a list of folder names.";
    if (hasArchives) return "Legacy FST install: import this folder with Import community campaigns before playing.";
    return {};
}
// Drop retired/unknown environment names even when their old value is invalid.
inline void migrateLegacySettings(nlohmann::json& settings, std::string& cfg) {
    for (const auto& key : mc2::options::legacy::kLegacyKeys) {
        const auto it = settings.find(key.envName);
        if (it == settings.end()) continue;
        const auto* option = mc2::options::find(key.option);
        const auto named = settings.find(key.option);
        const auto& source = named != settings.end() ? *named : *it;
        if (source.is_string()) {
            const auto value = mc2::options::legacy::translate(*option, source.get<std::string>());
            if (value) cfg = mc2::options::cfg::set(std::move(cfg), *option, *value);
        }
        settings.erase(key.envName);
    }
    for (const auto& option : mc2::options::kOptions) {
        const auto it = settings.find(option.key);
        if (it == settings.end()) continue;
        if (it->is_string()) {
            auto value = it->get<std::string>();
            if (value.empty() && option.kind == mc2::options::Kind::Bool) value = "FALSE";
            const auto translated = mc2::options::legacy::translate(option, value);
            if (translated) cfg = mc2::options::cfg::set(std::move(cfg), option, *translated);
        }
        settings.erase(option.key);
    }
    for (auto it = settings.begin(); it != settings.end();) {
        if (it.key().starts_with("MC2_") && !mc2::gate::lookupName(it.key())) it = settings.erase(it);
        else ++it;
    }
}

inline bool hasHighObjectNumber(std::string_view source) {
    for (std::size_t at = source.find("objectnumber"); at != std::string_view::npos;
         at = source.find("objectnumber", at + 12)) {
        const auto lineStart = source.rfind('\n', at);
        const auto start = lineStart == std::string_view::npos ? 0 : lineStart + 1;
        const auto comment = source.find_first_of(";/", start);
        if (comment < at || (at > start && (std::isalnum(static_cast<unsigned char>(source[at - 1])) || source[at - 1] == '_'))) continue;
        auto value = source.substr(at + 12, source.find_first_of("\r\n", at) - (at + 12));
        const auto equal = value.find_first_not_of(" \t");
        if (equal == std::string_view::npos || value[equal] != '=') continue;
        value.remove_prefix(equal + 1);
        const auto number = value.find_first_not_of(" \t");
        if (number == std::string_view::npos) continue;
        value.remove_prefix(number);
        int id = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
        if (parsed.ec == std::errc{} && id > 1188) return true;
    }
    return false;
}
}
