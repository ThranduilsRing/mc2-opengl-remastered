// mission_light_profile.h -- LOCAL-LIGHTS-4: which per-mission night profile a mission file name selects.
// Header-only (camera.cpp and the unit tests share it).
#pragma once
#include <cstddef>
#include <string_view>

namespace mc2_mission_light {

// Base name (no directory, no extension) equals `profile`, or starts with it followed by '_' (a save of that
// mission, "mc2_10_saved_204523"); ASCII case-insensitive. "mc2_100" does not match "mc2_10".
inline bool matches(std::string_view missionFileName, std::string_view profile)
{
    const size_t slash = missionFileName.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? missionFileName : missionFileName.substr(slash + 1);
    name = name.substr(0, name.find('.'));
    if (name.size() < profile.size()) return false;
    for (size_t i = 0; i < profile.size(); ++i) {
        char a = name[i], b = profile[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return name.size() == profile.size() || name[profile.size()] == '_';
}

} // namespace mc2_mission_light
