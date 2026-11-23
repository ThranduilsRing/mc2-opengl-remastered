#pragma once
#include "Gates/gate_cold.h"
#include "Gates/render_options_cfg.h"
#include "Gates/render_options_legacy_env.h"
#include "../3rdparty/include/nlohmann/json.hpp"
#include <charconv>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

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
inline std::string packImportProblem(std::string problem, const nlohmann::json& status, std::string_view id) {
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
struct CampaignChoice {
    std::string pack, fit, title;
};
template<class Packs>
std::vector<CampaignChoice> campaignChoices(const Packs& packs, const std::vector<std::string>& stockFits = {},
                                           const std::function<bool(std::string_view)>& hidden = {}) {
    std::vector<CampaignChoice> choices{{"", "campaign", "Stock campaign"}};
    for (const auto& fit : stockFits) {
        bool found = false;
        for (const auto& choice : choices) if (choice.fit == fit) found = true;
        if (!found) choices.push_back({"", fit, fit});
    }
    for (const auto& pack : packs) {
        if (pack.kind != "campaign" || (hidden && hidden(pack.id))) continue;
        for (const auto& [fit, info] : pack.campaigns)
            choices.push_back({pack.id, fit, campaignTitle("", info.title, fit)});
    }
    // Identical titles (e.g. three packs shipping "Metropolitan") get the pack title appended; unique titles stay.
    std::vector<std::string> labels;
    for (const auto& choice : choices) labels.push_back(choice.title);
    for (auto& choice : choices) {
        if (choice.pack.empty() || std::count(labels.begin(), labels.end(), choice.title) < 2) continue;
        std::string label = choice.pack;
        for (const auto& pack : packs) if (pack.id == choice.pack) {
            if constexpr (requires { pack.name; }) if (!pack.name.empty()) label = pack.name.substr(0, pack.name.find(" / "));
        }
        if (label != choice.title) choice.title += " (" + label + ")";
    }
    return choices;
}
// Label for one pack FIT in the picker list and dropdown (identical string in both).
inline const std::string& campaignChoiceTitle(const std::vector<CampaignChoice>& choices, std::string_view pack,
                                              std::string_view fit, const std::string& fallback) {
    for (const auto& choice : choices) if (choice.pack == pack && choice.fit == fit) return choice.title;
    return fallback;
}
// A saved last_campaign that is hidden (import source) or no longer installed falls back to stock ("").
template<class Packs>
std::string validSavedCampaign(const Packs& packs, std::string_view saved,
                               const std::function<bool(std::string_view)>& hidden = {}) {
    if (saved.empty()) return {};
    for (const auto& pack : packs) if (pack.id == saved) return hidden && hidden(saved) ? std::string() : std::string(saved);
    return {};
}
inline void selectCampaign(const CampaignChoice& choice, std::string& pack, std::string& fit) {
    pack = choice.pack;
    fit = choice.fit;
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

// ---- New Campaign menu: every installed campaign, stock + each mod (owner 11-23). ----
// A list entry's hidden text is the plain FIT name for a campaign the running content already serves, and "<mod>::<fit>"
// for one that needs a different mod layer ("::campaign" = stock with NO mod layer): picking it relaunches the game into that mod.
inline std::string campaignKey(std::string_view mod, std::string_view fit) {
    return std::string(mod) + "::" + std::string(fit);
}
inline bool splitCampaignKey(std::string_view key, std::string& mod, std::string& fit) {
    const auto at = key.find("::");
    if (at == std::string_view::npos) return false;
    mod = key.substr(0, at);
    fit = key.substr(at + 2);
    return !fit.empty();
}
struct ModCampaigns {
    std::string id, name, kind, problem;
    std::map<std::string, CampaignInfo> campaigns;
};
// Reads mods/<id>/mod.json and mods/<id>/data/campaign/*.fit. Plain folders only; FST archives are the importer's job.
inline std::vector<ModCampaigns> scanModCampaigns(const std::filesystem::path& modsRoot) {
    namespace fs = std::filesystem;
    std::vector<ModCampaigns> result;
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator i(modsRoot, ec), end; !ec && i != end; i.increment(ec))
        if (i->is_directory(ec)) dirs.push_back(i->path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& dir : dirs) {
        const auto id = dir.filename().string();
        if (id.empty() || id[0] == '.') continue;
        std::ifstream manifest(dir / "mod.json");
        const bool manifestExists = manifest.is_open();
        const auto json = manifestExists ? nlohmann::json::parse(manifest, nullptr, false) : nlohmann::json::object();
        ModCampaigns mod;
        mod.id = id;
        bool archives = false;
        for (fs::directory_iterator i(dir, ec), end; !ec && i != end; i.increment(ec)) archives |= i->path().extension() == ".fst";
        mod.problem = packProblem(json.is_discarded() ? nlohmann::json(1) : json, manifestExists, fs::is_directory(dir / "data", ec), archives, id);
        for (fs::directory_iterator i(dir / "data" / "campaign", ec), end; !ec && i != end; i.increment(ec)) {
            const auto ext = i->path().extension().string();
            if (ext != ".fit" && ext != ".FIT") continue;
            std::ifstream input(i->path());
            mod.campaigns.emplace(i->path().stem().string(), campaignInfo(std::string(std::istreambuf_iterator<char>(input), {})));
        }
        ec.clear();
        const auto text = [&](std::string_view key) { return json.is_object() && json.contains(key) && json[key].is_string() ? json[key].get<std::string>() : std::string(); };
        std::string fitTitle;
        if (!mod.campaigns.empty()) {
            const auto& [fit, info] = *mod.campaigns.begin();
            fitTitle = info.title;
        }
        mod.name = campaignTitle(text("name"), fitTitle, id);
        mod.kind = packKind(text("type"), id, !mod.campaigns.empty(), false);
        result.push_back(std::move(mod));
    }
    return result;
}
struct MenuCampaign {
    std::string key, text;
};
// Campaigns of installed mods other than the one that is mounted. Mods with a manifest problem or in `hidden` (import sources) are left out.
inline std::vector<MenuCampaign> modCampaignEntries(const std::vector<ModCampaigns>& mods, std::string_view activeMod,
                                                  const std::function<bool(std::string_view)>& hidden = {}) {
    std::vector<MenuCampaign> result;
    const auto unusable = [&](std::string_view id) {
        for (const auto& mod : mods) if (mod.id == id && !mod.problem.empty()) return true;
        return hidden && hidden(id);
    };
    for (const auto& choice : campaignChoices(mods, {}, unusable)) {
        if (choice.pack.empty() || choice.pack == activeMod) continue;
        result.push_back({campaignKey(choice.pack, choice.fit), choice.title + " [" + choice.pack + "]"});
    }
    return result;
}
// A save records the mod layers it was made under; false = an old save with no record (loads as before).
inline bool saveNeedsRelaunch(bool recorded, std::string_view savedMod, std::string_view savedDeps,
                              std::string_view activeMod, std::string_view activeDeps) {
    return recorded && (savedMod != activeMod || savedDeps != activeDeps);
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
