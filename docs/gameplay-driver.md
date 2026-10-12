<!-- doc-status: current | reviewed:2026-10-11 -->
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
py -3 scripts/game_auto.py --help
# game_auto.py is the driver client. tools/scenario_runner.py and its
# tools/scenarios/*.json were retired 2026-10-12, cull wave 1 (archived on
# E:/mc2-archive-2026-10/docs/cull-wave1-2-2026-10-12.tar.gz).
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
freeze_movers on 900        arm; engages at MISSION frame 900
freeze_movers off           thaw
freeze_movers status        active / armedAt / engagedAt / missionFrame / absFrame
```

Or without the channel at all: `MC2_FREEZE_MOVERS_AT=900`.

**`<frame>` is MISSION-relative — frames since `Mission::init`, not the
process-lifetime counter.** `g_mc2FrameCounter` starts at 0 once per process and
is never reset per mission, so arming against it directly would make `900` mean a
different instant in mission B than in mission A and would let a freeze armed in
one mission fire in the next. `Mission::init` re-stamps the baseline and disarms
any leftover freeze. Note this is a *different clock* from the one `at_frame`
and `mission_status frame=` use (both process-lifetime); `mission_status` reports
`missionFrame=` alongside so you can see both. Do not compare them.

**Always assert the latch fired** — the ack proves the command was accepted, not
that the freeze happened:

```json
{ "at_frame": 900, "cmd": "freeze_movers on 900", "expect": "ok" },
{ "assert_mission": { "frozen": 1 } }
```

Use `on <frame>`, not bare `on`. Bare `on` freezes whenever the driver's write
landed, which is not the same instant twice and gives you nothing across a
rebuild.

**It freezes the whole simulation tick, not literally "only movers".** The verb
keeps its name, but it suspends every system the pause path suspends —
`scenarioTime`, `mcTextureManager::update`, `SensorManager::update`,
`updateCollisions`, and the mission ABL brain — while rendering continues. That
is deliberate and it is not over-reach: a half-frozen sim is worse than either
extreme. In particular `mcTextureManager::update`'s own comment says it must not
run while paused because it "uncaches things which only `objectManager->update`
can cache back in", and a freeze skips exactly that call — so leaving the texture
manager live would *manufacture* missing/black-texture artifacts, which is the
defect class this harness exists to reproduce. Live sensors and a live ABL brain
would likewise keep mutating `drawFlags` and issuing orders between two arms
that are supposed to be identical.

**Unlocks — the mover-frozen vehicle-vs-mech lighting comparison**
(the retired tools/scenarios/lighting_pair.json). This is the comparison that previously
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

**Only `day`/`noon` and `night`/`midnight` are guaranteed to be opposite.**
`Camera::update` derives `isNight` from sun *altitude* against
`NIGHT_LIGHT_PITCH = 10°` (`mclib/camera.cpp:149-150`), where
`altitude = pitch` and `180 - pitch` past 90°. The phase table maps
`pitch = fraction * 360`, so:

| phase | pitch | altitude | `isNight` |
|---|---|---|---|
| `dawn` | 0° | 0° | **1** |
| `morning` | 45° | 45° | 0 |
| `day` / `noon` | 90° | 90° | 0 |
| `afternoon` | 135° | 45° | 0 |
| `dusk` / `sunset` | 180° | 0° | **1** |
| `night` / `midnight` | 270° | −90° | **1** |

`dawn`, `dusk` and `sunset` put the sun *on the horizon*, which is below the
engine's night threshold — they ack `isNight=1`. That is the engine being
consistent, not a bug, but `{"assert_light": {"is_night": false}}` after
`set_light dusk` **will fail**. For those phases assert on `light_pitch`
instead. `dawn` and `dusk` also share an altitude, so they differ only in the
colour recompute, not in sun direction. Only the `day`↔`night` pair was
measured live (by the GAMEPLAY-DRIVER-2 lane); the rest of this table is
derived from the threshold arithmetic and has not been run.

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
(the retired tools/scenarios/animated_building.json). `code/terrobj.cpp`'s own comment
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

**Order `freeze_movers` BEFORE `animate`, and know what you get.** Verified by
reading `GameObjectManager::updateAppearancesOnly` (`code/objmgr_misc.cpp:497`) and
`BldgAppearance::update` (`mclib/bdactor.cpp:3873`):

- Under freeze, `Building::update()` is **not** called at all — only
  `appearance->update(false)`. That matters twice over:
  - `Building::updateAnimations()` (`code/bldng.cpp:691-704`) re-issues
    `setGesture(0)` every frame once the object is out of transition. Frozen,
    that stomp **cannot** run, so a gesture set by `animate` sticks. Unfrozen,
    it will revert to idle at the end of the animation cycle.
  - `update(false)` skips the `if (animate && bdFrameRate != 0.0f)` frame
    advance, so the animation **holds at its first frame** instead of playing.
- So `freeze_movers` then `animate` gives a *deterministic* frame: the object is
  off the static batch and on the animated path, posed at animation frame 0,
  identically on every run and every build. That is what you want for an A/B.
- If you need a *later* animation frame, `animate` first, let it run, then
  freeze — but that pose depends on how many frames elapsed and is **not**
  reproducible across builds. Do not use it for a byte-diff.
- The terrain-object branch is gated on `recalcBounds()`, i.e. **in view**. A
  building the camera is not looking at is not updated; frame the target with
  `camera` before asserting on it.

## Runbook — the five verbs that read RENDERER input (RENDERER-INPUT-READBACK-1)

Every verb above this section reads **game** state: position, hp, gesture, the
cull verdict. None of them could read a **renderer input** — not the light row a
draw used, not what the shadow pass submitted for a building, not the pixel at a
world point, not the terrain under an object. So a wrong-input defect could only
be graded by whole-frame `mean_luma`: the instrument class that produced the
retracted "+14.8 luma" result, where two arms differed by a turret barrel angle
rather than by lighting.

These five close that gap. The organising question for each is **"which renderer
input would, if readable, make this defect a one-line assert?"**

### Preconditions

```
MC2_UI_COMMAND_FILE=<path>        the channel itself (already allowlisted in run_smoke.py)
MC2_SMOKE_FIXED_TIMESTEP=1        for any cross-build claim
```

**No new env gate.** All five are read-only, and they are already behind
`MC2_UI_COMMAND_FILE`. A second gate in front of a read-only verb inside an
already-gated channel buys nothing and adds one more documented-vs-actual
default to drift out of truth. Nothing to add to the `run_smoke.py` passthrough
list; check-gate-default-agreement.py (retired 2026-10-12, cull wave 1) was unaffected (verified: `DISAGREE 0`,
`CONTRADICT 0`).

### Reading a `-1`

`recipeDrift`, `shadowDrift` and every `*AboveGround` field print `-1` or `n/a`
for **"not computable"**, never for "zero". A `0.0000` drift is a measurement; a
`-1` is the absence of one. Do not write an assert that treats them alike.

---

### `light_row <partId>` — which lights this object's draw was handed

```
list buildings                 (or `list all` for movers) -> pick an id
light_row 32771
```

```
  src: class=building(0x10) feed=worldLights srcListCount=1024 gathered=3
  [0] type=5(TERRAIN) up=(0.00,0.00,0.00) src=(0.00,0.00,0.00) atOrigin=0 dir=(...) color=(...) falloff=(...)
  [1] type=3(POINT)   up=(0.00,0.00,0.00) src=(-4218.73,-5031.72,42.10) atOrigin=1 ...
  baked: recipe=42 lightDataIndex=17 slots=200 numLights=3 firstType=5 firstColor=(...)
```

Runs the **same** `GatherLightsParameters()` the draw path calls
(`mclib/txmmgr_render_phases.cpp:220`), against the **same** list this object's appearance
class passes to `SetLightList`. It is not a reimplementation — a second copy of
that arithmetic could agree with itself while disagreeing with the shipped draw.

**Unlocks — "92.7% of lamps sit at the world origin".** That figure came from an
aggregate census. `atOrigin` answers it per light, per object, from the bytes
that would be uploaded: `up=` is `lightToWorld[12..14]`, the only slots any
consumer reads as `m[3].xyz`; `src=` is `[3]/[7]/[11]`, where `BuildTranslation`
actually leaves the translation.

```json
{ "cmd": "light_row 32771", "expect": "ok" }
```
assert: **no line has `atOrigin=1`.** A single `atOrigin=1` on a `POINT`,
`SPOT` or `INFINITEWITHFALLOFF` light is the origin defect, on that object, now.

**Unlocks — "vehicles and mechs are not lit the same".** Spawn the pair
(`spawn clone` / `spawn vehicle`), `freeze_movers on <frame>`, then `light_row`
each. Both classes pass `eye->getWorldLights()`, so **if the two rows are
byte-identical the lighting inputs are identical and the difference is
elsewhere** — material, normals, or shader. That is a one-command elimination of
an entire hypothesis that previously needed a capture pair.

**Unlocks — "trees carry no directional light".** Check `feed=` first.
`feed=NULL` is a real state, not a parse failure:
`GenericAppearance::SetLightList(NULL, 0)` (`mclib/genactor.cpp:1238`) hands the
gather nothing, so every object on that class gathers **zero** lights on this
path. If a tree reports `feed=worldLights gathered=N` with `N > 0`, the "no
directional light" theory is about the *bake*, not the feed — which is what the
`baked:` line is for: it is the row actually uploaded and still being replayed
for a registered prop, read back through `MC_TextureManager::copyLightSlot`.
`gathered` disagreeing with `baked ... numLights` is a stale bake.

> **Observer effect, stated plainly.** Because `light_row` calls the *real*
> gather, it also trips the gather's diagnostic bookkeeping:
> `MC2_OBJECT_PARITY_TRACE`'s one-shot dump, the `MC2_LIGHT_ROW_CENSUS` tallies,
> `MC2_LIGHT_CLAMP_PROBE`'s counts, and the `SPOT_DIAG v1` pack counters. **Do
> not issue `light_row` in a run whose purpose is to read those counters.** No
> render state is touched. The alternative — a private copy of the gather — was
> rejected: a confidently-wrong light row is far worse than a perturbed tally.

---

### `draw_path <partId>` — which lane, and is the recipe stale

```
draw_path 32771
```

```
  path: verdict=recipe_replay class=0x10 type=bldg_gate inView=1 staticReg=1 staticNow=1 recipe=42
  pose: obj=(-4218.730,-5031.720,395.480) screen=(...)
  recipe: shape=gate01.asb baked=(-4218.730,-5031.720,395.480) recipeDrift=0.0000
  submit: typeID=7 flags=0x00(lightsOut=0 window=0 spotlight=0) cullRecordPrevFrame=1
```

`unit <partId>` already prints `inView` and `staticRecipe`. What it cannot say is
whether that recipe is **stale**. The batcher replays the recipe's **baked**
model matrix, never the object's live transform, so `recipeDrift` is the
distance between where the sim thinks the object is and where its pixels are
being placed.

**Unlocks — the sunk-turret and black-quad hypotheses, which currently rest on
source reading alone.**

```json
{ "cmd": "draw_path 32771", "expect": "ok" }
```
assert: **`recipeDrift` < 0.01** on any object that has not moved, and
`verdict=recipe_replay` only while `staticNow=1`. A `dynamic_submit` verdict on
an object you did not animate means something knocked it off the static batch.

---

### `shadow_feed <partId>` — what the shadow pass submitted, and from what pose

```
freeze_movers on 900
animate 32771 1
shadow_feed 32771
```

```
  feed: registryQueried=1 recipe=42 recipeResolved=1 buildingFeed=311 dynamicFeed=2044 modernShadowPath=1
  staticBldgPass: drew=1 types=12 instances=311 draws=12
  building: present=1 sameTypeLeaves=4 exactLeaves=1 submitted=(-4218.730,-5031.720,395.480) live=(-4218.730,-5031.720,412.900) shadowDrift=17.4200
  dynamic:  present=0 sameTypeLeaves=0 exactLeaves=0 submitted=(0.000,0.000,0.000) live=(...) shadowDrift=-1.0000
```

**Unlocks — the leading black-quad hypothesis becomes a one-line assert.** That
hypothesis is that an animating building casts from its frozen mission-load
pose. The mechanism is now readable: the static building shadow map is built
**once per mission** (`mclib/txmmgr.cpp:3090-3114`) from the registry's **baked**
matrices, and the per-frame dynamic prop caster feed (`:3283`) reads those same
baked matrices. Neither reads the live transform.

```json
{ "cmd": "list buildings",  "expect": "ok" },
{ "assert_building": { "min_animatable": 1 } },
{ "cmd": "shadow_feed 32771", "expect": "ok" },
{ "cmd": "animate 32771 1",   "expect": "ok" },
{ "cmd": "shadow_feed 32771", "expect": "ok" }
```
assert: **`shadowDrift` is unchanged and ≈ 0 across the `animate`.** A drift that
appears only after the animate is the frozen-pose cast, measured rather than
argued.

`staticBldgPass: drew=` is printed beside membership on purpose. It comes from
`gos_GetStaticBuildingShadowCounts`, which is documented zero unless
`MC2_STATIC_PROP_BUILDING_SHADOW` actually drew this mission — **membership in a
feed that never drew is not a shadow**, and reporting one without the other
would be a lying instrument.

`sameTypeLeaves > 0` with `exactLeaves = 0` is a distinct and interesting answer:
the type is being fed, but nothing is fed at this recipe's location. `shadowDrift`
stays `-1` there by design — differencing against an unrelated sibling instance
of the same building type would emit a large number that reads exactly like the
stale-pose signal.

---

### `ground <partId>` / `ground <x> <y>` — is it actually sunk

```
ground 32771
```

```
  at=(-4218.730,-5031.720) terrainZ=395.4800 waterZ=0.0000 terrainType=3 normal=(...) terrainLight=...
  obj: id=32771 z=395.4800 objAboveGround=0.0000
  recipe: idx=42 baked=(-4218.730,-5031.720,378.100) recipeTerrainZ=395.4800 recipeAboveGround=-17.3800 recipeVsObj=-17.3800
```

Three numbers, three different ways to be wrong: the **sim** pose vs terrain
(`objAboveGround`), the **render** pose vs terrain (`recipeAboveGround`), and
render vs sim (`recipeVsObj`).

**Unlocks — "sunk turret", settled objectively instead of by eye.**

```json
{ "cmd": "ground 32771", "expect": "ok" }
```
assert: **`recipeAboveGround` ≥ -0.5.** A large negative value is the object
being *drawn* below the terrain. If `objAboveGround` is fine while
`recipeAboveGround` is not, the sim is right and the **baked render pose** is
wrong — which points at the recipe, not at placement, and is the single most
useful discrimination this verb makes.

---

### `pixel <x> <y> [z]` / `pixel screen <px> <py>` — the texel at a world point

```
pixel -4218 -5031
pixel screen 320 240
```

```
  world=(-4218.000,-5031.000,395.480) screenYDown=(612.4,377.1) w=0.0021 behindCamera=0 offScreen=0 screenRes=(1280,720)
  fbo=1280x720 texelYUp=(612,342) inBounds=1 rgba=(0,0,0,255) luma=0.000 srcFrame=899
```

**Unlocks — "there is a black quad there" becomes a number.**

```json
{ "cmd": "pixel -4218 -5031", "expect": "ok" }
```
assert: **`luma` > 2.0** at a world point known to be lit terrain. `rgba=(0,0,0,*)`
on a daylit surface is the black quad, located at a coordinate rather than in a
screenshot.

**Two provenance facts, both printed, both load-bearing:**

1. **`srcFrame` is CURRENT-1.** This channel is polled from
   `MissionInterfaceManager::update()`, which runs before the frame renders, so
   the scene FBO still holds the previous frame. A `screenshot` issued on the
   same line is an **end-of-frame** capture and is one frame **AHEAD**. Comparing
   them without accounting for that is the most likely way this verb could lie.
2. **`Camera::projectForScreenXY` returns Y-DOWN** screen coordinates
   (`mclib/camera.h:593-635`), while `glReadPixels` is Y-UP. Both the Y-down
   screen coordinate and the Y-up FBO texel are printed so a disagreement with a
   TGA is diagnosable rather than mysterious.

**Cross-check it once, on first use, before believing any of it.** Park the
camera, `pixel screen <px> <py>`, then `screenshot probe`, and compare that texel
in `probe.tga` against the reported `rgba` — remembering the one-frame offset.
Under `MC2_CAMERA_PARK` + `freeze_movers` the two frames are identical, so they
should match exactly. If they do not, the Y-flip or the FBO scaling is wrong and
**every `pixel` result is void until it is fixed.** This cross-check has NOT been
run — the engine cannot be launched in this environment.

---

### What is NOT measured

Nothing in this section has been exercised against a running engine. The verbs
compile, link, and are confirmed present in the linked image by a byte scan whose
discrimination was proved by injecting the real regression (`#if 0` around the
`shadow_feed` branch) and watching exactly its three probes flip to FAIL. That
proves the code is *in the binary*. It does not prove any printed number is
*correct*. The first live run should be the `pixel` cross-check above, then a
`draw_path` on a known-static building (expect `recipeDrift` ≈ 0), because both
are self-evidently falsifiable.

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
