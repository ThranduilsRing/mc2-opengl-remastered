// Warrior value records. Native warrior.h layouts remain the legacy reader ABI.
#pragma once
#include "warrior.h"
#include "save_sections.h"

namespace mc2::save {
inline constexpr auto warriorStaticTag = fourcc('W', 'S', 'T', 'A');

struct WarriorStaticRecord {
    StaticMechWarriorData value{};
    void serialize(Archive& a) {
        a(value.numWarriors, value.numWarriorsInCombat);
        for (auto& enabled : value.brainsEnabled) a(enabled);
        a(value.minSkill, value.maxSkill, value.increaseCap, value.maxVisualRadius,
          value.curEventID, value.curEventTrigger);
    }
};
}
