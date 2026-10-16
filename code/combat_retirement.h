#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace mc2::combat {

inline constexpr auto invalidTick = std::numeric_limits<std::uint64_t>::max();

inline std::optional<std::uint64_t> secondsToTicks(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0) return std::nullopt;
    const double ticks = std::ceil(seconds * 1000.0);
    if (!std::isfinite(ticks) || ticks >= static_cast<double>(invalidTick)) return std::nullopt;
    return static_cast<std::uint64_t>(ticks);
}

inline std::optional<std::uint64_t> addTicks(std::uint64_t a, std::uint64_t b)
{
    if (a == invalidTick || b >= invalidTick - a) return std::nullopt;
    return a + b;
}

enum class ClockStep { advanced, clock_step, clock_overflow };

struct CombatClock {
    std::uint64_t ticks = 0;
    double frac = 0;
    bool rejectionReported = false;

    ClockStep advance(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0) return ClockStep::clock_step;
        const double total = seconds * 1000.0 + frac;
        const double whole = std::floor(total);
        if (!std::isfinite(total) || whole >= static_cast<double>(invalidTick))
            return ClockStep::clock_overflow;
        const auto next = addTicks(ticks, static_cast<std::uint64_t>(whole));
        if (!next) return ClockStep::clock_overflow;
        ticks = *next;
        frac = total - whole;
        return ClockStep::advanced;
    }
};

struct RetirePolicy {
    std::optional<std::uint64_t> ticks = 0;
    std::string_view source = "absent_default";

    static RetirePolicy fromAfterHit(bool beam, bool present, double seconds)
    {
        if (beam) return {0, "beam_default"};
        if (!present) return {};
        return {secondsToTicks(seconds), "AfterHitTime"};
    }
};

enum class ArmResult { armed, already_attempted, policy, deadline_overflow };

struct RetireDeadline {
    std::uint64_t tick = 0;
    bool armed = false;
    bool attempted = false;
    std::string_view source = "unarmed";

    ArmResult arm(std::uint64_t now, const RetirePolicy& policy)
    {
        if (attempted) return ArmResult::already_attempted;
        attempted = true;
        source = policy.source;
        if (!policy.ticks) { source = "invalid_policy"; return ArmResult::policy; }
        const auto deadline = addTicks(now, *policy.ticks);
        if (!deadline) { source = "deadline_overflow"; return ArmResult::deadline_overflow; }
        tick = *deadline;
        armed = true;
        return ArmResult::armed;
    }
};

} // namespace mc2::combat
