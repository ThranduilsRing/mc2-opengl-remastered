#pragma once

// Phase 1: only a future alpha shot may charge heat; normal fire is unchanged.
// Engine-free integer model. Lab/BV follow-ups must call capacityMilli after
// truncating EACH component's nonnegative Damage to integer lab points (and
// weapon Heat likewise), BEFORE summing and multiplying by 1000. Never round
// fractional mod values to milli-heat first. Chassis maxHeat is already integer.
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <limits>
#include <optional>

namespace mc2::heat {
inline constexpr int inventorySize = 72;
inline constexpr int ticksPerSecond = 30;
inline constexpr int maxLabPoints = std::numeric_limits<int32_t>::max() / 1000;

struct Profile {
    int heatProfile = 1;
    int coolSeconds = 20;
    int alphaWindowTicks = 60;
    int aiAlpha = 0;
    int thresholdCount = 0;
};

inline std::optional<Profile> validatedProfile(Profile p) {
    if (p.heatProfile != 1 || p.coolSeconds <= 0 ||
        p.coolSeconds > std::numeric_limits<int>::max() / ticksPerSecond ||
        p.alphaWindowTicks <= 0 || p.aiAlpha != 0 || p.thresholdCount != 0)
        return std::nullopt;
    return p;
}

inline int32_t capacityMilli(int chassisPoints, int workingSinkPoints) {
    const int64_t points = int64_t{chassisPoints} + workingSinkPoints;
    return static_cast<int32_t>(std::clamp<int64_t>(points * 1000, 0,
        std::numeric_limits<int32_t>::max()));
}

inline int32_t dissipate(int32_t heatMilli, int32_t capacity, int coolSeconds) {
    const int64_t ticks = int64_t{std::max(1, coolSeconds)} * ticksPerSecond;
    const int64_t loss = std::max<int64_t>(1, capacity / ticks);
    return static_cast<int32_t>(std::max<int64_t>(0, int64_t{heatMilli} - loss));
}

struct State {
    int32_t heatMilli = 0;
    std::bitset<inventorySize> alphaMask;
    uint64_t alphaExpireTick = 0;
    int32_t capacity = 0; // derived, never saved

    void recompute(int chassisPoints, int workingSinkPoints) {
        capacity = capacityMilli(chassisPoints, workingSinkPoints);
    }
    void tick(const Profile& p) {
        heatMilli = dissipate(heatMilli, capacity, p.coolSeconds);
    }
};
} // namespace mc2::heat
