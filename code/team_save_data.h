// Team values; the frozen prefix remains available to legacy readers.
#pragma once
#include "dteam.h"
#include "save_sections.h"

struct _TeamData {
    std::int32_t id;
    std::int32_t rosterSize;
    std::int32_t roster[120];
    void serialize(mc2::save::Archive& a) {
        a(id, rosterSize);
        for (auto& wid : roster) a(wid);
    }
};
using TeamData = _TeamData;

struct _TeamStaticData {
    std::int32_t numTeams;
    std::int32_t homeTeamId;
    char relations[MAX_TEAMS][MAX_TEAMS];
    bool noPain[MAX_TEAMS];
    void serialize(mc2::save::Archive& a) {
        a(numTeams, homeTeamId);
        for (auto& row : relations) for (auto& relation : row) a(relation);
        for (auto& flag : noPain) a(flag);
    }
};
using TeamStaticData = _TeamStaticData;

namespace mc2::save {
inline constexpr auto teamStaticTag = fourcc('T', 'M', 'S', 'T');
inline constexpr auto teamDataTag = fourcc('T', 'E', 'A', 'M');
}
