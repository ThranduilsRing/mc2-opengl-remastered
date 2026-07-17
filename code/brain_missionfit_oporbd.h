#pragma once
// BRAIN-MISSIONFIT-OPORD-CONSUMER-1 / BRAIN-FULL-PARSE-1: pure parsers for carver_v_enhanced's
// declarative inline Brain{} blocks in mission.fit.
//
// Each [WarriorN] section may carry a brace-block:
//   Brain {
//       archetype = "Archetype.InnerSphere.Standard"
//       compatibilityMode = "Enhanced"
//       EngageRadius = 300.0
//       RequestHelp = false
//       AttackerHelpRadius = 100
//       DefenderHelpRadius = 125
//       ReturnToPost = true
//       PrimaryOPORD   { type = Patrol; loop = true; Waypoint{index=0;x=…;y=…} … }
//       SecondaryOPORD { type = Guard }
//       Tactics { Flank = 0.45; Standard = 0.35; HitAndRun = 0.20 }
//       PoweredDown = true; WakeOnAttack = true        // Sentry
//   }
// FitIniFile cannot see brace-blocks (it is a flat [section]/typed-prefix reader), so these
// parsers RAW-SCAN the mission.fit text. PURE: no engine headers, no globals, no I/O.
// Unit-tested in isolation (tools/brain_missionfit_oporbd_test).

#include <cstdint>

static constexpr int kMissionFitMaxWaypoints = 8;

struct MissionFitWaypoint { float x; float y; };

// ---------------------------------------------------------------------------------------------
// Legacy slice-1 struct (PrimaryOPORD type/loop/waypoints only). Retained for the original
// consumer + harness. parseMissionFitOpords() fills one per warrior that has a PrimaryOPORD.
struct MissionFitOpord {
    int   warriorIndex;
    char  primaryType[24];
    bool  loop;
    int   waypointCount;
    MissionFitWaypoint waypoints[kMissionFitMaxWaypoints];
};

int parseMissionFitOpords(const char* text, MissionFitOpord* out, int maxOut);

// ---------------------------------------------------------------------------------------------
// BRAIN-FULL-PARSE-1: the full declarative Brain{} block.

static constexpr int kMissionFitMaxTactics = 8;

// One OPORD slot (Primary / Secondary / Tertiary).
struct MissionFitOpordSlot {
    char  type[24];                                   // OPORD type token, '\0' if the slot is absent
    bool  loop;                                       // loop= (Patrol); false if absent
    int   waypointCount;                              // 0..kMissionFitMaxWaypoints
    MissionFitWaypoint waypoints[kMissionFitMaxWaypoints];
    int   escortTargetIndex;                          // Escort: warrior index of escortee, -1 if absent
};

// Tri-state ints below: -1 = field ABSENT, else 0/1. Floats: < 0 = ABSENT.
struct MissionFitBrain {
    int   warriorIndex;                               // N from [WarriorN]
    char  archetype[48];                              // e.g. "Archetype.InnerSphere.Standard", '\0' absent
    char  compatibilityMode[16];                      // "Legacy"/"Hybrid"/"Enhanced", '\0' absent
    float posX;                                       // spawn X from the matching [PartN] block, <= -1e8 if absent (coords may be negative)
    float posY;                                       // spawn Y from the matching [PartN] block, <= -1e8 if absent

    // Brain switches (the set the carver corpus actually uses).
    float engageRadius;                               // EngageRadius, <0 absent
    float attackerHelpRadius;                         // AttackerHelpRadius, <0 absent
    float defenderHelpRadius;                         // DefenderHelpRadius, <0 absent
    int   requestHelp;                                // RequestHelp, tri-state
    int   returnToPost;                               // ReturnToPost, tri-state
    int   wakeOnAttack;                               // WakeOnAttack (Sentry), tri-state
    int   poweredDown;                                // PoweredDown (Sentry), tri-state

    // OPORD slots.
    MissionFitOpordSlot primary;
    MissionFitOpordSlot secondary;
    MissionFitOpordSlot tertiary;

    // Weighted tactics.
    int   tacticCount;                                // 0..kMissionFitMaxTactics
    char  tacticName[kMissionFitMaxTactics][24];
    float tacticWeight[kMissionFitMaxTactics];
};

// Parse all [WarriorN] inline Brain{} blocks. Writes one MissionFitBrain per warrior that has a
// Brain{} block, into out[0..maxOut). Absent fields take the sentinels above (so the consumer can
// tell "set to false" from "not present"). Returns the number of records written (>=0).
int parseMissionFitBrains(const char* text, MissionFitBrain* out, int maxOut);

// ---------------------------------------------------------------------------------------------
// BRAIN-PATROL-VIEWER-BRIDGE-1: pure position-match, shared by the editor's declarative-brain
// display (editor/MissionFitBrainSource.cpp, editor/UnitBrainPanel.cpp) and the patrol-waypoint
// importer that seeds a Unit's editable/drawable waypoint list (Unit::importPatrolFromBrainIfNeeded,
// editor/EditorObjects.cpp) -- both consult the SAME match so the panel never shows a brain that a
// different tolerance/tie-break would have imported. Kept here (not in editor/) so it is engine-free
// and directly unit-testable (tools/brain_missionfit_oporbd_test), same as parseMissionFitBrains.
//
// Nearest brain (by its [PartN] spawn position) to (ux,uy), within `tol` world units. Ties/multiple
// candidates within tolerance: nearest wins (strict <=, first-seen wins on an exact tie, matching
// the position-match semantics this bridges). A brain with no spawn position (posX <= -1e8, i.e.
// no matching [PartN] block) never matches. Returns nullptr if none qualify.
const MissionFitBrain* matchBrainForPos(const MissionFitBrain* brains, int brainCount,
                                         float ux, float uy, float tol = 50.0f);
