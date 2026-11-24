// Which mission and mod a save belongs to, read from its fit text before the running mission is torn down.
// An in-place quickload can only resume the mission that is running: another mission's script and object
// tables do not match the live ones (owner crash 2026-11-23, quickload of a save from a different mission).
#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace mc2::save {

// Value of `st <key> = "value"` in fit text; nullopt when the key is missing or unterminated.
inline std::optional<std::string> fitStringValue(std::string_view text, std::string_view key) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (line.substr(0, 3) != "st ") continue;
        line.remove_prefix(3);
        if (line.substr(0, key.size()) != key) continue;
        line.remove_prefix(key.size());
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        if (line.empty() || line.front() != '=') continue;
        const size_t open = line.find('"');
        if (open == std::string_view::npos) return std::nullopt;
        const size_t close = line.find('"', open + 1);
        if (close == std::string_view::npos) return std::nullopt;
        return std::string(line.substr(open + 1, close - open - 1));
    }
    return std::nullopt;
}

inline bool sameIdentity(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// True when the save can resume in place. A save that records no mission or no mod is an older file: allowed.
inline bool savedIdentityMatches(std::string_view missionFitText, std::string_view runningMission,
                                 std::string_view campaignFitText, std::string_view runningMod,
                                 std::string_view runningDeps) {
    if (const auto saved = fitStringValue(missionFitText, "MissionName"); saved && !sameIdentity(*saved, runningMission)) return false;
    if (const auto saved = fitStringValue(campaignFitText, "ActiveMod"); saved && !sameIdentity(*saved, runningMod)) return false;
    if (const auto saved = fitStringValue(campaignFitText, "ModDeps"); saved && !sameIdentity(*saved, runningDeps)) return false;
    return true;
}

}  // namespace mc2::save
