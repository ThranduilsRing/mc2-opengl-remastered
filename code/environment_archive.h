// B7.6 Environment value record (design: .claude/cleanup/cxx23-step4-save-schema-design.md, Environment row:
// weather schedule). Private codec only: Weather::save (FIT text) is still the writer and nothing calls this.
// Tick fields count simulation ticks; scheduleSystemId references the Clock schedule registry.
#pragma once
#include "save_sections.h"

#include <optional>

namespace mc2::save {

inline constexpr auto tagEnvironment = fourcc('E', 'N', 'V', 'W');

struct EnvironmentState {
    bool weatherActive = false;
    float rainLevel = 0, lightningLevel = 0, rainTrend = 0, rainFactor = 0, rainLightLevel = 0;
    std::int32_t baseRainChance = 0, baseLightningChance = 0; // percent 0..100
    std::uint64_t nextRainCheckTick = 0, nextLightningCheckTick = 0;
    std::optional<std::uint64_t> thunderDueTick;
    std::uint32_t scheduleSystemId = 0;
    std::uint64_t nextCueSequence = 0;

    void serialize(Archive& a) {
        a(weatherActive, rainLevel, lightningLevel, rainTrend, rainFactor, rainLightLevel, baseRainChance,
          baseLightningChance, nextRainCheckTick, nextLightningCheckTick, thunderDueTick, scheduleSystemId,
          nextCueSequence);
        if (!a.good()) return;
        if (rainLevel < 0.0f || rainLevel > 4.0f) { a.reject("environment_rain_level"); return; }
        if (baseRainChance < 0 || baseRainChance > 100 || baseLightningChance < 0 || baseLightningChance > 100) {
            a.reject("environment_chance");
            return;
        }
    }
    bool operator==(const EnvironmentState&) const = default;
};

} // namespace mc2::save
