// Commander values; native layout is retained only for the legacy reader.
#pragma once
#include "unitdesg.h"
#include "save_sections.h"

struct _MoverGroupData {
    std::int32_t id;
    std::int32_t numMovers;
    std::int32_t moverWIDs[MAX_MOVERGROUP_COUNT];
    std::int32_t pointWID;
    bool disbandOnNoPoint;

    void serialize(mc2::save::Archive& a) {
        a(id, numMovers);
        for (auto& wid : moverWIDs) a(wid);
        a(pointWID, disbandOnNoPoint);
    }
    bool operator==(const _MoverGroupData&) const = default;
};
using MoverGroupData = _MoverGroupData;

struct _CommanderData {
    std::int32_t id;
    std::int32_t teamId;
    MoverGroupData groups[MAX_MOVERGROUPS];

    void serialize(mc2::save::Archive& a) {
        a(id, teamId);
        for (auto& group : groups) a(group);
    }
};
using CommanderData = _CommanderData;

struct _StaticCommanderData {
    std::int32_t numCommanders;
    std::int32_t homeCommanderId;
    void serialize(mc2::save::Archive& a) { a(numCommanders, homeCommanderId); }
};
using StaticCommanderData = _StaticCommanderData;

namespace mc2::save {
inline constexpr auto commanderStaticTag = fourcc('C', 'M', 'S', 'T');
inline constexpr auto commanderDataTag = fourcc('C', 'M', 'D', 'R');

}
