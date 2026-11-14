#pragma once
#include <string_view>

namespace mc2::components {
// Rack sizes are authored in the CSV name, never the localized display string.
// Unnumbered missiles (Thunderbolt) are single projectiles.
inline int rackSize(std::string_view name) {
    auto marker = name.find("LRM");
    if (marker == name.npos) marker = name.find("SRM");
    if (marker == name.npos) marker = name.find("Missile/");
    if (marker == name.npos) return 1;
    auto digit = name.find_first_of("0123456789", marker);
    if (digit == name.npos) return 1;
    int count = 0;
    while (digit < name.size() && name[digit] >= '0' && name[digit] <= '9') {
        count = count * 10 + name[digit++] - '0';
        if (count > 32) return 1;
    }
    return count > 0 ? count : 1;
}

inline float nextSpeedClass(float speed) {
    constexpr float ladder[] = {5, 10, 15, 19, 23, 27, 31, 35};
    for (int i = 0; i < 7; ++i)
        if (speed <= ladder[i]) return ladder[i + 1];
    return speed;
}

inline bool defensive(float damage, unsigned char range) {
    return damage == 0.0f && range == 255;
}

inline bool canIntercept(bool disabled, float ready, float now, long ammo) {
    return !disabled && ready <= now && ammo > 0;
}
}
