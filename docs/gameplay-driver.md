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
`assert_light`, `assert_mission`, `assert_building`. Each reads the world back
through `list` / `list buildings` / `unit` / `light_state` / `mission_status`.

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

## Runbook — the three verbs that make A/Bs comparable

`freeze_movers`, `set_light` and `animate` exist because three specific classes
of measurement were impossible without them. Each entry below is the exact
command a lane runs and the exact comparison it unlocks.

### Preconditions (all three verbs)

```
MC2_UI_COMMAND_FILE=<path>        the channel itself
MC2_SMOKE_FIXED_TIMESTEP=1        REQUIRED for any cross-build claim
MC2_TEST_SPAWN=1                  only for spawn / teleport / animate
```

`MC2_SMOKE_FIXED_TIMESTEP=1` is not optional garnish. Without it `frameLength`
is wall-clock, so "frame 900" is a different simulated instant in every build
and a frozen pose is reproducible **within one process only**. Every gate above
is on the `run_smoke.py` passthrough allowlist; anything not on that list is
dropped by `Popen`, which silently makes both arms of an A/B run the same path.

### `freeze_movers on <frame> | off | status`

```
freeze_movers on 900        arm; engages the first frame counter >= 900
freeze_movers off           thaw
freeze_movers status        active / armedAt / engagedAt
```

Or without the channel at all: `MC2_FREEZE_MOVERS_AT=900`.

**Always assert the latch fired** — the ack proves the command was accepted, not
that the freeze happened:

```json
{ "at_frame": 900, "cmd": "freeze_movers on 900", "expect": "ok" },
{ "assert_mission": { "frozen": 1 } }
```

Use `on <frame>`, not bare `on`. Bare `on` freezes whenever the driver's write
landed, which is not the same instant twice and gives you nothing across a
rebuild.

**Unlocks — the mover-frozen vehicle-vs-mech lighting comparison**
(`tools/scenarios/lighting_pair.json`). This is the comparison that previously
produced a **retracted** "+14.8 luma brighter" result: the two arms differed by
a turret barrel resting at a different aim angle, not by lighting. Frozen movers
still draw, but `updateAppearancesOnly` never calls `setMoverParameters`, so
torso twist and turret aim hold their last value — the drift is gone by
construction. Spawn a mech and a vehicle at adjacent known coordinates, freeze at
a fixed frame, then vary **one** thing:

- `pair_day` vs `pair_night` → light varies, chassis held constant.
- `pair_day` mech vs vehicle → chassis varies, light held constant.
- `pair_day` vs `pair_night` **across two builds** → the fix varies; only valid
  because the freeze is frame-armed.

Do not read the cross pair (mech_day vs vehicle_night) — two variables.

### `set_light <phase|0..1>` / `set_light alt <deg> [yaw]`

```
set_light day | noon | dawn | morning | afternoon | dusk | sunset | night | midnight
set_light 0.75              raw daylight fraction (0.25 noon, 0.5 sunset, 0.75 midnight)
set_light alt 15            sun ALTITUDE only -- ambient/sky/fog untouched
```

Delegates to the same `Camera::setDaylightFraction` / `Camera::setSunPitch` that
`time_of_day` and `sun_pitch` use; it is a named front door, not a second
lighting mechanism. Assert on `isNight` / `lightPitch`, never `nightFactor`
(`Weather::update` raises it while raining and takes the MAX).

**Unlocks — the standing day-AND-night verdict rule becomes a fixture.**
Previously enforceable only on missions that happen to ship with the sun you
need, which is why the old night test hardcoded `mc2_10`. Now any mission:

```json
{ "cmd": "set_light day",   "expect": "ok" },
{ "assert_light": { "is_night": false } },
{ "cmd": "screenshot arm_day" },
{ "cmd": "set_light night", "expect": "ok" },
{ "assert_light": { "is_night": true } },
{ "cmd": "screenshot arm_night" }
```

**Unlocks — the sun-altitude sweep for the terrain floor.** `alt` moves the sun
*direction* while leaving ambient, sky and fog exactly as they were, which is the
only way to separate "the terrain floor responds to sun angle" from "the whole
frame got a different colour recompute". Sweep and read `mean_luma` per capture
from `report.json`:

```json
{ "cmd": "set_light alt 5",  "expect": "ok" }, { "cmd": "screenshot floor_a05" },
{ "cmd": "set_light alt 15", "expect": "ok" }, { "cmd": "screenshot floor_a15" },
{ "cmd": "set_light alt 30", "expect": "ok" }, { "cmd": "screenshot floor_a30" },
{ "cmd": "set_light alt 60", "expect": "ok" }, { "cmd": "screenshot floor_a60" },
{ "cmd": "set_light alt 85", "expect": "ok" }, { "cmd": "screenshot floor_a85" }
```

A terrain floor that is *clamped* shows a luma curve that flattens at low
altitude while `lightPitch` keeps moving — that divergence is the measurement.
Freeze movers first, or movers in frame add their own variation to every sample.

### `animate <partId> <gestureId|damaged|normal>` (needs `MC2_TEST_SPAWN=1`)

```
list buildings              ungated; ids + which gestures have animation data
animate 32771 1             setGesture -> explicit invalidateStaticRegistration
animate 32771 damaged       setObjStatus -> bldgDmgShape swap (different route)
animate 32771 normal        back to the undamaged shape
```

Building part ids are **position-derived**
(`MIN_TERRAIN_PART_ID + vertex*width + block`), not sequential — always resolve
them from `list buildings` at runtime. Requesting a gesture with no animation
data is acked `blocked` rather than silently doing nothing, because `setGesture`
would call `ClearAnimation()` and leave the object on the static path, which
looks exactly like success.

**Unlocks — the animated-building black-quad reproduction**
(`tools/scenarios/animated_building.json`). `code/terrobj.cpp`'s own comment
says its `animStartInvalidated` counter "may be zero in smoke missions if no
registered-static building animates" — an admission that the render path the
user's black quads *and* sunk turrets both live on had never been exercised,
because a passive fly-through never makes a building animate.

Assert the object actually left the static batch; the ack reports intent, the
readback reports the world:

```json
{ "cmd": "list buildings", "expect": "ok" },
{ "assert_building": { "min_animatable": 1 } },
{ "cmd": "animate 32771 1", "expect": "ok" },
{ "assert_building": { "id": 32771, "static_now": 0 } },
{ "assert_building": { "min_anim_invalidated": 1 } }
```

The two sub-verbs leave the static batch by **different** routes and should both
be captured: `setGesture` invalidates the registration explicitly, while
`damaged` swaps in `bldgDmgShape` and does *not* — the stale
`staticReg.shape != bldgShape` is dropped a frame **later** from `render()`. That
one-frame window is itself a prime black-quad suspect, so capture immediately
after the damage step, not only once settled.

If `assert_building: {min_animatable: 1}` fails, that is a **finding**, not a
setup error: the mission cannot witness this defect class, and the honest result
is "cannot reproduce under harness" rather than arms that prove nothing.

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
