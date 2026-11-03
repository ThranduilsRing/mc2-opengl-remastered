<!-- doc-status: current | reviewed:2026-11-03 -->
# Autoplay mission bot (level 1, in-engine)

Owner backlog item 2026-10-27: an on-demand bot that plays a mission through
its objectives, for stock and mod missions. Dev tooling only, never part of a
merge gate. **Almost all of it already exists** (`claude/player-brain-l1`,
merged); this lane's job was to confirm it, document it, and run it across
stock missions and one MCO mission. No new engine gates were added.

## Pieces

| Piece | Where | Role |
|---|---|---|
| Player brain | `Mission::updateMissionTest` (`code/mission_update_test.cpp`), `producePlayerIntent` (`code/player_unit_intent.h`), TechBrain policy (`code/brain_execution_policy.cpp`) | picks the active objective, builds squads, issues ordinary PLAYER-origin orders through `submitPlayerOrder` |
| Gate `MC2_PLAY_MISSION` | `Gates/gate_rows_2.inc` | turns the bot on (default OFF) |
| Gate `MC2_PLAY_MISSION_INVULNERABLE` | same | `Team::noPain[home]`: player cannot be hurt (default OFF) |
| Gate `MC2_PLAY_MISSION_SKIP_CINEMATICS` | same | `eye->forceMovieToEnd()` (default OFF) |
| Gate `MC2_PLAY_MISSION_SECONDS` / `_GOTO` | same | sim-time timeout; fixed goto point |
| Driver | `tools/play_mission.py` (via `scripts/run_smoke.py --headless`) | launches under the host lease, parses `[MISSION_OBJECTIVES v1]` and `[MISSION_VERDICT v1]` |

All gates are registered in `Gates/gate_table.cpp` rows and default OFF; with
`MC2_PLAY_MISSION` unset `updateMissionTest` returns on its first line.

## Objective-to-order mapping

Each tick (2 s cadence) the bot takes the first non-hidden, active, undetermined
objective of the home team and its first unfinished condition that has a marker.

| Condition species | Order produced |
|---|---|
| move-to-area style (marker point) | MOVETO_POINT (clamped to 800 wu per order, fast travel) |
| `DESTROY_SPECIFIC_ENEMY_UNIT` | ATTACK_OBJECT on the unit (score +1000) |
| `DESTROY_SPECIFIC_STRUCTURE` | ATTACK_OBJECT on the building at the marker, nearest building within 1 wu |
| `CAPTURE_STRUCTURE` | CAPTURE (within 400 wu), else move |
| `MOVE_ALL_(SURVIVING_)(MECHS|UNITS)_TO_AREA` | extraction: move plus kill only enemies near the area |
| anything else (timers, flags, hidden triggers) | not driven; bot keeps escorting toward the marker |

Per squad (4 units) the best contact on the route is attacked (threat/range/health
score). Retreat on low health, repair truck, salvage and airstrike purchases are
all in `producePlayerIntent` / `updateMissionTest`.

## Verdict and enemy checks

Log lines (stderr, always on while the gate is on):

- `[MISSION_OBJECTIVES v1] id= state=PASS|FAIL|PENDING primary= hidden=` on every state change
- `[MISSION_ENEMIES v1] event=spawn|react_move|damage|disabled_or_dead|destroyed wid=`
- `[MISSION_VERDICT v1] verdict=PASS|FAIL|TIMEOUT ... enemies= reacted= damaged= destroyed= losses= survivors=`

`tools/play_mission.py` prints the per-objective table and the verdict;
`--invulnerable` tests objective completion rather than brain skill;
`--mod NAME:DEPS:BOOT` runs a mod mission without editing `smoke_missions.txt`.
The enemy-check summary column (spawn/react/damage/die) is the verdict line.

## Usage

```bash
py -3 tools/play_mission.py --mission mc2_01 --install E:/mc2-installs/lane7 --minutes 5 --invulnerable
py -3 tools/play_mission.py --batch mc2_01 mc2_02 mc2_03 --install E:/mc2-installs/lane7
py -3 tools/play_mission.py --mission <mcomission> --mod MechCommanderOmnitech::campaign --install ...
```

## Out of scope (skipped by design)

Escort timing, don't-destroy, stealth, scripted ABL-timer objectives: the bot
does not drive them; they surface as PENDING/TIMEOUT.

## Results (2026-11-03, invulnerable, 5 min budget, lane7 install)

| Mission | Verdict | Objectives | Enemies reacted / damaged / destroyed |
|---|---|---|---|
| mc2_01 | PASS (202 s) | 6/6 primary | 26 / 20 / 17 of 26 |
| mc2_02 | TIMEOUT | 5 pass, 1 pending | 33 / 28 / 23 of 42 |
| mc2_04 | TIMEOUT | 2 pass, 2 pending | 41 / 34 / 29 of 68 |
| mc2_05 | TIMEOUT | 1 pass, 5 pending | 17 / 10 / 10 of 41 |
| MCO a_0_1 | ERROR | engine STOP "Unable to open Mission File" on load | n/a |

TIMEOUT means no failed objective, only unfinished ones at the time budget; a longer `--minutes` may finish them.
The MCO load failure is a `--mod` / mission-path argument problem (tried `MechCommanderOmnitech::campaign` and
`MechCommanderOmnitech:mco-compat:campaign`); not diagnosed further.

## Dev gates (documented here; registered in Gates/gate_rows_2.inc)

`MC2_PLAY_MISSION` (enable bot), `MC2_PLAY_MISSION_INVULNERABLE` (player side takes no damage),
`MC2_PLAY_MISSION_SKIP_CINEMATICS` (force movies to end), `MC2_PLAY_MISSION_GOTO`. All default OFF.
