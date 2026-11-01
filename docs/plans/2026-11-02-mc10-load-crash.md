# mc10 load crash (lane8, claude/mc10-load-crash)

Crash: W95 exe e5cf6ba9c043, mc2_10 via logistics LAUNCH, READ at 0x0, RVA 0x1f13ac
(virtual returning null, then vtable call on it, inside a loop over an object list).
Last trace line: mission_init.cpp:652 VERIFY commandersToLoad[2] uninitialized commander 2 / team 2.

Root cause (source analysis; exact line not symbolized, install PDB is stale):
logistics builds commandersToLoad as identity {i,i,i}; mc2_10 has fewer than 3 teams, so
the entry names a null Team. The VERIFY skipped setTeam, leaving commander 2 with no team.
Quickstart uses {2,0,2} so it never hits this. Later code dereferenced Commander::team.

Fix: Mission::initTeamsAndCommanders falls back to Team::home for the skipped commander;
null-checks added in artlry.cpp, missiongui_style.cpp, missiongui_support.cpp.
Test: scripts/tests/test_mission_init_team_fallback.py (source-contract guard).
Not reproduced end to end through the logistics UI (headless); fix is by analysis.
lrm20/clanlrm20 shape lines: guarded log-only paths; content issue if a mod exposes the weapon without the .ase.
