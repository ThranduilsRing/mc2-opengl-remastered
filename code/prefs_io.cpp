#include "../mclib/campaign_mod_catalogue.h"
// Copyright (C) Microsoft Corporation. All rights reserved.
#include "mc2_czstring.h"
#include "platform_io.h"
#include "platform_str.h"
#include "prefs.h"
#include "Gates/gate.h"
#include "net/user_paths.h"
#include "Gates/preferences_cfg.h"
#include "Gates/render_options_publish.h"
static std::string launcherModDefaults() {
    std::string ids;
    static const auto addons = mc2::mods::catalogue("mods");
    for (const auto& addon : addons)
        if (mc2::mods::selected(mc2::gate::str<"MC2_MOD_DEPS">(), addon.id) && mc2::mods::conflict(addon, addons, ids).empty()) mc2::mods::toggle(ids, addon.id);
    for (const auto& [id, on] : {std::pair{"@faster-weapons", mc2::gate::get<"MC2_DIRECT_FIRE_STRAIGHT">()},
         {"@infinite-money", mc2::gate::get<"MC2_CHEAT_INFINITE_MONEY">()}, {"@salvage-all", mc2::gate::get<"MC2_CHEAT_SALVAGE_ALL">()},
         {"@purchase-all", mc2::gate::get<"MC2_PURCHASE_ALL">()}, {"@jumpjets-all", mc2::gate::get<"MC2_JUMPJETS_ALL">()}})
        if (on) mc2::mods::toggle(ids, id);
    return ids;
}
extern int loadPreferenceHotKeys(FitIniFile&);
extern int savePreferenceHotKeys(std::string&);
extern unsigned long long preferenceHotKeysFingerprint();
// The Gates snapshot plus the game-rules keys (owned here, see prefs.h).
std::vector<mc2::options::PreferenceValue> CPrefs::snapshotValues() const {
    auto out = mc2::options::preferenceSnapshot(*this, render);
    // A rule key equal to the default is never added to options.cfg; a key already in the file stays tracked so a revert to the default
    // still rewrites the stored non-default value.
    const auto defaults = mc2::rules::cfgEntries(mc2::rules::newCampaignDefaults());
    const auto current = mc2::rules::cfgEntries(gameRules);
    for (std::size_t i = 0; i < current.size(); ++i) {
        if ((current[i].key != "GameRulesMods" || launcherModDefaults().empty()) && current[i].text == defaults[i].text && !mc2::options::cfg::get(sourceText_, current[i].key).has_value()) continue;
        out.push_back({current[i].key, current[i].text, current[i].type});
    }
    return out;
}
int CPrefs::load(mc2::czstring name) {
    FullPathFileName path;
    path.init("./", name, ".cfg");
    loadedDefault_ = S_stricmp(name, "options") == 0;
    const char* namePath = path;
    sourceText_ = mc2::options::cfg::readTextFile(mc2::net::userFile(namePath).c_str());
    static_cast<mc2::options::Preferences&>(*this) = mc2::options::Preferences{};
    gameRules = mc2::rules::newCampaignDefaults();   // no stored rules: a new campaign starts from the Magic packs
    FitIniFile file;
    if (!sourceText_.empty() && file.open(sourceText_.data(), static_cast<int>(sourceText_.size())) == NO_ERR) {
        mc2::options::loadPreferences(file, *this);
        mc2::rules::loadCfgText(gameRules, sourceText_);
#ifndef VIEWER
        loadPreferenceHotKeys(file);
#endif
        file.close();
    }
    if (renderer < 0 || renderer > 3) renderer = 0;
    if (bitDepth && renderer == 3) bitDepth = 16;
    render = mc2::options::RenderOptions{};
    std::vector<mc2::options::Reject> rejects;
    mc2::options::cfg::loadFromText(render, sourceText_, &rejects);
    for (const auto& reject : rejects)
        mc2::options::logRejectedOption(reject.key, reject.text, reject.why, mc2::options::find(reject.key));
    if (!mc2::options::cfg::get(sourceText_, "GameRulesMods") || mc2::gate::isSet<"MC2_LAUNCHED">()) {
        gameRules.mods = launcherModDefaults();   // the launcher's preselection, on top of the new-campaign defaults
        for (const auto& id : mc2::mods::split(mc2::rules::newCampaignDefaults().mods))
            if (!mc2::mods::selected(gameRules.mods, id)) mc2::mods::toggle(gameRules.mods, id);
    }
    loadedValues_ = snapshotValues();
    // Startup reads these before SoundSystem exists; loading the defaults-screen draft must not publish it.
    if (this == &prefs) mc2::options::runtimePreferences() = static_cast<const mc2::options::Preferences&>(*this);
#ifndef VIEWER
    loadedKeyboard_ = preferenceHotKeysFingerprint();
#endif
    return 0;
}

std::expected<void, int> CPrefs::save() {
    const auto values = snapshotValues();
    std::string text = mc2::options::rewritePreferences(sourceText_, loadedValues_, values);
#ifndef VIEWER
    if (sourceText_.empty() || loadedKeyboard_ != preferenceHotKeysFingerprint())
        savePreferenceHotKeys(text);
#endif
    if (loadedDefault_ && text == sourceText_ && !sourceText_.empty()) return {};
    // Keep the existing recoverable backup, and never expose a partially written file to the next startup.
    const auto current = mc2::net::userFile("options.cfg");
    const auto temporary = mc2::net::userFile("options.cfg.tmp");
    const auto backup = mc2::net::userFile("options.cfg.old");
    if (std::FILE* output = std::fopen(temporary.c_str(), "wb")) {
        const bool written = std::fwrite(text.data(), 1, text.size(), output) == text.size();
        const bool closed = std::fclose(output) == 0;
        if (!written || !closed) { std::remove(temporary.c_str()); return std::unexpected(-1); }
    } else return std::unexpected(-1);
    _chmod(current.c_str(), _S_IWRITE); _chmod(backup.c_str(), _S_IWRITE);
    std::remove(backup.c_str());
    std::rename(current.c_str(), backup.c_str());
    if (std::rename(temporary.c_str(), current.c_str()) != 0) {
        std::rename(backup.c_str(), current.c_str());
        return std::unexpected(-1);
    }
    sourceText_ = std::move(text); loadedValues_ = values; loadedDefault_ = true;
#ifndef VIEWER
    loadedKeyboard_ = preferenceHotKeysFingerprint();
#endif
    return {};
}
