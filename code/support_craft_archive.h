// B7.6 SupportCraft value records (design: .claude/cleanup/cxx23-step4-save-schema-design.md, SupportCraft row;
// runtime state today lives in code/missiongui.h:160-171). Private codec only: missionInterface->Save (FIT)
// is still the writer and nothing calls this.
#pragma once
#include "save_sections.h"
#include "tactical_order_queue.h"

#include <optional>
#include <vector>

namespace mc2::save {

inline constexpr auto tagSupportCraft = fourcc('S', 'U', 'P', 'C');
inline constexpr std::uint32_t maxSupportCraft = 16; // one per team slot; legacy MAX_TEAMS names the source bound

struct SupportCraft {
    std::uint32_t commanderId = 0, vehicleId = 0;
    Vec3 position;
    std::optional<mc2::sim::ObjectRef> salvageMechRef;
    bool paintingVtol = false;
    float vTolTime = 0;
    bool mechRecovered = false, vehicleDropped = false;
    std::uint32_t currentFrame = 0, currentGestureId = 0;

    void serialize(Archive& a) {
        a(commanderId, vehicleId, position, salvageMechRef, paintingVtol, vTolTime, mechRecovered,
          vehicleDropped, currentFrame, currentGestureId);
        if (a.good() && vTolTime < 0.0f) a.reject("support_vtol_time");
    }
    bool operator==(const SupportCraft&) const = default;
};

struct SupportCraftState {
    std::vector<SupportCraft> craft; // ascending commanderId, unique

    void serialize(Archive& a) {
        a.sequence(craft, maxSupportCraft);
        if (!a.good()) return;
        for (std::size_t i = 1; i < craft.size(); ++i)
            if (craft[i].commanderId <= craft[i - 1].commanderId) { a.reject("support_commander_order"); return; }
    }
    bool operator==(const SupportCraftState&) const = default;
};

} // namespace mc2::save
