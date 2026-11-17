// The commanderPartTeam fallback of Mission::initCommanders (code/mission_commander_team.h).
#include "doctest.h"
#include "../../code/mission_commander_team.h"

#include <cstring>

namespace {
using namespace mission_commander_team;
constexpr long kMaxTeams = 4;
bool teamsZeroToTwo(long t) { return t >= 0 && t <= 2; }
void clear(signed char (&t)[kCommanderSlots]) { std::memset(t, -1, sizeof(t)); }
}  // namespace

TEST_CASE("commander team: team 0 is a valid part team") {
    signed char t[kCommanderSlots]; clear(t);
    notePartTeam(t, 2, 0);
    CHECK(resolvePartTeam(t, 2, true, kMaxTeams, teamsZeroToTwo) == 0);
}

TEST_CASE("commander team: a non-home team resolves (MCO: commander 2 declares team 1)") {
    signed char t[kCommanderSlots]; clear(t);
    notePartTeam(t, 2, 1);
    CHECK(resolvePartTeam(t, 2, true, kMaxTeams, teamsZeroToTwo) == 1);
}

TEST_CASE("commander team: conflicting part TeamIds, the first wins") {
    signed char t[kCommanderSlots]; clear(t);
    notePartTeam(t, 3, 2);
    notePartTeam(t, 3, 0);
    notePartTeam(t, 3, 1);
    CHECK(t[3] == 2);
    CHECK(resolvePartTeam(t, 3, true, kMaxTeams, teamsZeroToTwo) == 2);
}

TEST_CASE("commander team: no parts, bad ids, missing commander or missing team resolve to -1") {
    signed char t[kCommanderSlots]; clear(t);
    CHECK(resolvePartTeam(t, 1, true, kMaxTeams, teamsZeroToTwo) == -1);   // no parts
    notePartTeam(t, 1, -1);                                                 // a part without a team records nothing
    notePartTeam(t, 8, 1);                                                  // commander outside the table
    notePartTeam(t, -1, 1);
    CHECK(resolvePartTeam(t, 1, true, kMaxTeams, teamsZeroToTwo) == -1);
    CHECK(resolvePartTeam(t, 8, true, kMaxTeams, teamsZeroToTwo) == -1);
    notePartTeam(t, 4, 3);
    CHECK(resolvePartTeam(t, 4, true, kMaxTeams, teamsZeroToTwo) == -1);    // team 3 does not exist
    notePartTeam(t, 5, 7);
    CHECK(resolvePartTeam(t, 5, true, kMaxTeams, teamsZeroToTwo) == -1);    // beyond MAX_TEAMS
    notePartTeam(t, 6, 1);
    CHECK(resolvePartTeam(t, 6, false, kMaxTeams, teamsZeroToTwo) == -1);   // commander object missing
}


TEST_CASE("commander team: sparse commander gaps are unused identity rows") {
    signed char t[kCommanderSlots]; clear(t);
    for (long c : {0L, 1L, 2L, 7L}) notePartTeam(t, c, c == 1 ? 1 : 0);
    for (long c = 0; c < kCommanderSlots; ++c)
        CHECK(unusedIdentity(t, c, c, c) == (c >= 3 && c <= 6));
    CHECK_FALSE(unusedIdentity(t, 3, 1, 3));
    CHECK_FALSE(unusedIdentity(t, 8, 8, 8));
    CHECK_FALSE(unusedIdentity(t, -1, -1, -1));
}
