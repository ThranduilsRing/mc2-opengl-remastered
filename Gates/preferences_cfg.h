// The preferences schema and lossless options.cfg editor. File IO belongs to CPrefs.
#pragma once
#include "preferences_types.h"
#include "render_options_cfg.h"
#include <cstring>
namespace mc2::options {
template<class P, class F> void forEachPreference(P& p, F&& f) {
    f("DigitalMasterVolume", p.DigitalMasterVolume, 'l');
    f("MusicVolume", p.MusicVolume, 'l');
    f("RadioVolume", p.RadioVolume, 'l');
    f("SFXVolume", p.sfxVolume, 'l');
    f("BettyVolume", p.BettyVolume, 'l');
    f("Shadows", p.useShadows, 'b');
    f("DetailTexture", p.useWaterInterestTexture, 'b');
    f("HighObjectDetail", p.useHighObjectDetail, 'b');
    f("Difficulty", p.GameDifficulty, 'l');
    f("UnlimitedAmmo", p.useUnlimitedAmmo, 'b');
    f("Rasterizer", p.renderer, 'l');
    f("ResolutionX", p.resolutionX, 'l');
    f("ResolutionY", p.resolutionY, 'l');
    f("FullScreen", p.fullScreen, 'b');
    f("Brightness", p.gammaLevel, 'l');
    f("useLeftRightMouseProfile", p.useLeftRightMouseProfile, 'b');
    f("BaseColor", p.baseColor, 'l');
    f("Highlightcolor", p.highlightColor, 'l');
    f("faction", p.faction, 'l');
    f("PilotVideos", p.pilotVideos, 'b');
    f("UseUpscaledVideos", p.UseUpscaledVideos, 'l');
    f("UseLocalShadows", p.useLocalShadows, 'b');
    f("UseNonWeaponEffects", p.useNonWeaponEffects, 'b');
    f("AsyncMouse", p.asyncMouse, 'b');
    f("FogPos", p.fogPos, 'l');
    f("BitDepth", p.bitDepth, 'c');
    f("SaveTranscripts", p.saveTranscripts, 'b');
    f("Tutorials", p.tutorials, 'b');
    f("ScrollSpeedMult", p.scrollSpeedMult, 'f');
    f("ZoomSpeedMult", p.zoomSpeedMult, 'f');
    f("InsigniaFile", p.insigniaFile, 's');
    for (int i = 0; i < 10; ++i) {
        f("PlayerName" + std::to_string(i), p.playerName[i], 's');
        f("UnitName" + std::to_string(i), p.unitName[i], 's');
        f("IPAddress" + std::to_string(i), p.ipAddresses[i], 's');
    }
    mc2::rules::forEachField(p.gameRules, f);
}

// FitIniFile owns the legacy number/string grammar. Read from the one caller-owned file buffer.
template<class File, class P> void loadPreferences(File& file, P& p) {
    file.seekBlock("MechCommander2");
    bool hasPrimaryName = false;
    forEachPreference(p, [&](const auto& key, auto& value, char type) {
        const std::string name(key);
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_array_v<T>) {
            T candidate{};
            std::memcpy(candidate, value, sizeof(candidate));
            const auto result = file.readIdString(name.c_str(), candidate, sizeof(candidate) - 1);
            if (result == 0) std::memcpy(value, candidate, sizeof(candidate));
            if (name == "PlayerName0") hasPrimaryName = result == 0;
        } else {
            T candidate = value;
            long result = 0;
            if constexpr (std::is_same_v<T, bool>) {
                if (type == 'l') { long v = value; result = file.readIdLong(name.c_str(), v); candidate = v != 0; }
                else result = file.readIdBoolean(name.c_str(), candidate);
            } else if constexpr (std::is_same_v<T, char>) result = file.readIdChar(name.c_str(), candidate);
            else if constexpr (std::is_same_v<T, float>) result = file.readIdFloat(name.c_str(), candidate);
            else result = file.readIdLong(name.c_str(), candidate);
            if (result == 0) value = candidate;
        }
    });
    mc2::rules::sanitize(p.gameRules);   // a hand-edited out-of-range rule falls back to its default
    if (!hasPrimaryName) {
        file.readIdString("PlayerName", p.playerName[0], sizeof(p.playerName[0]) - 1);
        file.readIdString("UnitName", p.unitName[0], sizeof(p.unitName[0]) - 1);
    }
}
template<class T> std::string preferenceText(const T& value, char type) {
    if constexpr (std::is_array_v<T>) return "\"" + std::string(value) + "\"";
    else if constexpr (std::is_same_v<T, bool>) return type == 'b' ? (value ? "TRUE" : "FALSE") : (value ? "1" : "0");
    else if constexpr (std::is_floating_point_v<T>) {
        char buffer[48]; const auto r = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return std::string(buffer, r.ptr);
    } else return std::to_string(static_cast<long>(value));
}
template<class P> std::vector<PreferenceValue> preferenceSnapshot(const P& p, const RenderOptions& render) {
    std::vector<PreferenceValue> out;
    forEachPreference(p, [&](const auto& key, const auto& value, char type) {
        out.push_back({std::string(key), preferenceText(value, type), type});
    });
    for (const Option& option : kOptions) withValue(render, option, [&](const auto& value) {
        const char type = cfg::typeLetter(option);
        out.push_back({option.key, preferenceText(value, type), type});
    });
    return out;
}
// Replace only settings that changed since load. Unchanged bytes (including malformed values,
// comments, unknown mod keys, duplicate keys and the keyboard block) are retained exactly.
inline std::string rewritePreferences(std::string text, const std::vector<PreferenceValue>& before,
                                      const std::vector<PreferenceValue>& after) {
    const bool create = text.empty();
    if (create) text = "FITini \r\n\r\n[MechCommander2]\r\n\r\nFITend \r\n";
    for (const auto& value : after) {
        const auto found = std::find_if(before.begin(), before.end(), [&](const auto& old) { return old.key == value.key; });
        if (create || found == before.end() || found->text != value.text)
            text = cfg::setRaw(std::move(text), "MechCommander2", value.key, value.type, value.text);
    }
    return text;
}
} // namespace mc2::options
