# CONE-SPILL-GRADE-1 — STATUS

Adversarial grade of two lanes that landed real code and then **died without
reporting**, so neither had ever been reviewed or graded.

* **(1) `c1f98b45`** CONE-SPILL-COUPLING-1 — "centre night pools on the real
  cone/terrain intersection", `mclib/bdactor.cpp` +120/-10. Shipped **ungated**.
* **(2) `fbef958e`** EDGE-LIGHT-EMISSION-1 — "runway/beacon edge lights are
  UNLIT, not invisible". Behind `MC2_STATIC_PROP_NIGHT_TAGS`, default OFF.

Attributable exes, all `MC2_BUILD_GIT_DIRTY=0`, built in an **isolated worktree**
(`A:/Games/mc2-conegrade1`): `6823bc001562` → `7f3d0eec32b8` → `c01117626557`.
Deploy lane `mc2-win64-v0.4c`, manifest `src_commit c01117626557`.

---

## VERDICT

| item | verdict |
|---|---|
| `c1f98b45` cone/terrain spill coupling | **REGRESSIVE — now default-OFF** (`7f3d0eec`) |
| `fbef958e` stock night vertex tags | **Correct fix, real effect. Do NOT flip ON as-is** — recommend a scoped Hot-Green-only variant first |

---

## META REVIEW — premises checked against source, not status docs

| brief premise | verdict |
|---|---|
| `c1f98b45` has no gate of its own; rides default-ON `MC2_LIGHT_CONE_NODES` | **CONFIRMED.** `cone_glow.cpp:118` `return !(v && v[0]=='0')`. It was a live default-behaviour change nobody had looked at. |
| `MC2_LIGHT_CONE_NODES` is a registered gate | **FALSE.** `gate_status` → `UNREGISTERED` + `UNDOCUMENTED`, 1 reader. A default-ON gate outside the registry. |
| `MC2_STATIC_PROP_NIGHT_TAGS` returns 0 unset | **CONFIRMED** (`gos_static_prop_batcher.cpp:791`); registered, documented, smoke-allowlisted. |
| contracts 68/68 | **FALSE.** Tree baseline is **67/68** — `doc_links` (7 broken refs in live docs) already failed at parent `6772323e`. Not caused by this lane; verified by checking out both commits. |
| `tests/visual/bookmarks/*` fixtures unreliable | **CONFIRMED,** and worse than stated: the real user sets are `scratch/allposes/bm/*.json`. `mc2_03` and `mc2_24` contain only **one distinct pose each** (`saved_220422` and `saved_220422_dup` hash identically, `e61644c9f55c9750`; same for `mc2_24`). The ">=3 locations per map" standard is **not satisfiable** for those maps from user bookmarks. |

### Additional finding — shared worktree is unsafe for evidence builds

Mid-lane, `TERRAIN-SHADOW-GUARD-1` registered (2 min after me) and began live-editing
`shaders/include/shadow.hglsl` + `GameOS/gameos/gos_postprocess.cpp` in the shared
`A:/Games/mc2-main`. Both affect night shadow rendering — i.e. exactly this lane's
subject. A build taken there stamped `MC2_BUILD_GIT_DIRTY=1` and was unattributable.
**All evidence in this doc was built in an isolated worktree instead.** `mc2-main`'s
HEAD moved twice during the lane. Any lane needing an attributable exe on this branch
should assume the shared worktree is contended.

---

## (1) `c1f98b45` — REGRESSIVE, defaulted OFF

### The instrument

Pixel A/B was attempted first and is **invalid**: a noise-floor control arm
(`A2`, byte-identical env to `A`) shows mc2_10 captures are **not reproducible
across process launches**. The claimed gate effect is indistinguishable from noise:

| pose | noise A vs A2 | coupling A/B | night-tags A/C |
|---|---|---|---|
| `lance_close` | 24.3 % | 25.8 % | 26.1 % |
| `saved_204937` | 21.6 % | 21.1 % | 23.1 % |
| `saved_204753` | 10.3 % | 9.2 % | 10.0 % |

Only three poses are noise-free (`212019`, `212140`, `mc2_01/220655`), and at all
three the coupling arms are **byte-IDENTICAL** — the change does nothing visible there.

The valid instrument is the numeric diag `MC2_LIGHT_CONE_DIAG=1`, which is immune
to render nondeterminism. It was extended (`c0111762`) to report the **applied**
branch, because the pre-existing `hit=` field is computed *before* the gate and
therefore reads identically whether the coupling is on or off.

### The measurement — all 9 distinct building spotlight fixtures on mc2_10

```
DEFAULT (unset)  coupling=0  hit1=2  POOLS_KEPT=9/9
MC2_CONE_SPILL_COUPLING=1    coupling=1  hit1=2  POOLS_KEPT=2/9   <-- c1f98b45
MC2_CONE_SPILL_COUPLING=0    coupling=0  hit1=2  POOLS_KEPT=9/9
```

**`c1f98b45` deletes 7 of 9 night ground pools.** The 2 survivors move by
`horizErr = 7.0` world units — imperceptible. The `200.0f` clamp fires **0** times.

### Why it misses

The stock fixtures are authored **near-horizontal**:
`aim.z = -0.089` over `coneLen = 167.8` → only **14.9 units of drop across the
entire beam**, from an apex ~17 units above the terrain. The beam therefore never
reaches ground within its authored length. Three more fixtures point **straight up**
(`aim = (0,0,1)`, beacons) and can never hit ground at all.

The load-bearing false assumption is *"the authored beam length IS the physical
reach"* (commit message, defect 2). It is not — it is a **visual extent**. The
pre-`c1f98b45` code snapped `z` to terrain and so always produced a pool near where
the beam pointed, which is crude but is what the stock authoring expects.

Net effect at its shipped default: **7 of 9 pools deleted, in exchange for moving 2
by 7 units and removing a clamp that never fires** — a regression against the exact
complaint it was written for ("the actual lights are not on at ALL").

### Per-defect disposition (vs recon `ec928028`)

| # | defect | disposition |
|---|---|---|
| i | vertical drop instead of ray/terrain intersection | **Implemented.** Real ray march + bisection, and correct (aim is normalised in `GetNodeAim`). But worth only **7.0 world units** on the 2 fixtures where it applies. |
| ii | arbitrary `200.0f` spill clamp | **Removed — and it is a no-op.** `clamped=1` count is **0/9**; every fixture's spill is 26–168, well under 200. |
| iii | pool radius never derived from cone spread | **NOT FIXED.** Untouched by `c1f98b45`, and worse than the recon states: the pool is a **`TG_LIGHT_POINT`** with **hardcoded** `SetFalloffDistances(20.0f, 80.0f)` and `SetIntensity(0.5f)` at `bdactor.cpp:3333` — not FIT-derived at all. A radially symmetric point light can never match a cone's elliptical footprint regardless of where it is centred. |
| iv | no miss rejection / phantom pools | **Implemented, and it is the cause of the regression.** It over-rejects. Two sub-bugs: (a) the commit claims a beam "aimed at a wall" is rejected — **false**, `getTerrainElevation` is the heightfield only, buildings are not in it, so such a beam passes through and lands on terrain behind; (b) off-map uses `Terrain::IsGameSelectTerrainPosition`, which is the **playable-area** test, not an on-heightfield test (contrast `IsEditorSelectTerrainPosition`, `move.cpp:618`) — so visible border terrain is treated as off-map. |

### Disposition

Default flipped to OFF (`7f3d0eec`). The intersection path stays available under
`MC2_CONE_SPILL_COUPLING=1`. To make it shippable, the miss policy must **fall back
to the old projected point instead of suppressing**, and defect iii (point light /
hardcoded falloff) is the one that actually governs whether the pool looks like the
cone's footprint — it remains entirely unaddressed.

---

## (2) `fbef958e` — correct fix; recommend scoping before any default flip

### What is verified

* **Diagnosis is right and the mechanism is sound.** The GPU static-prop path passed
  `false, 0.0` into `get_base_light()`, so all three night-only branches took their
  daytime arm. `static_prop.vert` now evaluates `get_base_light` twice and scales only
  the **delta**, clamped — provably a no-op at scale 0 and exact stock at 1.
* **The "night ambient never darkens it" claim is TRUE.** Traced downstream:
  `lit = base_light + ambientTerm + sunColor*ndl` (`static_prop.vert:653`) — additive,
  not multiplicative.
* **Real, measurable effect** at the one noise-free night pose (`mc2_10/saved_212019`):
  `+17.1` mean luminance, **100 %** of changed pixels brighter, against a noise floor
  of **IDENTICAL** at that pose. The lights do come on.

### Why NOT to flip it ON as-is

1. **The blast radius is real and lands where it is least wanted.** The same additivity
   that protects the emission means tagged geometry is `emission + already-high night
   ambient`. Mode 2 (4× exaggeration) produced `+17.27` vs mode 1's `+17.14` — i.e.
   **no additional headroom**, which is the signature of a term already **saturating**.
   Combined with the sibling lane's measurement that night ambient reads ~196/255, that
   is the over-bright building complaint waiting to happen.
2. **The branches are separable, so the blast radius is avoidable.** `get_base_light`
   keys on three *distinct* magic colours:
   * `0xff00ff00` Hot Green — *Building Base Lights* ← **this is what every runway
     edge light and beacon uses**, and it is the user's actual complaint
   * `0xffff00ff` Hot Pink — *Lit Windows* ← the building-window blast radius
   * `0xffffff00` Hot Yellow — *Outside Building Lights*

   **Recommendation: add a scoped mode that enables only the Hot Green branch.** That
   delivers the user's fix with essentially zero blast radius and no interaction with
   the near-saturated night ambient. Flipping the current all-branch gate ON should
   wait until that is graded.
3. **Grading is incomplete and cannot be completed with the current harness** — see
   below. The previous marker-light regression shipped precisely because verification
   was range-limited, and I cannot presently rule that out at overview altitude.

### Hygiene note

`fbef958e` committed `code/gamecam.cpp` with a **whole-file CRLF rewrite** — 3072
changed lines for a **12-line** real change (`git diff -w --ignore-cr-at-eol`).
Functionally harmless, but it makes the commit unreviewable by diff and will conflict
with any concurrent edit to that file.

---

## WHAT IS NOT VERIFIED — stated plainly

The standing capture rule (>=3 locations per map, day AND night, mc2_01/03/10/24,
WIDE verdict shots) was **not met**, and cannot be met with the current harness:

* **mc2_10 captures are nondeterministic across process launches** (noise floor up to
  24 % of pixels). Any verdict shot from that mission is unreadable as evidence unless
  the delta clears that floor. This invalidates the pose-based grading of *both*
  features on the one map that actually has the towers and the runway.
* **mc2_03 and mc2_24 have only one distinct user pose each** (their second bookmark is
  a byte-identical dup), so ">=3 locations" is unsatisfiable there from user bookmarks.
* Consequently **no clean ground-level / mid-altitude / overview triad exists for the
  night-tags flip**, which is exactly the axis the previous marker-light regression
  escaped on. This is the single biggest reason not to flip `MC2_STATIC_PROP_NIGHT_TAGS`
  ON yet.

The `c1f98b45` verdict does **not** depend on any of this — it rests on the numeric
diag over all 9 fixtures, which is immune to render nondeterminism.

**Recommended follow-up lane:** make `run_visual_capture` reproducible across launches
for mc2_10 (the fixed-timestep/seed path is evidently not sufficient), then re-grade the
night-tags flip at three altitudes.

---

## Gates

| gate | default | state |
|---|---|---|
| `MC2_CONE_SPILL_COUPLING` | **OFF** (was the ungated `c1f98b45` behaviour) | new; registered, documented, smoke-allowlisted |
| `MC2_STATIC_PROP_NIGHT_TAGS` | OFF | unchanged — recommend a scoped Hot-Green mode before flipping |
| `MC2_LIGHT_CONE_NODES` | ON | unchanged, but **unregistered + undocumented** — worth fixing |

Commits: `6823bc00` (gate, no-op) → `7f3d0eec` (default OFF + evidence) →
`c0111762` (diag reports applied branch).

`check-env-registry.sh` PASS (992 vars). Contracts **67/68** (`doc_links`
pre-existing at parent, unchanged by this lane).
