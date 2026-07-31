# MEASUREMENT-LEDGER.md

Lane `MEASUREMENT-ADJUDICATE-1`, 2026-07-31. Worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`. Session start `0d0db03ec`; branch advanced to `c09234f6f`
**during** this adjudication (see the box below — it changed one of my verdicts).

## Why this file exists

A recurring failure mode: a lane computes a number, reports it, and **never commits
the artifact or the script**. A later lane greps the repo, finds nothing, and reports
the number as fabricated. **Both lanes behaved correctly.** The repo is not a record of
what was measured — it is a record of what was *committed*.

This ledger settles every number disputed on 2026-07-31 with a **three-way** verdict.
The middle category is the whole point:

| Verdict | Meaning |
|---|---|
| **MEASURED AND REPRODUCIBLE** | The artifact or script exists and I re-ran it, or verified it from source. Safe to repeat to the user. |
| **MEASURED BUT UNCOMMITTED** | A lane's report contains it with a plausible derivation, and the derivation checks out, but **nothing tracked in git reproduces it**. Usable — but must be labelled as unreproduced, and the fix is one `git add`. |
| **UNSUPPORTED** | No derivation anywhere, including in lane reports and scratch. **Never repeat to the user.** |

**Rule for future lanes:** "I could not find it in the repo" is evidence of
*non-commitment*, not of *falsity*. Before you write "fabricated", check: untracked
files (`git status --porcelain`, a leading `??`), sibling worktrees, sibling *installs*
(`A:/Games/mc2-opengl/...` holds the stock data this worktree lacks), commits that
**pruned** an artifact after it was used (`git log --diff-filter=D`), and
`git log --all --oneline -- <path>` — a file can be `??` in your worktree and still be
committed on a ref you are not on.

> ### The failure mode caught itself, live, inside this ledger
> I opened this adjudication at `0d0db03ec`, where `scripts/object_burial_audit.py` and
> both turret CSVs were **untracked (`??`)**. I graded Dispute 1 **MEASURED BUT
> UNCOMMITTED** and wrote out the `git add` that would fix it. While I was still working,
> the originating lane committed all three files as **`6d9360046`**, and the verdict
> became **MEASURED AND REPRODUCIBLE**.
>
> Nobody was wrong at any point. The number was real the whole time; the repo simply had
> not caught up yet. **This is exactly the gap this ledger exists to name — and it is
> narrow enough to open and close inside a single session.** Timestamp your verdicts and
> record the commit you graded against.

---

## THE LEDGER

### Dispute 1 — turret elevation on mc2_03

| # | Number as claimed | Verdict | Evidence |
|---|---|---|---|
| 1a | mc2_03 has **11 TURRET-class objects** | **MEASURED AND REPRODUCIBLE** | Re-derived below: exactly 11. |
| 1b | They sit on the **gameplay heightfield to max abs delta 1.97e-6 wu** | **MEASURED AND REPRODUCIBLE** | I recomputed at full precision: **1.969674e-06 wu**. Exact match. |
| 1c | Three `L_TurretGenerator` objects, **minZ -24.928 on height 51.99 = 47.95% of the mesh below origin** | **MEASURED AND REPRODUCIBLE — but the framing is misleading; see caveat** | Recomputed: minUp `-24.927999`, maxUp `27.057600`, height `51.985600`, **47.9517%**. Exactly 3 instances on mc2_03. |
| 1d | Counter-claim: *"2e-6 and 47.95% are not in the repo; the decisive check has never been run; the mission .pak is not in this worktree"* | **RETRACT — the first two clauses are now false** | The check **has** been run, twice, and I ran it a third time. Only the third clause is true, and it is not load-bearing: the script finds the stock data in a sibling install. |

**Why the dispute happened, and how it resolved.** At `0d0db03ec` the apparatus was on
disk but **untracked** — a lane grepping tracked files correctly found nothing:

```
$ git status --porcelain scripts/object_burial_audit.py tests/terrain/object_burial_*.csv
?? scripts/object_burial_audit.py
?? tests/terrain/object_burial_by_kind_2026-07-31.csv
?? tests/terrain/object_burial_turrets_2026-07-31.csv
```

**It is now committed** — `6d9360046` *"tools(terrain): OBJECT-BURIAL-AUDIT-1"*, landed
2026-07-31 01:47, all three files, 2927 insertions:

```
scripts/object_burial_audit.py                     | 1314 ++++++
tests/terrain/object_burial_by_kind_2026-07-31.csv |  333 +++++
tests/terrain/object_burial_turrets_2026-07-31.csv | 1280 +++++
```

The script (1314 lines, lane `TURRET-Z-TABLE-1`) **is** the line-by-line port of
`MapData::terrainElevation()` (`mclib/mapdata.cpp:2189`). Dispute 1 is fully reproducible
from a fresh clone as of `6d9360046`.

**The committed result is broader than the disputed claim**, and is itself safe to
repeat: across **all 72,153 stock objects on all 37 shipped maps, 0 unresolved rows**,
`authored_z` equals the **Phong-retired** elevation to **max abs delta 0.0001 wu on every
class**, while against the **legacy Phong** elevation it disagrees by up to **13.39 wu on
59-96% of objects**. The shipped data was authored on the plain triangle-linear
heightfield. mc2_03's eleven turrets are one cell of that table, not a special case.

**Reproduction (I ran all of these today):**

```
py -3 scripts/object_burial_audit.py --missions mc2_03            # the table
py -3 scripts/object_burial_audit.py --selftest                   # 8 negative controls
```

Full-precision 1b (the CSV rounds to 6 dp, which is why it prints `2e-06`, not `1.97e-06`
— **the rounding is the whole reason 1.97e-6 "appears nowhere"**):

```
=== mc2_03 TURRET-class objects: authored_z vs GAMEPLAY (plain) heightfield ===
name                            authored_z      ground_plain           delta
MediumAutoCannonTurret       190.000000000     190.000000000       0.000e+00
MediumAutoCannonTurret       190.000000000     190.000000000       0.000e+00
MediumAutoCannonTurret       190.000000000     190.000000000       0.000e+00
SRMTurret                     51.223564148      51.223564148       0.000e+00   (x6)
MediumAutoCannonTurret       217.647064209     217.647064209       0.000e+00
MediumAutoCannonTurret       236.339859009     236.339857039       1.970e-06
COUNT = 11
MAX |authored_z - ground_plain|  = 1.969674e-06 wu   <-- 'gameplay heightfield'
MAX |authored_z - ground_phong|  = 1.206563e+00 wu   <-- legacy CPU-Phong (NOT gameplay)
```

> **The words "gameplay heightfield" are load-bearing and must never be dropped.**
> Against the legacy CPU-Phong field the same 11 objects are off by up to **1.21 wu** —
> six orders of magnitude worse. The claim is true only because CPU-Phong is retired by
> default. Verified **from the C++, not from docs** — `mclib/mapdata.cpp:2452-2454`:
> ```cpp
> const char* v = getenv("MC2_CPU_PHONG_RETIRE");
> return !v || v[0] != '0';   // default ON; only "=0" reverts
> ```
> and `RenderCore/RendererFeatureRegistry.h:3849-3852` (default `true`). If that gate is
> ever flipped, **1b becomes false** and the turrets are off by ~1.2 wu.

> **CAVEAT ON 1c — the number is right, the impression it creates is wrong.**
> The `-24.928` does **not** come from the building. It comes entirely from an
> **8-vertex `Coil` card that straddles the origin symmetrically** (-24.93 .. +24.93).
> The actual body (`PowerGenerator`, 67 verts) bottoms out at **-0.6365 wu = 1.22% below
> origin**. The script's own docstring says this; the summary sentence dropped it.
> Anyone quoting "47.95% of the mesh is below ground" is describing a decorative coil,
> not a sunk building. **Quote 1.22%, or quote both.**
> Also: `L_TurretGenerator` is BUILDING-class and **re-snaps to ground every frame**
> (`code/bldng.cpp:830`), so the authored-Z columns do not even apply to it.

**Reproducible from a fresh clone: YES, as of `6d9360046`.** No follow-up needed.
One residual gap worth knowing: the script needs stock data it locates in a *sibling
install* (`A:/Games/mc2-opengl/mc2srcdata-fresh/missions` + `mc2-win64-v0.4c`), not in
this worktree. A clone alone is not sufficient — an install must be present. That, and
only that, is the true part of "the mission .pak is not in this worktree".

---

### Dispute 2 — do the two complaints name the same models?

| # | Claim | Verdict |
|---|---|---|
| 2a | *"The black-quad report names the same two models as the sunk-turret finding"* | **UNSUPPORTED as written — the overlap is ONE model, not two** |
| 2b | The black-quad model names themselves (`BTurretControl`, `LiaoTurretGenerator`) | **MEASURED BUT UNCOMMITTED — origin is outside the repo, but every name checks out against mission data** |

**The sets, measured from the shipped `mc2_03.pak`:**

| Set | Members on mc2_03 |
|---|---|
| Sunk-turret finding (the 47.95% one) | `L_TurretGenerator` x3 — **one model** |
| Black-quad report | `BTurretControl` **and** `LiaoTurretGenerator` — **two models** |
| Overlap | **`LiaoTurretGenerator` only = 1** |

`BTurretControl` is **measurably not sunk**: minUp `-0.171800` on height `94.350099`
= **0.18% below origin**, body min identical. It has no burial defect at all. So it
cannot be part of a burial finding.

**Resolution — the sentence conflates two different groupings, and one reading is true:**

- Read as *"the two models in the 47.95% burial finding"* → **FALSE.** That finding
  covers one model.
- Read as *"the turret-named BUILDING-class props on mc2_03"* → **TRUE.** That
  population is exactly `{L_TurretGenerator x3, BTurretControl x2}` — precisely the two
  models the black-quad report names.

**What this means for the original question (are the two complaints one bug?): NO.**
They share one model. The co-located second model (`BTurretControl`) has the black-quad
defect and demonstrably **not** the burial defect. Two defects on overlapping props in
one compound — not one bug with two symptoms.

**On provenance of the black-quad names — the honest answer.** `BTurretControl` and
`LiaoHeavyGateControl` appear in **no lane status doc, in any worktree**:

```
$ for d in /a/Games/mc2-*/.claude; do grep -rl "BTurretControl" "$d"; done
(no output)
$ git log --all --oneline --grep="BTurretControl\|black.quad" -i
(no output)
$ grep -rl "BTurretControl" /a/Games/mc2-main
release_assets_src/0.5.2/review_pack.json          <- the review pack itself
release_assets/0.5.2/REVIEW/INDEX.html             <- its mirror
scripts/object_burial_audit.py                     <- measurement tool, names no defect
tests/terrain/object_burial_*.csv                  <- measurement output, names no defect
```
(The last two are now committed at `6d9360046`, but they are *elevation* data — they
establish the models exist and are not sunk; they say nothing about black quads.)

So the names entered the repo **only** at the review pack, which describes the defect as
"stated precisely for the first time". There is no in-repo derivation. **But they are not
invented** — I ran an authenticity check and every model named survives it:

| Named in black-quad report | Present on mc2_03? | `.fit` |
|---|---|---|
| `BTurretControl` (defective) | yes, x2 | `BTurretControl.fit` |
| `LiaoTurretGenerator` (defective) | yes, x3 | `LiaoTurretGenerator.fit` |
| `LiaoHeavyGateControl` (renders correctly) | yes, x1 | `LiaoHeavyGateControl.fit` -> appearance `L_GateControl` |
| Barracks (renders correctly) | yes, x2 | `Barracks.fit` |
| Quonsets (render correctly) | yes, x9 | `Quonset` / `Quonset2` / `QuonsetClean` |

All six families are on the map, the report uses the `.fit` naming convention correctly,
and the "renders correctly" contrast set is exactly the comparable window-banded
buildings present. That is not a pattern you get by guessing. **Verdict: the names are
almost certainly a faithful transcription of a real observation made outside the repo
(the user's own frame). Usable, but flag as unreproduced-in-repo.**

**To make reproducible:** record the black-quad observation's source (the frame the user
supplied, or the screenshot path) in a tracked status doc. That is the only missing link;
the model names themselves now cross-check cleanly against committed mission data.

---

### Dispute 3 — the `dirty` flag

| # | Claim | Verdict |
|---|---|---|
| 3a | *"`dirty` is a compile-time int and is NOT recoverable from the binary"* | **The premise is right; the conclusion is WRONG** |
| 3b | *"dirty=False verified from the exe's own embedded fingerprint"* | **SUPPORTED — if the lane ran `exe_fingerprint.py`** |
| 3c | Same claim, when actually backed by `deploy_status()` / manifest `src_commit` | **SUPPORTED BUT BY A DIFFERENT (WEAKER) ROUTE** |

**From the C++, not the docs.** `build64/generated/build_fingerprint.h:5`, generated by
`cmake/GenerateBuildFingerprint.cmake:57`:

```
#define MC2_BUILD_GIT_DIRTY 1
```

An integer preprocessor macro. The disputing lane is **correct** that its *value* never
reaches the string table — `code/mechcmd2.cpp:904` passes it as a `%d` runtime vararg:

```cpp
printf("[BUILD_FINGERPRINT v1] sha=%s dirty=%d branch=%s built=%s src=%s\n",
       MC2_BUILD_GIT_SHA, MC2_BUILD_GIT_DIRTY, ...);
```

**But three lines later is a `#if`, not a runtime branch** — `code/mechcmd2.cpp:906-908`:

```cpp
#if MC2_BUILD_GIT_DIRTY
    printf("[BUILD_FINGERPRINT v1] WARNING: BUILT-FROM-DIRTY-TREE\n");
#endif
```

When dirty==0 that string literal is **deleted by the preprocessor and cannot exist in
the binary**. When dirty==1 it is compiled in. `scripts/exe_fingerprint.py:182` reads the
flag exactly this way — **presence of a companion marker, not a stored value**:

```python
"dirty": bool(blob.find(DIRTY_MARKER) >= 0)
```

So `dirty` **is** recoverable from the exe — indirectly, and soundly.

**Reproduction, run today:**
```
py -3 scripts/exe_fingerprint.py A:/Games/MC2-OpenGL-TIP-2026-07-31/mc2.exe --json
-> {"present": true, "sha": "e4ce37d91162", "dirty": false, "branch": "deploy/tip-2026-07-31"}
```

**Negative control + the decisive positive control:** an independent byte scan found no
`dirty=` value literal anywhere in the 12.5 MB exe (confirming the premise), while the
known sha string `e4ce37d91162` **was** found (confirming the scanner works), and an
impossible needle was absent. Then the true two-directional control:
`A:/Games/mc2-main/build64/RelWithDebInfo/mc2.exe` was compiled with
`MC2_BUILD_GIT_DIRTY 1`, and there the marker is **present** and the tool reports
`dirty: true`. **The flag flips correctly in both directions** — it is not a scan that
always says "clean".

> **The real trap, and the thing to police.** `scripts/deploy_payload.py:509-540` reads
> `#define MC2_BUILD_GIT_DIRTY` out of the **generated header**, and
> `tools/repo_intel/deploy_status.py:205` infers dirty purely from whether the manifest's
> `src_commit` ends in `-dirty`. **Neither touches the exe.** `exe_fingerprint.py`'s own
> docstring warns that the header route answers "what did cmake stamp last", and a
> failed or skipped compile leaves it silently wrong. Several lane docs
> (`GOLDEN-REBLESS-2-STATUS.md:43` among them) say "exe embedded ... dirty=false" while
> citing only `deploy_status()` / `--require-clean`. **That phrasing is the bug.**
> Say which route you used. `exe_fingerprint.py` against the actual exe is the strong one.

---

### Dispute 4 — albedo and trees

| # | Claim | Verdict |
|---|---|---|
| 4a | *"Trees are the darkest asset class at every shipped colour scheme"* | **MEASURED AND REPRODUCIBLE — but must be RESTATED; as written it is a category error** |
| 4b | *"'Mechs are the brightest' is not robust — the rank flips on **5 of 10** schemes"* | **UNSUPPORTED. Retract the figure.** |

Tool `tools/albedo_masked_audit.py` is **tracked and committed** (`dcdce66dc`).

**4a — the objection is correct, and the tool says so in its own source.** Paint is
applied only to mech and vehicle: `PAINTED_CLASSES = ("mech", "vehicle")`
(`tools/albedo_masked_audit.py:936`), gated at `:1282`. The tool documents it at
`:976-979`: *static_prop and tree are never repainted; their one number is unambiguous.*
Trees are scheme-invariant **by construction**, so "at every scheme" is not a property
they can have.

```
py -3 tools/albedo_masked_audit.py --data-root A:/Games/mc2-srcdata \
     --classes static_prop,mech,vehicle,tree
```
As-drawn median L: **tree 58.0** < vehicle 96.9 < static_prop 103.9 < mech 112.0.

**Correct restatement (this one is TRUE and safe to repeat):**
> *Trees are the darkest asset class — and trivially so at every scheme, because trees
> are never repainted.*

Note the trap: at the **raw .tga** convention (a paint mask that is never drawn —
`:106-115`) mech reads 47.0 and vehicle 48.4, both *below* tree's 58.0. Quote the
wrong convention and you conclude the opposite. Always say **as-drawn**.

**4b — "5 of 10" is a flag default wearing a measurement's clothes.** `--paint-sweep-limit`
defaults to 10 (`:1237-1239`) and merely caps how many schemes get iterated. The true
population is **114 distinct mission-authored schemes** (`harvest_mission_schemes()` over
`A:/Games/mc2-srcdata/missions/*.fit`). No reading reproduces 5-of-10:

| Comparison | Flips |
|---|---|
| mech vs vehicle, first 10 | **0 of 10** |
| mech vs static_prop, first 10 | **6 of 10** |
| mech vs static_prop, all 114 | **66 of 114** |
| mech vs vehicle, all 114 | 5 of 114 *(contains a "5"; wrong denominator)* |

The **qualitative** claim survives and is reproducible: paint scheme swings mech albedo
**71.2 .. 112.7 (1.58x)** at n=10 and **30.1 .. 170.5 (5.66x)** across all 114, so
"mechs are the brightest" is not a scheme-stable property. **Say that. Do not say
"5 of 10".**

**Negative control:** the tool's own `--negative-control` passes (identical art via
different files -> ratio 1.0000; sampling-density invariance -> PASS; discrimination
check -> PASS). Plus a purpose-built tree-invariance control: the same tree asset
(`birch2`) under two unrelated `--paint-slots` arguments returns **L=76.9 both times**,
while the same sweep moves mech/vehicle 1.5-5.7x. The sweep genuinely varies something,
and correctly leaves scheme-invariant assets untouched.

---

### Dispute 5 — the mc2_17 waterline

| # | Claim | Verdict |
|---|---|---|
| 5a | mc2_17 staircase index **0.260** | **MEASURED AND REPRODUCIBLE** (reproduced digit-for-digit) |
| 5b | **0.222** smooth-curve floor | **MEASURED AND REPRODUCIBLE** — and it is an *analytic constant*, 20/90 |
| 5c | *"The cures were verified on mc2_10 and mc2_01, never on mc2_17"* | **FALSE as a blanket claim** — true only of each cure's initial pass |
| 5d | *"So the three dead cures were aimed correctly"* | **UNSUPPORTED** |

**This is the ledger's best example of "pruned, not fabricated".** The computing tool
landed at `95ffdb9b`, then commit `6ed1a0d2e` (*"FABLE-DEBLOAT-1 — prune the shoreline
NO-GO's raw artifact tree from the tip"*) **deliberately removed it from the tip**. Both
are ancestors of HEAD. A lane grepping the working tree finds nothing and concludes
fabrication; the artifact is one `git show` away.

```
git show 95ffdb9b:.claude/shoreline-v0-falsify-1/fine_level.py > fine_level.py
git show 95ffdb9b:.claude/shoreline-v0-falsify-1/fst_probe.py  > fst_probe.py
python3 fst_probe.py Viewer/mission.fst mc2_17. <paks_dir>
python3 fine_level.py <paks_dir> <out_dir>
```
```
mc2_17  stair=0.260   mc2_01  stair=0.226   mc2_05  stair=0.297   mc2_10  stair=0.737
```
Pak md5s matched the shipped `mission.fst` hashes exactly.

**5b is not an empirical floor** — it is a property of the metric. `shape_metrics()`
classifies "near-axis" as contour angle <10deg or >80deg out of a folded [0,90] range:
**20/90 = 0.2222...**

**Negative control (exactly the required shape):**
```
CONTROL A (perfect circle, 200k segments): staircase_index = 0.22222611   <- returns the floor
CONTROL B (literal 128-wu axis-aligned staircase): staircase_index = 1.0  <- returns the max
Analytic isotropic expectation = 20/90 = 0.22222222
```
The metric reports "no defect" where there is none and saturates where there is.

**5c — the counter-claim overreaches and must itself be corrected.** Cure 1
(`MC2_WATER_SHORE_FEATHER`, `a8ea6726`) was indeed verified only on mc2_10 + mc2_01.
But cure 2 (`MC2_WATER_SHORE_MEANDER`) **was** measured on mc2_17 during its death
(`WATER-MEANDER-CLAMP-1`: bookmark `tests/visual/bookmarks/mc2_17_water_wide.json`,
net water -11.18 pts — *that mc2_17 number is why the default was reverted to 0*), and
cure 3 (B-spline, `SHORELINE-V0-FALSIFY-1`) was falsified numerically against **mc2_17's
own heightfield**. So: true of initial passes, false of the cures' lifecycles.

**The premise, verified from GLSL/C++ rather than docs — TRUE, but conditionally.**
`shaders/shoreline.frag:22-23`:
```glsl
bool isWater = (myAlpha > 0.15 && myAlpha < 0.35); if (!isWater) return;
```
Water writes gbuffer1 alpha **0.0** (`render_contract.hglsl:27-29`), default terrain
writes **1.0** (`:95-97`) — both outside the band, so the legacy pass cannot touch
water. **However** `shaders/gos_terrain.frag:475-476` writes a *continuous*
`mix(1.0, 0.25, waterFlag)` that **does** land in-band — but that is the legacy terrain
shader, live only under `MC2_TERRAIN_LOD_CHUNK=0` (default is ON,
`RendererFeatureRegistry.h:3390-3393`). **Premise true at default config; false under
that opt-out.**

**5d — why "aimed correctly" fails.** Ruling out the post-process narrows the
*subsystem*. It does not show the three specific cures were tested against the defect.
And the numbers undercut the aim: mc2_17 sits at **0.260 against a 0.222 floor — 0.038
above isotropic**, barely a defect, while **mc2_10 measures 0.737**, deep toward the
pathological end. **The cures were tuned on the map with the real staircase and the
complaint is about a different map.** An untested rival (terrain LOD decimation at
gameplay altitude; a "drape cap / depth-test at 128 wu" lead) remains open.

---

### Dispute 6 — `MC2_STATIC_PROP_IBL_SUN_ROTATE` reachability

| # | Claim | Verdict |
|---|---|---|
| 6a | *"Absent from all three binaries -> NOT GRADABLE"* | **TRUE of those three builds** |
| 6b | *"Present in a fourth install, `MC2-OpenGL-TIP-2026-07-31` @ `e4ce37d91162`"* | **TRUE** |
| 6c | Are 6a and 6b contradictory? | **NO — COMPATIBLE. Different points in time.** |

The gate was introduced by **`f16b49e96`** (2026-07-31 00:51). Every install predating it
lacks the string; the one postdating it has it. There is no anomaly in either direction.

| Install | fingerprint sha | gate | pos. ctrl | neg. ctrl | introducing commit is ancestor? |
|---|---|---|---|---|---|
| `MC2-OpenGL-CURRENT-2026-07-30` | `e1bdb7af697d` | **ABSENT** | present | absent | **NO** |
| `MC2-OpenGL-AB-SUNAXIS-2026-07-30` | `dab061529977` | **ABSENT** | present | absent | **NO** |
| `MC2 OpenGL Remastered` (2026-07-05) | `85aaaa4f57b1` | **ABSENT** | present | absent | **NO** |
| `MC2-OpenGL-TIP-2026-07-31` | `e4ce37d91162` | **PRESENT** | present | absent | **YES** |
| `mc2-main/build64/RelWithDebInfo` (dirty) | `49de784c5c38` | **PRESENT** | present | absent | **YES** |

**Negative control:** positive control `MC2_TERRAIN_LOD_CHUNK` present in all five
(the scan finds real strings); impossible needle `MC2_THIS_GATE_CANNOT_EXIST_9f3a2b`
absent in all five (the scan is not matching everything). Both clean, so the ABSENT
results are real. Each exe's independently computed sha256 also matched its own
`.deployed_manifest.csv`.

**The false-absent risk was checked explicitly:** the reader is a plain literal
`getenv("MC2_STATIC_PROP_IBL_SUN_ROTATE")` at
`GameOS/gameos/gos_static_prop_batcher.cpp:1264` — **not** assembled from prefix+suffix
at runtime — so a string-table scan is a valid method here. (Had the name been
constructed at runtime, the scan would have produced a false ABSENT. **Always check this
before trusting a string-table absence.**) Registered
`RenderCore/RendererFeatureRegistry.h:3939-3948` (default false), documented
`docs/tier1_env_vars.md:378`, allowlisted `scripts/run_smoke.py:2319`.

**6d — gradable today, but not yet graded.** All wiring is present and consistent in the
TIP install. That is **mechanically gradable**, which is not the same as **visually
graded**: per that install's own `READ-ME-FIRST.md`, nothing in it has ever been launched
or screenshotted. Do not report this gate as verified-looking-correct.

---

## RETRACTIONS — do not repeat these to the user

1. **"the rank flips on 5 of 10 schemes"** — UNSUPPORTED. `10` is `--paint-sweep-limit`'s
   default; the real population is 114. Say instead: *paint scheme swings mech albedo
   1.58x over 10 schemes and 5.66x over all 114, so "mechs are the brightest" is not
   scheme-stable.*
2. **"trees are the darkest at every colour scheme"** — restate as *"trees are the
   darkest, and trivially so at every scheme, because trees are never repainted."*
3. **"the black-quad report names the same two models"** (as applied to the 47.95%
   burial finding) — the overlap is **one** model. `BTurretControl` is not sunk (0.18%).
4. **"the three dead cures were aimed correctly"** — UNSUPPORTED.
5. **"the cures were verified on mc2_10 and mc2_01, never on mc2_17"** — false as a
   blanket claim; two of the three were measured on mc2_17, and that is what killed them.
6. **"2e-6 and 47.95% appear nowhere in the repo; the decisive check has never been
   run"** — **false.** It was run twice, I ran it a third time, and the whole apparatus
   is committed at `6d9360046`. It was untracked when that objection was written, which
   is why the objection was reasonable — and it is now simply out of date.
7. **"47.95% of the mesh is below origin"** quoted bare — misleading. The body is
   **1.22%** below; the rest is an 8-vertex decorative `Coil` card symmetric about origin.
8. **"dirty=False verified from the exe's embedded fingerprint"** when the lane actually
   ran `deploy_status()` or read the generated header — say which route. Only
   `scripts/exe_fingerprint.py` against the exe is the strong claim.
9. **"56,395 of 72,153 objects (78%) move"**, **"96.5% of trees move"**, **"±2.4 m
   world-wide"**, and **"his first launch will look like the world shifted"** — ALL
   WRONG (retracted 2026-07-31, RESNAP-INVERSION-1). `scripts/object_burial_audit.py`
   had its elevation model **backwards** until `632122f7d`: it hard-coded zero change
   for the re-snapping classes and applied the delta to the frozen-Z ones, on the false
   premise that the **drawn** ground was Phong-smoothed before `a75339bb0`. It was not —
   that commit changed **gameplay** elevation only. The `±11.8379 wu` these claims rested
   on is the TREE row's *authored-vs-Phong agreement* column, **not a movement**. Say
   instead: *6,155 of 72,153 objects (8.5%) change apparent burial — 6,094 BUILDING and
   61 GATE, zero trees/turrets/terrain objects — 3,272 down, 2,883 up, max 13.3948 wu
   (2.674 m) on a bridge.* Re-derived from the fixed tool and independently recomputed
   from the raw per-object CSV.
10. **"the CPU-Phong retire is a candidate explanation for sunk TURRET-class objects"** —
   INVERTED. TURRET keeps its authored Z, so `a75339bb0` changes it by **exactly zero**
   (now pinned by NEGATIVE CONTROL 9 in the audit's `--selftest`). The retire moves the
   re-snapping BUILDING/GATE population and nothing else.
11. **"the committed `object_burial_by_kind_2026-07-31.csv` corroborates 11.8379"** —
   it did not. That CSV was itself output of the inverted model, so the two artifacts
   agreeing proved only that the same bug ran twice. **Agreement between two derived
   artifacts is not corroboration when both derive from the same broken step.** The CSV
   has been regenerated under the corrected model.

## Safe to repeat

- mc2_03 has exactly **11 TURRET-class objects**, sitting on the **gameplay** heightfield
  to **1.97e-6 wu** — fully reproducible via `scripts/object_burial_audit.py` (`6d9360046`).
- The fleet-wide version: across **72,153 stock objects on 37 maps**, authored Z matches
  the Phong-retired elevation to **0.0001 wu**, vs up to **13.39 wu** against legacy Phong.
  (This is an **agreement** test, not a movement claim — the distinction is what
  retraction 9 above is about. It was correct then and is correct now.)
- What `a75339bb0` actually moves: **6,155 of 72,153** placed objects (**8.5%**), all of
  them **BUILDING (6,094)** or **GATE (61)**; **TREE, TURRET and TERRAINOBJECT change by
  exactly zero**. 3,272 settle down, 2,883 rise up, max **13.3948 wu (2.674 m)** on a
  bridge (`mc2_m03 #1266`).
- The burial that was real and worth telling him about: **`L_LightWall`, 43 instances on
  mc2_03**, sat up to **6.23 wu (1.24 m)** underground — **26.9%** of the wall's own
  23.16 wu (4.62 m) height — and `a75339bb0` fixed it **default-ON**.
- mc2_17 staircase **0.260** vs an **analytic 0.222** isotropic floor; **mc2_10 is 0.737**.
- `MC2_STATIC_PROP_IBL_SUN_ROTATE` is gradable **only** on `MC2-OpenGL-TIP-2026-07-31`,
  and has never been looked at.
- Trees are the darkest as-drawn asset class.
