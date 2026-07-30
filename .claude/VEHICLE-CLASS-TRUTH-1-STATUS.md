# VEHICLE-CLASS-TRUTH-1 — STATUS

Truth lane. **No rendering code changed. No build. No deploy. No push.**
Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`, HEAD `639aa1bc`.
Method: offline mission-data parsing + C++/GLSL source reading only.

---

## VERDICT (read this first)

**1. The shipped fix DOES hit the class of object the user photographed.**
The mc2_10 scripted intro frames **9 `GVAppearance` ground-vehicle MOVERS and
ZERO vehicle-shaped static props.** `5c04048c` / `ccb713ae` target exactly that
class. The premise that the user's objects are static props is **wrong for the
intro**.

**2. The untracked `VEHICLE-SELFLIT-5-STATUS.md` second headline is an
overclaim.** Its finding is true *only for the north-east compound it happened
to capture*, which is a different place from the one the user named. See §3.

**3. But the fix does NOT finish the job, and its own falsifiable prediction is
arithmetically false.** Vehicles still lack `mech.vert`'s `kAmbientFloor = 0.35`
and its `clamp(...,0,1)`. On mc2_10 a vehicle's shadow-side multiplier is
**0.753 against a mech's 1.0** — the truck still reads darker than the mech
beside it. On mc2_03 **every vehicle pixel gets darker**, by 1.26×–8.2×,
contradicting the prediction written into `5c04048c`. See §5.

---

## 1. Per-class inventory of mc2_10

### Method (reproducible, no engine run)

`mc2_10.pak` packet 1 is the placed-object list: `int count`, then `count` × 40-byte
records `{int objTypeNum; float x,y,z; float rotation; int damage; int teamId;
int parentId; int pad; int pad;}` — layout read from
`code/objmgr.cpp:1289-1304`, packet index from `code/mission.cpp:4832`.
Packets are `PACKET_FILE_VERSION 0xFEEDFACE`, 4-byte seek table at byte 8, type
in the top 3 bits (`mclib/packet.h:44-49`); packet 1 is `STORAGE_TYPE_ZLIB`.
`objTypeNum` indexes `objects/object2.pak`, whose packets are FITini text; the
runtime class comes from `[ObjectClass] ObjectTypeNum` (`code/objtype.cpp:368-372`,
enum `code/dobjtype.h:31-58`) refined by `TreeData` vs `TerrainObjectData`
(`code/terrobj.cpp:412-417`). Movers come from the `.fit` `[Part*]` blocks
(`ul ObjectNumber`), same object2.pak lookup.

Parse is exact: `4 + 2629*40 == 105164 == ` unpacked packet size.

### Static objects — `mc2_10.pak` packet 1: **2629** placed objects

| runtime class | count | lighting path |
|---|---|---|
| TREE | 2056 | static prop batcher, **sun deactivated at bake** |
| TERRAINOBJECT | 389 | static prop batcher |
| BUILDING | 152 | static prop batcher |
| TURRET | 32 | static prop batcher |
| GROUNDVEHICLE | **0** | — |

**There are no movers in the object list at all** — by construction, movers live
only in the `.fit`.

### Movers — `mc2_10.fit`: **48** `[Part*]` blocks, all `Exists=1`

| runtime class | count |
|---|---|
| BATTLEMECH (`Mech3DAppearance`) | 19 |
| GROUNDVEHICLE (`GVAppearance`) | **29** |

29 GVs: 10 Centipede, 4 Ambulance, 4 APC, 3 CargoTruck, 2 Hunter, 2 Jeep,
2 LRMC, 2 TroopTransport.

### Vehicle-SHAPED static props (the "prop trucks") — 13 total

| appearance | class | count | positions |
|---|---|---|---|
| JeepProp | TERRAINOBJECT | 5 | (2795,5440) (3563,4800) (3221,5525) (3093,5483) (−2283,−3605) |
| FuelTruckProp | BUILDING | 3 | (3435,5355) (3563,5312) (−5141,448) |
| OrdnanceTruckProp | BUILDING | 1 | (2880,5568) |
| TruckProp | BUILDING | 1 | (−4715,533) |
| PrivateJet | BUILDING | 1 | (3861,5184) |
| VehicleHangar | BUILDING | 1 | (−3477,3051) |
| lifeboatrusted | TERRAINOBJECT | 1 | (−5184,1301) |

These are **static props**, not movers — confirmed independently of the
white-probe, from the mission data plus the engine's own class resolution.

---

## 2. What the mc2_10 INTRO actually is — and what it frames

**mc2_10 has a scripted intro movie.** `missions/mc2_10.abl` drives
`MoviePhase 0→13` with `SetMovieMode` / `setCameraGoalPosition` /
`playVideo("MC2_10A.bik")`, ending at `MoviePhase 12` with
`setCameraPosition(startCameraPosition); EndMovieMode`.

| phase | label | camera (x, y) |
|---|---|---|
| 0 | `Cam1` | (−1788.65, 791.10) |
| 2 | `Cam2` | (−1728.00, 2794.00) |
| 4 | `Cam3` | (−2112.46, 2837.00) |
| 6 | `Cam4` | (−1978.26, 2629.07) |
| 9 | `Cam5` | (−1947.81, 2875.53) |
| 12 | `Cam6` | back to the player lance, movie ends |

**Union of Cam2–Cam5 within 1200 wu — the intro subject:**

| class | count | which |
|---|---|---|
| **GROUNDVEHICLE movers (`GVAppearance`)** | **9** | Centipede ×4, CargoTruck ×3, Hunter ×2 |
| BATTLEMECH movers | 2 | Werewolf (d = 153–344), Zeus (d = 772–1153) |
| **vehicle-shaped STATIC PROPS** | **0** | — |

Closest to `Cam2`: `CargoTruck` at **43 wu**, `Werewolf` mech at **153 wu**
— literally "the truck is DARK, while the mech next to it is lit."
Within ~500 wu of the intro cameras there are **6–7** GV movers, matching
"like 7 vehicles in the scene" precisely.

The mission-start camera is *not* the `[Cameras]` fit position (6214, 5235):
`GameCamera::activate` (`code/gamecam.cpp:1024-1051`) snaps to the first
home-commander on-GUI mover, and `ready = active = FALSE` at `camera.h:399`
guarantees the snap runs. The intro movie then takes over from ABL.

---

## 3. Verdict on the untracked white-probe claim (`VEHICLE-SELFLIT-5-STATUS.md`)

**Half right, and the headline overclaims.** Judged by my own independent method:

- ✅ **CORRECT** where it looked. It captured poses it named `compound_*`. Those
  are the **north-east compound, centred (3340, 5123)** — which holds
  JeepProp ×4, FuelTruckProp ×2, OrdnanceTruckProp ×1, PrivateJet ×1 (8 static
  props) plus 6 Centipede GV movers, with the Atlas mech at d = 546. The
  "parked brown truck" it probed white **is** a static prop. My mission-data
  parse confirms all 8 are `BUILDING`/`TERRAINOBJECT` class. No dispute.
- ❌ **WRONG as a general claim.** Its headline — *"the objects the user is
  pointing at in the mc2_10 compounds are STATIC PROPS"* — generalises from one
  self-chosen location to the user's report. The user said **"the intro of
  mc2_10"**, which is a *different place* (−1728…−2112, 2629…2876), and there
  the vehicle count is 9 movers / 0 props.
- The doc **already admitted this gap in its own §4** ("My captures contain no
  real ground-vehicle units… it cannot yet answer…"). The headline was written
  past the evidence. It being untracked and unreviewed is exactly how that
  survived.

**Net: do not use that doc to conclude the fix misses the user's objects.**
Its `MC2_LEGACY_LIT_MC2_FRAME`-does-nothing measurement remains valid *for the
pre-fix build it ran on* — and `5c04048c` explains why (a frame correction on a
row holding zero lights is unreachable, not wrong).

---

## 4. Lighting path per class at HEAD

| | GV mover (post-fix) | Mech | Static prop | Tree |
|---|---|---|---|---|
| shader | `gos_tex_vertex_lighted.*` | `mech.vert` | `static_prop.vert` | `static_prop.vert` |
| fn | `calc_light` (6-type) | `calc_light` (6-type) | inline reduced scan | inline reduced scan |
| AMBIENT | ✅ | ✅ | ✅ | ✅ |
| INFINITE/sun | ✅ dir from row | ✅ dir from row | colour only, dir from `u_terrainSunMC2` | **none — sun deactivated at bake** |
| POINT / SPOT | ✅ | ✅ | ❌ ignored | ❌ ignored |
| `numLights==0` | **`vec3(1,1,1)`** | `base_light` = 0.35 | loop skipped → ≈0 | ≈0 |
| ambient floor | **none** | **`kAmbientFloor = 0.35`** (`mech.vert:204`) | none | none |
| output clamp | **none** (RGBA16F target) | **`clamp(0,1)`** (`mech.vert:222`) | `clamp(0,1)` | `clamp(0,1)` |
| light pools | ✅ added by `5c04048c` | ✅ | ✅ | ✅ |

Key refs: `shaders/include/lighting.hglsl:228-307` (early return `:236`),
`shaders/gos_tex_vertex_lighted.frag:66` (`c.xyz = c.xyz * lighting;`),
`shaders/mech.vert:204,214-225`, `shaders/static_prop.vert:713-735,823`,
`mclib/tgl.cpp:3508`, `mclib/gvactor.cpp:2316-2317`.

**Tree analogue does NOT extend to static-prop vehicles.** `TreeAppearance::update`
sets `eye->getWorldLight(0)->active = false` across its bake and restores it
(`mclib/bdactor.cpp:7044-7045`, `:7161`); repo-wide, `->active = false` on the
sun exists **only** there. `BldgAppearance::update` does the opposite
(`bdactor.cpp:3918` writes the terrain light *into* slot 0 and leaves it active).
The residual hazard is bake-**order** leakage, documented at `bdactor.cpp:3238-3241`
and measured (but not causally proven) by `PROP-RECIPE-BAKE-1` — see §6.

---

## 5. Night/day magnitude of the shipped change — corrected

**The brief's "night ambient is 32/255 at camera.cpp:363" does not apply to any
playable stock mission.** That line is the `readIdFloat` *fallback* taken only
when `DayToNightTime` is **absent**; all 24 `mc2_NN.fit` define it. And
`updateDaylight()` — the only consumer of `nightAmbient*` — **never runs in-game**:
`code/gamecam.cpp:1060` is `//updateDaylight(true);`, commented out. The live
ambient is the mission's authored `AmbientRed`, constant for the whole mission.
Only `set_light`/`time_of_day` via the command channel can move it.

Multiplier model, away from lamps: **M(ndl) = A + S·ndl**, `A = AmbientRed/255`,
`S = getLightRed(getTerrainLight)/255` (`camera.cpp:4392-4408`,
`gvactor.cpp:2611-2620`). Pre-fix M ≡ **exactly 1.0**.

| mission | pitch | A | S | M(ndl=0) | M(ndl=1) | vs pre-fix @ndl=0 |
|---|---|---|---|---|---|---|
| **mc2_10** (user's) | 180° | 0.753 | 0.753 | **0.753** | **1.506** | 1.33× darker / **1.5× brighter on lit faces** |
| mc2_05 | 270° | 0.165 | 0.165 | 0.165 | 0.329 | **6.07× darker** |
| mc2_23 | 9° | 0.326 | 0.373 | 0.326 | 0.698 | 3.07× darker |
| mc2_19 | 0° | 0.502 | 0.502 | 0.502 | 1.004 | 1.99× darker |
| **mc2_03** (DAY) | 46° | 0.122 | 0.675 | **0.122** | **0.796** | **8.23× darker; peak still < 1.0** |
| mc2_01 (DAY) | 50° | 0.278 | 0.886 | 0.278 | 1.165 | 3.59× darker at ndl=0 |

**Corrections to the brief:**
- "5–7× darker, multiplier 0.13–0.2" is right for **mc2_05 only**. Minimum
  across all stock maps is 0.165, not 0.13.
- The worst darkening is **not at night** — it is **mc2_03 in DAYLIGHT** (8.2×),
  because mc2_03 authors `AmbientRed = 31`. mc2_03/12/13/17/18 all do.
- On mc2_03 a vehicle's **peak** post-fix multiplier is 0.796 < 1.0, so **every
  vehicle pixel darkens at every normal**. That **falsifies** `5c04048c`'s own
  pre-registered prediction ("on the mc2_03 airfield in DAYLIGHT: vehicles
  brighten toward the mechs").
- mc2_10 is engine-**night** (`lightAltitude = 0 < NIGHT_LIGHT_PITCH 10`,
  `camera.cpp:2263`) with a bright authored ambient of 192 — a bright night raid.

**Residual risk: real, but not the 5–7× shape claimed.** It is a per-map
darkening that is worst on the low-ambient DAY maps.

---

## 6. What would ACTUALLY fix the user's complaint

**The fix is incomplete, not misdirected.** `5c04048c`'s stated goal is "the same
lighting path as everything else", but the legacy path is missing the two terms
that make `mech.vert` look lit:

```
mech.vert:204   const float kAmbientFloor = 0.35;
mech.vert:219   vec3 litRGB = calc_light(..., base /*=0.35*/);
mech.vert:222   baseLight = clamp(litRGB + highlight, 0.0, 1.0);
```

On mc2_10: mech = `clamp(0.35 + 0.753 + 0.753·ndl, 0, 1)` ≈ **1.0 everywhere**;
vehicle = `0.753 + 0.753·ndl` ≈ **0.753 on shadow faces**. So post-fix, the truck
beside the mech is **still ~25 % darker** — the user's exact complaint, surviving
the fix. On mc2_03 the same gap is 0.472 (mech) vs 0.122 (vehicle) — ~3.9×.

Candidate slice (NOT implemented here): pass a non-zero `base_light` floor into
`calc_light` from `gos_tex_vertex_lighted.vert` and clamp the result, matching
`mech.vert`. That is a shader-side change to one file, gated, needing day+night
captures on mc2_01/03/10/24 per the standing capture rule. **Verify against the
mc2_03 daylight case first — that is where the current default-ON state is worst.**

### Is this the same defect family as the black-quad / sunk-turret work? **No — adjacent, not identical.**

- **Vehicles (this lane):** movers, legacy `TG_Shape::Render`, row gathered
  **every frame**. The defect was a *missing gather*, now fixed; the residual is a
  *missing floor/clamp*. Nothing is frozen.
- **Static props (black-quad / sunk-turret / `PROP-RECIPE-BAKE-1` /
  `PROP-AMBIENT-POLARITY-1`):** a **frozen per-recipe bake**
  (`mc2CacheOrBakeStaticGpuLight`, `bdactor.cpp:3166-3230`) plus a one-shot
  per-actor `fogLightSet` latch (`bdactor.cpp:3833`), invalidated only once at
  mission-ready + 8 frames. Symptom is per-instance within-type variation
  (ambient 0.7529 vs 0.2471 on the *same* type), attributed to first-active-light
  slot collision in `decomposeFirstActiveLightColor` (`txmmgr.cpp:1355`).

They share a *root theme* — MC2's global `worldLights[]` being mutated per actor
and sampled at inconsistent times — but the vehicle path re-gathers per frame and
so cannot exhibit the frozen-recipe failure. **Fixing one will not fix the other.**
Note `PROP-AMBIENT-POLARITY-1` (untracked) claims the polarity is inverted from
`PROP-RECIPE-BAKE-1`'s reading; that disagreement is unresolved and outside this lane.

---

## 7. Evidence / reproduction

Parsers (scratch, not committed): pak seek-table + zlib reader, object2.pak class
resolver, `[Part*]` mover classifier, intro-camera proximity report. All derive
from the file:line contracts cited in §1 — re-derivable from source in one pass.

Contract harness measured this lane: **97 pass / 1 fail** (`capture_provenance`,
2 zero-byte manifests, pre-existing and correctly unrepairable) — unchanged from
the last measurement, and this lane touched no code.

## 8. What this lane did NOT do

- No rendering code changed; no build; no deploy; no capture; no push.
- Did not resolve the `PROP-RECIPE-BAKE-1` vs `PROP-AMBIENT-POLARITY-1`
  polarity disagreement.
- Did not visually confirm the intro frame — the identification is from mission
  data + the ABL camera script, not from pixels. **A single capture at
  `(-1728, 2794)` would close that**, and is the one cheap thing left.
- Did not re-measure the `MC2_LEGACY_LIT_MC2_FRAME` / `MC2_LIGHT_POS_COLUMN_FIX`
  promotions that `5c04048c` shipped default-ON alongside the gather.
- Registry hygiene noted, not fixed: `docs/tier1_env_vars.md:251` and `:401`
  still carry stale trailing "Default **OFF**" sentences that contradict their
  own new DEFAULT-ON preambles.
