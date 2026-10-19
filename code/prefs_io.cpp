// Copyright (C) Microsoft Corporation. All rights reserved.
#include "platform_io.h"
#include "platform_str.h"
#include "prefs.h"
#include "Gates/render_options_publish.h"
extern int loadPreferenceHotKeys(FitIniFile&);
extern int savePreferenceHotKeys(std::string&);
extern unsigned long long preferenceHotKeysFingerprint();
int CPrefs::load(const char* name) {
    FullPathFileName path;
    path.init("./", name, ".cfg");
    loadedDefault_ = S_stricmp(name, "options") == 0;
    sourceText_ = mc2::options::cfg::readTextFile(path);
    static_cast<mc2::options::Preferences&>(*this) = mc2::options::Preferences{};
    FitIniFile file;
    if (!sourceText_.empty() && file.open(sourceText_.data(), static_cast<int>(sourceText_.size())) == NO_ERR) {
        mc2::options::loadPreferences(file, *this);
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
    loadedValues_ = mc2::options::preferenceSnapshot(*this, render);
    // Startup reads these before SoundSystem exists; loading the defaults-screen draft must not publish it.
    if (this == &prefs) mc2::options::runtimePreferences() = static_cast<const mc2::options::Preferences&>(*this);
#ifndef VIEWER
    loadedKeyboard_ = preferenceHotKeysFingerprint();
#endif
    return 0;
}

int CPrefs::save() {
    const auto values = mc2::options::preferenceSnapshot(*this, render);
    std::string text = mc2::options::rewritePreferences(sourceText_, loadedValues_, values);
#ifndef VIEWER
    if (sourceText_.empty() || loadedKeyboard_ != preferenceHotKeysFingerprint())
        savePreferenceHotKeys(text);
#endif
    if (loadedDefault_ && text == sourceText_ && !sourceText_.empty()) return 0;
    // Keep the existing recoverable backup, and never expose a partially written file to the next startup.
    if (std::FILE* output = std::fopen("options.cfg.tmp", "wb")) {
        const bool written = std::fwrite(text.data(), 1, text.size(), output) == text.size();
        const bool closed = std::fclose(output) == 0;
        if (!written || !closed) { std::remove("options.cfg.tmp"); return -1; }
    } else return -1;
    _chmod("options.cfg", _S_IWRITE); _chmod("options.cfg.old", _S_IWRITE);
    std::remove("options.cfg.old");
    std::rename("options.cfg", "options.cfg.old");
    if (std::rename("options.cfg.tmp", "options.cfg") != 0) {
        std::rename("options.cfg.old", "options.cfg");
        return -1;
    }
    sourceText_ = std::move(text); loadedValues_ = values; loadedDefault_ = true;
#ifndef VIEWER
    loadedKeyboard_ = preferenceHotKeysFingerprint();
#endif
    return 0;
}
