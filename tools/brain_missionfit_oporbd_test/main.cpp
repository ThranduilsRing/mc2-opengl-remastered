// unit test for the pure mission.fit OPORD parser.
// Engine-free — compiles code/brain_missionfit_oporbd.cpp only. Exit 0 = pass, 1 = fail.
#include "brain_missionfit_oporbd.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what) {
    if (ok) { ++g_pass; } else { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fail; }
}
static bool feq(float a, float b) { return std::fabs(a - b) < 0.01f; }

// A realistic carver mission.fit excerpt: 3 player units, 1 patroller (loop, 2 wps), 1 guard.
static const char* kFit =
"[Warrior1]\n"
"st Brain = \"\"\n"
"Brain {\n"
"    sourceABLBrain = \"PBrain\"\n"
"    PrimaryOPORD { type = PlayerControlled }\n"
"}\n"
"[Warrior4]\n"
"Brain {\n"
"    sourceABLBrain = \"mc2_01_Pat1\"\n"
"    PrimaryOPORD {\n"
"        type = Patrol\n"
"        loop = true\n"
"        Waypoint { index=0; x=3008.000; y=1472.000 }\n"
"        Waypoint { index=1; x=3136.000; y=960.000 }\n"
"    }\n"
"    SecondaryOPORD { type = Guard }\n"
"}\n"
"[Warrior7]\n"
"Brain {\n"
"    PrimaryOPORD { type = Guard }\n"
"}\n"
"[Warrior9]\n"          // no Brain block — must be skipped
"st Profile = \"x\"\n";

int main() {
    MissionFitOpord recs[16];
    int n = parseMissionFitOpords(kFit, recs, 16);

    check(n == 3, "record count == 3 (W1, W4, W7; W9 has no Brain)");

    // Find by warrior index (order-independent).
    const MissionFitOpord* w1 = nullptr; const MissionFitOpord* w4 = nullptr; const MissionFitOpord* w7 = nullptr;
    for (int i = 0; i < n; ++i) {
        if (recs[i].warriorIndex == 1) w1 = &recs[i];
        if (recs[i].warriorIndex == 4) w4 = &recs[i];
        if (recs[i].warriorIndex == 7) w7 = &recs[i];
    }

    check(w1 && std::strcmp(w1->primaryType, "PlayerControlled") == 0, "W1 type PlayerControlled");
    check(w1 && w1->waypointCount == 0, "W1 no waypoints");

    check(w4 && std::strcmp(w4->primaryType, "Patrol") == 0, "W4 type Patrol");
    check(w4 && w4->loop == true, "W4 loop true");
    check(w4 && w4->waypointCount == 2, "W4 2 waypoints");
    check(w4 && feq(w4->waypoints[0].x, 3008.0f) && feq(w4->waypoints[0].y, 1472.0f), "W4 wp0 = (3008,1472)");
    check(w4 && feq(w4->waypoints[1].x, 3136.0f) && feq(w4->waypoints[1].y, 960.0f), "W4 wp1 = (3136,960)");

    check(w7 && std::strcmp(w7->primaryType, "Guard") == 0, "W7 type Guard");
    check(w7 && w7->loop == false, "W7 loop false (no loop= key)");
    check(w7 && w7->waypointCount == 0, "W7 no waypoints");

    // Empty / null inputs.
    check(parseMissionFitOpords("", recs, 16) == 0, "empty text → 0");
    check(parseMissionFitOpords("[Warrior1]\nst Profile = \"x\"\n", recs, 16) == 0, "no Brain → 0");

    // -----------------------------------------------------------------------------------------
    // full Brain{} block parse (real carver block + a simpler second warrior).
    static const char* kBrainFit =
    "[Warrior4]\n"
    "st Brain = \"\"\n"
    "Brain {\n"
    "    sourceABLBrain = \"mc2_01_Pat1_1\"\n"
    "    compatibilityMode = \"Enhanced\"\n"
    "    archetype = \"Archetype.InnerSphere.Standard\"\n"
    "    RequestHelp = false\n"
    "    EngageRadius = 300.0\n"
    "    AttackerHelpRadius = 100\n"
    "    DefenderHelpRadius = 125\n"
    "    ReturnToPost = true\n"
    "    PrimaryOPORD {\n"
    "        type = Patrol\n"
    "        loop = true\n"
    "        Waypoint { index=0; x=2368.000; y=2752.000 }\n"
    "        Waypoint { index=1; x=1728.000; y=2240.000 }\n"
    "    }\n"
    "    SecondaryOPORD { type = Guard }\n"
    "    Tactics {\n"
    "        Flank                     = 0.45\n"
    "        Standard                  = 0.35\n"
    "        HitAndRun                 = 0.20\n"
    "    }\n"
    "}\n"
    "[Warrior5]\n"
    "Brain {\n"
    "    archetype = \"Archetype.Clan.Standard\"\n"
    "    PrimaryOPORD { type = Sentry }\n"
    "    PoweredDown = true\n"
    "    WakeOnAttack = true\n"
    "}\n";

    MissionFitBrain brains[8];
    int bn = parseMissionFitBrains(kBrainFit, brains, 8);
    check(bn == 2, "brain record count == 2 (W4, W5)");

    const MissionFitBrain* b4 = nullptr; const MissionFitBrain* b5 = nullptr;
    for (int i = 0; i < bn; ++i) {
        if (brains[i].warriorIndex == 4) b4 = &brains[i];
        if (brains[i].warriorIndex == 5) b5 = &brains[i];
    }

    // Warrior4 — full block.
    check(b4 != nullptr, "W4 brain present");
    check(b4 && b4->warriorIndex == 4, "W4 warriorIndex == 4");
    check(b4 && std::strcmp(b4->archetype, "Archetype.InnerSphere.Standard") == 0, "W4 archetype");
    check(b4 && std::strcmp(b4->compatibilityMode, "Enhanced") == 0, "W4 compatibilityMode Enhanced");
    check(b4 && feq(b4->engageRadius, 300.0f), "W4 engageRadius 300");
    check(b4 && feq(b4->attackerHelpRadius, 100.0f), "W4 attackerHelpRadius 100");
    check(b4 && feq(b4->defenderHelpRadius, 125.0f), "W4 defenderHelpRadius 125");
    check(b4 && b4->requestHelp == 0, "W4 requestHelp == 0 (false)");
    check(b4 && b4->returnToPost == 1, "W4 returnToPost == 1 (true)");
    check(b4 && b4->wakeOnAttack == -1, "W4 wakeOnAttack == -1 (absent)");
    check(b4 && b4->poweredDown == -1, "W4 poweredDown == -1 (absent)");
    check(b4 && std::strcmp(b4->primary.type, "Patrol") == 0, "W4 primary type Patrol");
    check(b4 && b4->primary.loop == true, "W4 primary loop true");
    check(b4 && b4->primary.waypointCount == 2, "W4 primary 2 waypoints");
    check(b4 && feq(b4->primary.waypoints[0].x, 2368.0f) && feq(b4->primary.waypoints[0].y, 2752.0f), "W4 primary wp0 (2368,2752)");
    check(b4 && feq(b4->primary.waypoints[1].x, 1728.0f) && feq(b4->primary.waypoints[1].y, 2240.0f), "W4 primary wp1 (1728,2240)");
    check(b4 && std::strcmp(b4->secondary.type, "Guard") == 0, "W4 secondary type Guard");
    check(b4 && b4->secondary.waypointCount == 0, "W4 secondary no waypoints");
    check(b4 && b4->tacticCount == 3, "W4 tacticCount 3");
    check(b4 && b4->tacticCount == 3 && std::strcmp(b4->tacticName[0], "Flank") == 0 && feq(b4->tacticWeight[0], 0.45f), "W4 tactic0 Flank 0.45");
    check(b4 && b4->tacticCount == 3 && std::strcmp(b4->tacticName[1], "Standard") == 0 && feq(b4->tacticWeight[1], 0.35f), "W4 tactic1 Standard 0.35");
    check(b4 && b4->tacticCount == 3 && std::strcmp(b4->tacticName[2], "HitAndRun") == 0 && feq(b4->tacticWeight[2], 0.20f), "W4 tactic2 HitAndRun 0.20");

    // Warrior5 — Sentry-ish simpler block.
    check(b5 != nullptr, "W5 brain present");
    check(b5 && std::strcmp(b5->archetype, "Archetype.Clan.Standard") == 0, "W5 archetype Clan");
    check(b5 && std::strcmp(b5->primary.type, "Sentry") == 0, "W5 primary type Sentry");
    check(b5 && b5->poweredDown == 1, "W5 poweredDown == 1 (true)");
    check(b5 && b5->wakeOnAttack == 1, "W5 wakeOnAttack == 1 (true)");
    check(b5 && b5->requestHelp == -1, "W5 requestHelp == -1 (absent)");
    check(b5 && b5->engageRadius < 0.0f, "W5 engageRadius < 0 (absent)");

    // Null / empty robustness.
    check(parseMissionFitBrains("", brains, 8) == 0, "brain empty text -> 0");
    check(parseMissionFitBrains("[Warrior1]\nst Brain = \"\"\n", brains, 8) == 0, "brain no block -> 0");

    // -----------------------------------------------------------------------------------------
    // matchBrainForPos position-match + Primary-OPORD-Patrol-waypoint
    // proof. This IS the logic Unit::importPatrolFromBrainIfNeeded() (editor/EditorObjects.cpp)
    // runs to seed a unit's editable/drawable waypoint list, and the SAME logic the "Declarative
    // Brain" panel section (editor/UnitBrainPanel.cpp, via editor/MissionFitBrainSource.cpp) uses
    // to pick which brain to show for the selected unit -- so asserting on matchBrainForPos here,
    // headless, proves "parse OPORD -> expected waypoints == what the viewer would draw" without
    // needing Unit/EditorObjectMgr/ImGui (which this standalone target does not link).
    static const char* kPosFit =
    "[Part4]\n"
    "PositionX = 3008.000\n"
    "PositionY = 1472.000\n"
    "[Warrior4]\n"
    "Brain {\n"
    "    PrimaryOPORD {\n"
    "        type = Patrol\n"
    "        loop = true\n"
    "        Waypoint { index=0; x=2368.000; y=2752.000 }\n"
    "        Waypoint { index=1; x=1728.000; y=2240.000 }\n"
    "    }\n"
    "}\n"
    "[Part5]\n"
    "PositionX = 5000.000\n"
    "PositionY = 5000.000\n"
    "[Warrior5]\n"
    "Brain {\n"
    "    PrimaryOPORD { type = Sentry }\n"
    "}\n"
    "[Warrior6]\n"                 // Brain{} with NO matching [PartN] -> posX stays absent (-1e9)
    "Brain {\n"
    "    PrimaryOPORD { type = Patrol; loop = false; Waypoint { index=0; x=9999.0; y=9999.0 } }\n"
    "}\n";

    MissionFitBrain posBrains[8];
    int pn = parseMissionFitBrains(kPosFit, posBrains, 8);
    check(pn == 3, "pos-fit record count == 3 (W4, W5, W6)");

    const MissionFitBrain* pb4 = nullptr; const MissionFitBrain* pb5 = nullptr; const MissionFitBrain* pb6 = nullptr;
    for (int i = 0; i < pn; ++i) {
        if (posBrains[i].warriorIndex == 4) pb4 = &posBrains[i];
        if (posBrains[i].warriorIndex == 5) pb5 = &posBrains[i];
        if (posBrains[i].warriorIndex == 6) pb6 = &posBrains[i];
    }
    check(pb4 && feq(pb4->posX, 3008.0f) && feq(pb4->posY, 1472.0f), "W4 posX/posY from [Part4]");
    check(pb5 && feq(pb5->posX, 5000.0f) && feq(pb5->posY, 5000.0f), "W5 posX/posY from [Part5]");
    check(pb6 && pb6->posX <= -1e8f, "W6 posX absent (no matching [Part6])");

    // Exact position -> matches W4; its Primary Patrol waypoints are exactly what a viewer
    // seeding a unit's waypoint list from this match would draw.
    const MissionFitBrain* m = matchBrainForPos(posBrains, pn, 3008.0f, 1472.0f);
    check(m == pb4, "exact pos (3008,1472) matches W4");
    check(m && std::strcmp(m->primary.type, "Patrol") == 0, "matched W4 primary type Patrol");
    check(m && m->primary.loop == true, "matched W4 primary loop true");
    check(m && m->primary.waypointCount == 2, "matched W4 primary 2 waypoints");
    check(m && feq(m->primary.waypoints[0].x, 2368.0f) && feq(m->primary.waypoints[0].y, 2752.0f),
        "matched W4 wp0 == parsed OPORD wp0 (2368,2752)");
    check(m && feq(m->primary.waypoints[1].x, 1728.0f) && feq(m->primary.waypoints[1].y, 2240.0f),
        "matched W4 wp1 == parsed OPORD wp1 (1728,2240)");

    // Within default tolerance (50wu): dx=30,dy=0 -> dist=30 < 50 -> still W4.
    m = matchBrainForPos(posBrains, pn, 3038.0f, 1472.0f);
    check(m == pb4, "pos within tolerance (dist=30) still matches W4");

    // Outside default tolerance: dist=100 > 50 -> no match at all (W5 is far away too).
    m = matchBrainForPos(posBrains, pn, 3108.0f, 1472.0f);
    check(m == nullptr, "pos outside tolerance (dist=100) matches nothing");

    // Explicit tol override: same 100wu-away point WITH tol=150 now matches W4 (proves the
    // parameter is live, not just the 50wu default).
    m = matchBrainForPos(posBrains, pn, 3108.0f, 1472.0f, 150.0f);
    check(m == pb4, "pos outside default tol but within explicit tol=150 matches W4");

    // Nearest-wins: a point strictly closer to W5 than W4 picks W5, and W5's Primary OPORD is
    // Sentry (not Patrol) -- exactly the gate EditorImportDeclarativePatrol (editor/
    // EditorObjects.cpp) uses to decide "no declarative patrol waypoints here, fall back to the
    // legacy .abl scan": a matched-but-wrong-type brain must yield zero waypoints, not the wrong
    // brain's waypoints.
    m = matchBrainForPos(posBrains, pn, 4980.0f, 4980.0f);
    check(m == pb5, "pos near W5 matches W5, not W4");
    check(m && std::strcmp(m->primary.type, "Patrol") != 0, "W5 primary is NOT Patrol (Sentry)");
    check(m && m->primary.waypointCount == 0, "W5 primary has zero waypoints (Sentry has none)");

    // A brain with no [PartN] spawn position (W6) never matches, even sitting exactly on its own
    // (unparsed) waypoint coordinate -- the position-match invariant only trusts [PartN], never
    // falls back to guessing from OPORD waypoint content.
    m = matchBrainForPos(posBrains, pn, 9999.0f, 9999.0f);
    check(m != pb6, "W6 (no spawn pos) never matches, even at its own waypoint coords");

    // Empty / degenerate inputs.
    check(matchBrainForPos(posBrains, 0, 3008.0f, 1472.0f) == nullptr, "zero brainCount -> nullptr");
    check(matchBrainForPos(nullptr, 0, 0.0f, 0.0f) == nullptr, "null brains array + 0 count -> nullptr");

    std::printf("brain_missionfit_oporbd_test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
