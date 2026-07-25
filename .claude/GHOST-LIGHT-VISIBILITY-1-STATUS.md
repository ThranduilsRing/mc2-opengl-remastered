# GHOST-LIGHT-VISIBILITY-1 — status

Lane: GHOST-LIGHT-VISIBILITY-1 · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
Deploy/test lane: `A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1`
Verdict: **FIXED** (agent-verified, deterministic A/B; user click-through pending)

---

## User reports

1. mc2_10 @ bookmark **212019** — "the MECH MODEL IS NOT VISIBLE but ITS LIGHT
   CONE IS. If the mech is marked as not visible, it should not produce light."
2. Scope sharpen, mc2_10 @ **204753** — light cones visible on invisible
   vehicles. Rule: **building/static lamp cones may always show; MOVER
   (mech/vehicle/helo) light must NOT render when the unit is not visible to the
   player** — exposing a hidden enemy via its cone is a positional leak.

## Root causes (two, both real)

### A. Mover emission gated on a flag that is not a fog-of-war predicate

Every mover light/cone predicate read the appearance `visible` flag:

```
active = (eye->isNight && visible && !InEditor && status != DESTROYED/DISABLED/SHUTDOWN)
```

`visible` is **not** a visibility test. `code/mech.cpp:6816-6866` and
`code/gvehicl.cpp:3784-3833` call `setVisibility(true,true)` on **every** contact
branch — including `CONTACT_SENSOR_QUALITY_1..4`, where the mesh is deliberately
**not** drawn and only a sensor blip renders (`mech3d.cpp:3641`,
`gvactor.cpp:2432`). So a mech/vehicle the player held only as a blip had
`visible == true`, pooled light onto the terrain and drew a searchlight cone at
its exact position.

The in-code note from T1.13 asserting *"`visible` IS the mesh-drawn fog-of-war
predicate"* was wrong. The `(sensorLevel > 4)` gate T1.13 replaced was wrong in
the other direction (it also excluded visually-spotted enemies at sensorLevel 0).

**Correct predicate** (matches the mesh/blip split and the existing precedent at
`mech3d.cpp:4613`):

```
model is shown  iff  sensorLevel == 0 || sensorLevel >= 5
```

| sensorLevel | meaning | mesh drawn? |
|---|---|---|
| 0 | enemy at CONTACT_VISUAL (or destroyed/disabled/ShowMovers) | yes |
| 1..4 | enemy known ONLY as a sensor contact | **no** — blip only |
| >=5 | not an enemy (home/allied team) | yes |

### B. Cone glow bypassed the renderer's own staleness guard

`TG_Shape::Render` (`tgl.cpp:3155`) drops any leaf whose
`lastTurnTransformed != turn` — a leaf culled / never spawned / off-map-staged
this frame is never stamped, so the renderer rejects it.
`mc2_cone_glow::QueueConeGlow` bypassed that guard and re-rendered the cone node
straight from a possibly stale `shapeToWorld`, emitting a beam with no caster.

Applied to **movers only** (`ownerIsMover`). A building's cone leaf is stripped
from the static-prop recipe by GHOST-CONE-RECIPE-SKIP-1 and is never
individually transformed on the GPU prop path, so its staleness stamp is
meaningless — a blanket guard removed every static lamp cone (caught in A/B, see
"Iteration" below).

## Correction to DEFERRED-BATCH-8C

8C concluded the 212019 wedge was the textured `L_SpotLight` lamp-head geometry
rendered unlit ("cream"), and closed the item as RESOLVED / legitimate.
That is **wrong**. Deterministic A/B on the v0.5.1 install: `MC2_LIGHT_CONE_GLOW=0`
removes the wedge and **nothing else in frame**. The wedge is an additive **cone
glow**, not lamp geometry. (The soft vertical beams in the same shot survive
`glow=0` — those are the drawn cone geometry and are untouched by this lane.)
The 8C DO-NOT-WIDEN invariant guard on `GetIsSpotlight()` remains correct and
was not touched.

## Commits (branch `claude/nifty-mendeleev`, never pushed)

| sha | what |
|---|---|
| `ea999e8b` | cone glow follows the model draw predicate (staleness guard) + `TG_Shape::GetLastTurnTransformed()` |
| `e9f57f90` | mover lights/cones gated on `mover_model_shown(sensorLevel)` at both the `active` site and the off-screen `ownerEnabled` freshness refresh |
| `24f8daea` | scope the staleness guard to movers (`ownerIsMover`), restoring building lamp cones |

Files: `mclib/cone_glow.{h,cpp}`, `mclib/tgl.h`, `mclib/mech3d.cpp`,
`mclib/gvactor.cpp`, `scripts/check-env-registry.sh`, `scripts/run_smoke.py`.
`mclib/bdactor.cpp` (buildings) deliberately **untouched**.

## Gates (both default ON, correctness-fix convention)

| gate | default | =0 restores |
|---|---|---|
| `MC2_LIGHT_MOVER_VIS_GATE` | ON | mover lights/cones emit for sensor-blip-only enemies (the position leak) |
| `MC2_LIGHT_CONE_VIS_GATE` | ON | mover cone glow emits from a stale `shapeToWorld` (ghost cone, no caster) |

Registered in `check-env-registry.sh` (PASS, 967 vars) and added to the
`run_smoke.py` passthrough allowlist (no ENV_DROP_RISK).

## Iteration (the one wrong turn, and how it was caught)

First cut applied the staleness guard to **all** callers. A/B at 204753 showed
it also removed legitimate **building** lamp cones across the airbase
(`scratch/ghostlight/ul_LEG.png` vs `ul_FIX.png`) — a direct violation of the
user rule. Fixed by adding `ownerIsMover` (default false) so `bdactor` keeps
pre-lane behaviour. Re-verified: building cones restored.

## Verification — deterministic A/B, same exe, gates toggled

Harness: `scripts/run_visual_capture.py`, mc2_10 saved bookmarks, frame 431,
settle 30, fixed-timestep clock, 3840x2160, all captures `deterministic=true`.
Arms: **FIX2** = defaults (gates ON) · **LEG2** = `MC2_LIGHT_MOVER_VIS_GATE=0
MC2_LIGHT_CONE_VIS_GATE=0`.

| pose | changed px | verdict |
|---|---|---|
| **204753** (user-graded) | 585,840 (7.1%) | offending **cone over open pavement with no vehicle under it — GONE**; all building lamp cones, lamp posts, wall turrets, beacons preserved. Large delta is the 64-slot pool budget reshuffling to building lights once the hidden movers' pools were dropped. |
| **212019** (original report) | 5,063 (0.06%) | **ghost wedge GONE**; all six searchlight towers keep lamp heads + beams. legacy mean 147.2 → fixed 86.5 in the changed region (light removed). |
| **204907** | 3,200 (0.04%) | one floating cone removed; every building lamp cone / beacon / pool preserved. |
| **205952** | 1,905 (0.02%) | near-neutral (71.7 → 71.2); no lamp loss. |

Evidence (full-res crops, not thumbnails) in `scratch/ghostlight/`:

- `EVID_conewedge_LEG2.png` / `EVID_conewedge_FIX2.png` — **the money shot**:
  cone-on-invisible-vehicle at 204753, before/after.
- `EVID_212019_LEG2.png` / `EVID_212019_FIX2.png` — original 212019 wedge.
- `EVID_204907_LEG2.png` / `EVID_204907_FIX2.png`, `EVID_205952_*` — regression poses.
- `ul_LEG.png` / `ul_FIX.png` / `ul2_FIX2.png` — the building-cone regression and its repair.
- `crop_A.png` / `crop_B.png`, `zoom_A.png` — original `MC2_LIGHT_CONE_GLOW=0`
  attribution that disproved the batch-8C lamp-head theory.
- `diffmask_*.png` — magenta change masks.

**tier1: 5/5 PASS** on the deployed exe (`--exe .../mc2-win64-v0.4d-rc1/mc2.exe`),
Δdestroys +0 on every mission. (`DEPLOY_FINGERPRINT` hard-fail in the runner tail
is the known worktree-dirt condition — untracked `scratch/`, `debug_state/` and a
peer lane's in-progress `cone_glow.cpp` edits — not a wrong-exe condition; the
released lease names `0.4d-rc1`, my deploy.)

## Notes / open items

- **Not verified by the user yet** — click-through at 204753 + 212019 is the
  remaining gate.
- Pool-budget reshuffle at 204753 is a *side effect*, not a bug: dropping hidden
  movers' pool lights frees slots in the 64-light pool, so nearby building lights
  get packed and the base reads slightly warmer/broader. Worth a user eyeball.
- A parallel lane (**LIGHT-REALISM-1**) was editing `mclib/cone_glow.cpp` in this
  same worktree throughout. Its committed work (`45058613`) and its later
  in-tree beacon-glow WIP were **not** scooped — my commits contain no beacon
  code (verified with `git show ... | grep QueueBeaconGlow` = 0). My final build
  does compile its in-tree beacon work, which is why beacons appear in both arms
  of the same-exe A/B (a confound only across exes, not within).
- The mech `pointLight` (anubis searchlight) keeps its stricter legacy
  `sensorLevel > 4` gate — more restrictive than the new predicate, so it never
  leaks. Loosening it would *add* light and was out of scope.
