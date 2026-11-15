#pragma once
#include <string_view>
#include <cstddef>
#include <cstdint>

namespace mc2::components {
// A removed grid item is placed once: at the destination, or back at its source.
// Copy the source because placement callbacks can change the UI selection.
template<class Add>
inline bool finishGridDrop(long& x, long& y, bool fromGrid,
                           long sourceX, long sourceY, Add add) {
    if (add(x, y) == 0) return true;
    if (!fromGrid) return false;
    x = sourceX;
    y = sourceY;
    return add(x, y) == 0;
}

// Inactive storage can contain a previous loadout. Unmapped live components
// are preserved without indexing the logistics table with a missing location.
template<std::size_t N, class Item, class Location, class Logistics>
inline void resetSnapshot(int32_t (&out)[N], const Item (&inventory)[N], long active,
                          const Location (&locations)[N], const Logistics (&table)[N]) {
    for (std::size_t i = 0; i < N; ++i) out[i] = 0xff;
    for (long item = 0; item < active && item < static_cast<long>(N); ++item) {
        out[item] = inventory[item].masterID;
        if (out[item] == 0xff) continue;
        for (std::size_t location = 0; location < N; ++location) {
            if (locations[location] != item) continue;
            if (table[location].clearOut) out[item] = 0xff;
            break;
        }
    }
}

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
