#pragma once

#include <algorithm>
#include <optional>
#include <string>

class FitIniFile;

namespace logistics_deployment {
inline constexpr int singlePlayerSlots = 24;
inline constexpr long legacyVersion = 10004;
inline constexpr long recordVersion = 10005;

// Multiplayer storage and wire formats remain twelve-wide.
inline int capacity(int configured = singlePlayerSlots, int players = 0)
{
    return players > 0 ? std::min(12, 36 / players)
                       : std::clamp(configured, 0, singlePlayerSlots);
}

inline bool validSlot(long slot, int cap)
{
    return slot > 0 && slot <= cap;
}

inline bool canAdd(int count, int cap, float weight, float current, float maximum)
{
    return count < cap && current + weight <= maximum;
}

// Only 10004 uses pilot order to reconstruct deployment. Keep its counter,
// including increments beyond twelve, independent of the new SP policy.
inline int legacyPilotSlot(int& count)
{
    ++count;
    return count > -1 && count < 13 ? count : 0;
}

// This is the campaign inventory record, not the in-mission mover save layout.
struct MechRecord {
    std::string chassis;
    std::string variant;
    std::optional<std::string> pilot;
    bool designer = false;
    long slot = 0;
};

void writeMech(FitIniFile& file, long index, const MechRecord& record);
std::optional<MechRecord> readMech(FitIniFile& file, long version);
// Validate before clearing the live campaign. A rejected record leaves it intact.
bool compatible(FitIniFile& file, long version, long inventoryCount);
int readCap(FitIniFile& file, int fallback = singlePlayerSlots);
} // namespace logistics_deployment
