// Team values; the frozen prefix remains available to legacy readers.
#pragma once
#include "dteam.h"
#include "save_sections.h"

struct _TeamData {
    long id;
    long rosterSize;
    std::int32_t roster[120];
    void serialize(mc2::save::Archive& a) {
        static_assert(sizeof(long) == 4);
        a(id, rosterSize);
        for (auto& wid : roster) a(wid);
    }
};
using TeamData = _TeamData;

struct _TeamStaticData {
    long numTeams;
    long homeTeamId;
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
