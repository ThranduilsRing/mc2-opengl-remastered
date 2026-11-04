// Pure helpers for the "team a commander's own parts declare" fallback in Mission::initCommanders (code/mission_init.cpp).
// Header-only with no engine dependencies so tests/unit/test_commander_team.cpp can exercise the exact logic the loader runs.
#ifndef CODE_MISSION_COMMANDER_TEAM_H
#define CODE_MISSION_COMMANDER_TEAM_H

namespace mission_commander_team {

constexpr int kCommanderSlots = 8;   // MissionInitFrame::commanderPartTeam size

// Record the first [PartN] TeamId seen for a commander; later parts that disagree are ignored (first wins). teamID < 0 is "no team".
inline void notePartTeam(signed char (&table)[kCommanderSlots], long commanderID, long teamID) {
    if ((commanderID >= 0) && (commanderID < kCommanderSlots) && (teamID >= 0) && (table[commanderID] < 0))
        table[commanderID] = static_cast<signed char>(teamID);
}

// The team the commander's parts declared, or -1 when none is usable (commander missing, no parts, id outside [0, maxTeams), or
// teamExists(team) is false). teamExists is the loader's "Team::teams[t] != nullptr".
template <typename TeamExists>
inline long resolvePartTeam(const signed char (&table)[kCommanderSlots], long commander, bool commanderOk, long maxTeams, TeamExists teamExists) {
    const long partTeam = (commanderOk && (commander >= 0) && (commander < kCommanderSlots)) ? table[commander] : -1;
    return ((partTeam > -1) && (partTeam < maxTeams) && teamExists(partTeam)) ? partTeam : -1;
}

}  // namespace mission_commander_team

#endif  // CODE_MISSION_COMMANDER_TEAM_H
