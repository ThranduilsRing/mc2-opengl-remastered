# Gameplay driver — driving a running mission from an agent

`MC2_UI_COMMAND_FILE` turns a running mission into something an agent can drive:
point it at a text file, write one verb per line, read the acks back. The full
verb list and every env-gate detail live in
[`docs/tier1_env_vars.md`](tier1_env_vars.md) under *Gameplay command channel*.
This page is the how-to.

The engine side is `MissionInterfaceManager::processUiCommandChannel()`
(`code/missiongui.cpp`) — one `if/else-if` chain over a `std::string`. Adding a
verb is one `else if` plus a `RendererFeatureRegistry.h` entry.

## Quick start

```bash
py -3 tools/scenario_runner.py tools/scenarios/day_night_ab.json \
    --install A:/Games/mc2-opengl/mc2-win64-v0.3 \
    --out .claude/captures/gameplay-driver/day_night_ab
```

The runner boots the engine headless, waits for mission-live, feeds the
scenario's steps, captures screenshots, **asserts against real world state**, and
writes `report.json`. It kills only the PID it spawned and refuses to run if an
`mc2.exe` is already live from the target install.

## The five things that bite you

**1. The channel is mission-only.** Nothing outside the mission loop polls it.
The first ack doubles as the "mission is live" probe. The AAR / logistics /
mechbay screens are unreachable from the channel — `end_mission win` will start
the real termination flow, but once logistics takes over the channel is dead.

**2. `ok` describes the COMMAND, not the WORLD.** `spawn ... -> ok` and
`order move ... -> ok` are both true statements about what the engine accepted.
The 2026-07-23 prototype run reported `ok=True` for a scenario in which every
spawned unit was inert and had ignored every order. Always follow a
world-changing verb with an assertion:

```json
{ "snapshot": "before" },
{ "cmd": "order move -4200 -5000 run", "expect": "ok", "wait": 12.0 },
{ "assert_moved": { "since": "before", "unit": 512, "min_dist": 40 } }
```

Available: `assert_moved`, `assert_pos`, `assert_hp`, `assert_count`,
`assert_light`. Each reads the world back through `list` / `unit` /
`light_state`.

**3. You need `list` before you can target anything.** `select` only reaches
home-commander units and `status` only dumps units already selected, so `list`
is the only source of an enemy `partId` for `order attack`. Ids are not stable
across missions or runs (authored units were 512-516; spawned ones came back as
2050+), so resolve them at runtime — or use `select class <NAME>` /
`select name <substr>`.

**4. For an A/B capture, pin the camera with `MC2_CAMERA_PARK`, not `camera`.**
The `camera` verb uses the swoopy goal-seek and the scripted camera keeps writing
the pose afterwards. An unsettled pair measured *midnight brighter than noon*
purely because the two arms framed different terrain. `camera_park` hard-sets the
pose once and freezes it, so every arm shares one frame.

**5. `--duration` is WALL-CLOCK, not sim seconds.** `gos_smoke.cpp shouldQuit()`
compares against `SDL_GetPerformanceCounter`.

## Day / night verdict in one scenario

This is the capability the channel previously lacked entirely — the only way to
see night light was to pick a mission that ships as night.

```json
"camera_park": "-6656,-6656,150.625,10.9094,6045.2358@600",
"steps": [
  { "at_frame": 700 },
  { "cmd": "time_of_day 0.25", "expect": "ok", "wait": 4.0 },
  { "assert_light": { "is_night": false, "pitch_min": 80, "pitch_max": 100 } },
  { "cmd": "time_of_day 0.75", "expect": "ok", "wait": 4.0 },
  { "assert_light": { "is_night": true } }
],
"captures": [
  { "name": "dn_noon",     "after_step": 1 },
  { "name": "dn_midnight", "after_step": 3 }
]
```

The runner records each capture's `mean_luma` in `report.json`, so a lighting
change can be shown to have reached the pixels rather than just the ack.

Assert on `isNight` / `lightPitch`, **not** `nightFactor` — `Weather::update`
raises `nightFactor` while it is raining and takes the MAX, so a rainy day
reports `nightFactor ≈ 0.8` with `isNight = 0`.

## Reading back which draw path an object took

```
unit 512
  id=512 name=Zeus class=BattleMech pos=(-4218.73,-5031.72,395.48) hp=1.000 ...
  mover: team=0 cmdr=0 gui=1 selected=1 typeHandle=58 pilot=Bubba
  draw: appearType=zeus appearNum=218103808 inView=0 staticRecipe=-1 screenPos=(...)
```

- `inView` — the cull verdict for this frame.
- `staticRecipe` — `>= 0` means the object went through the StaticSceneProxy /
  GPU-direct static recipe path; `-1` means the per-frame dynamic walk.
- `screenPos` — post-transform; `w <= 0` means behind the camera.

## Mech vs vehicle under identical light

`spawn vehicle` clones an existing `GROUNDVEHICLE` (searching the player roster
first, then the whole mission) so a mech and a vehicle can be posed side by side
and captured in one frame:

```json
{ "cmd": "spawn clone   -4750 -5250", "expect": "ok" },
{ "cmd": "spawn vehicle -4650 -5250", "expect": "ok" }
```

Caveat, and it is a real one: a spawned **mech** comes up with a broken status
rating (`-nan(ind)` on three of four measured runs, `1.000` on the fourth) and
should not be treated as a functional gameplay actor. A spawned **vehicle** was
healthy in the same runs. Both render and both join the GUI roster, so they are
fine as lighting/render subjects. Requires `MC2_TEST_SPAWN=1`.

## Every launch must carry the windowed env

The runner sets these for you; set them yourself if you drive the channel
directly:

```
MC2_WINDOWED=1 MC2_WIDTH=640 MC2_HEIGHT=480 MC2_NO_CURSOR_GRAB=1
```

`MC2_WINDOWED` alone opens an `options.cfg`-sized window covering the whole
desktop, and `STARTUPINFO`/`wShowWindow` does not help because SDL calls
`ShowWindow(SW_SHOW)` explicitly.

## Adding a verb

1. One `else if` in `processUiCommandChannel()` (`code/missiongui.cpp`). Write
   any multi-line readback into the existing `extra` string; it is appended after
   the ack line.
2. Update the `MC2_UI_COMMAND_FILE` entry in
   `RenderCore/RendererFeatureRegistry.h` and the section in
   `docs/tier1_env_vars.md`.
3. If the verb needs a new env gate, add it to the registry **and** to the
   `scripts/run_smoke.py` passthrough allowlist — `Popen` drops anything not on
   that list, so a gate-ON smoke silently runs gate-OFF.
4. Route through the dispatcher the mouse/keyboard uses (`handleOrders`,
   `handleTacticalOrder`, `Mover::setSelected`) so game rules still apply.
