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

## Results (2026-11-03, invulnerable, lane7 install)

| Mission | Budget | Verdict | Objectives | Enemies reacted / damaged / destroyed |
|---|---|---|---|---|
| mc2_01 | 5 min | PASS (202 s) | 6/6 primary | 26 / 20 / 17 of 26 |
| mc2_02 | 15 min | TIMEOUT | 5 pass, 1 pending (50011, primary, visible, at -2752,2837) | 33 / 28 / 23 of 42 |
| mc2_04 | 15 min | TIMEOUT | 3 pass, 1 primary pending (50021) + hidden fit:5 | 54 / 47 / 36 of 68 |
| mc2_05 | 15 min | TIMEOUT | 1 pass, 5 pending (50023-50027, visible) | 17 / 10 / 9 of 41 |
| MCO a_0_1 | 5 min | TIMEOUT | 1 pass, rest pending (fit:3 hidden) | 7 / 4 / 0 of 11 |
| MCO a_0_2 | 10 min | TIMEOUT | 0 pass, 7 pending | 8 / 8 / 1 of 9 |

TIMEOUT means no failed objective, only unfinished ones at the budget.

Why the rest stay pending (15 min does not help): the pending stock objectives are visible and non-timer, so the
bot is the limit, not the clock. Over a whole run the bot keeps one fixed goal target (mc2_02 id 173, mc2_05
id 166), its orders show `rejected>0` or issue with no path (`pgoal=0 pstep=0/0`), most home units sit idle and
the target stays at full hp. Pending hidden objectives (fit:N, no position) are scripted triggers the bot never
drives. Not fixed: needs a retarget/unstick pass in `producePlayerIntent`, larger than a small driver addition.

## MCO load failure (fixed)

`Unable to open Mission File` came from a stale `mods/<mod>/.modindex-cache` copied by deploy from another
install: it matched the file-count + mtime signature but held that install's absolute paths, so every mod
`_open` failed silently. `LoadModIndexCache` (mclib/file_mod.inc) now rejects a cache whose first path is not
under the current data dir and rebuilds it. `--mod NAME:DEPS:BOOT` arguments were correct.

## Dev gates (documented here; registered in Gates/gate_rows_2.inc)

`MC2_PLAY_MISSION` (enable bot), `MC2_PLAY_MISSION_INVULNERABLE` (player side takes no damage),
`MC2_PLAY_MISSION_SKIP_CINEMATICS` (force movies to end), `MC2_PLAY_MISSION_GOTO`. All default OFF.
