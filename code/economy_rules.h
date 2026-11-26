// code/economy_rules.h -- pure helpers for the two economy rules (GameRules::cbillMultiplier, GameRules::salvage). Standard library only.
//
// payoutScaled: the mission-objective C-Bill payout (LogisticsData::endMission, per successful objective) scaled by the multiplier percent.
//   pct == 100 returns the input untouched; otherwise a 64-bit intermediate, truncation toward zero, clamped to int. Only the objective
//   payout is scaled: the additionalCBills stipend, salvage refunds and cheats are not income from objectives and stay unscaled.
// salvagePriceFactor: the fraction of a mech's cost charged to salvage it (salvagemecharea.cpp). MC2 (stock) is exactly 0.8f; Generous is
//   0.6f (cheaper); Scarce is 1.0f (full price). No RNG; which mechs are offered for salvage is unchanged.
#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <map>

#include "game_rules.h"

namespace mc2::rules {

inline long weaponSupplyCap(long mode) { return mode == 1 ? 2 : mode == 2 ? 1 : mode == 3 ? 3 : 0; }
inline bool salvageVehicleEligible(bool all, bool vehicles, bool enemy, bool air, bool disabled, bool destroyed) {
    return all && vehicles && enemy && !air && (disabled || destroyed);
}

struct WeaponSupply {
    std::map<long, long> spare, bought;
    bool allows(long cap, const std::map<long, long>& delta) const {
        if (!cap) return true;
        for (const auto& [id, count] : delta) {
            const long s = spare.contains(id) ? spare.at(id) : 0;
            const long b = bought.contains(id) ? bought.at(id) : 0;
            if (count > 0 && count > s + cap - b) return false;
        }
        return true;
    }
    void settle(const std::map<long, long>& delta) {
        for (const auto& [id, count] : delta) {
            const long used = std::clamp(count, 0L, spare[id]);
            spare[id] += count < 0 ? -count : -used;
            bought[id] += std::max(0L, count - used);
        }
    }
    template<class File> void save(File& file) const {
        if (spare.empty() && bought.empty()) return;
        file.writeBlock("WeaponSupply");
        long row = 0;
        auto entries = spare;
        for (const auto& [id, count] : bought) entries.try_emplace(id, 0);
        file.writeIdLong("Count", static_cast<long>(entries.size()));
        for (const auto& [id, count] : entries) {
            const auto key = std::to_string(row++);
            file.writeIdLong("Id" + key, id);
            file.writeIdLong("Spare" + key, count);
            file.writeIdLong("Bought" + key, bought.contains(id) ? bought.at(id) : 0);
        }
    }
    template<class File> void load(File& file) {
        *this = {};
        if (file.seekBlock("WeaponSupply") != 0) return;
        long count = 0;
        file.readIdLong("Count", count);
        for (long row = 0; row < std::clamp(count, 0L, 4096L); ++row) {
            const auto key = std::to_string(row);
            long id = 0, s = 0, b = 0;
            if (file.readIdLong("Id" + key, id) != 0) continue;
            file.readIdLong("Spare" + key, s);
            file.readIdLong("Bought" + key, b);
            spare[id] = std::max(0L, s);
            bought[id] = std::max(0L, b);
        }
    }
};

inline int payoutScaled(int rp, long pct) {
    if (pct == 100) return rp;
    const std::int64_t v = static_cast<std::int64_t>(rp) * static_cast<std::int64_t>(pct) / 100;  // truncates toward zero
    constexpr std::int64_t kLo = INT_MIN, kHi = INT_MAX;
    return static_cast<int>(std::clamp(v, kLo, kHi));
}

inline float salvagePriceFactor(long mode) {
    if (mode == kSalvageGenerous) return 0.6f;
    if (mode == kSalvageScarce) return 1.0f;
    return .8f;  // kSalvageMC2 and any unknown value: stock
}

}  // namespace mc2::rules
