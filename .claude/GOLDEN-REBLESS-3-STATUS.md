# GOLDEN-REBLESS-3 — STATUS

Re-bless the visual goldens at HEAD from one clean attributable build, on the
deterministic path, with the full effective `MC2_*` environment recorded; refuse
to bless through measured noise; vendor the poses; and rule on
`v0.5.2-baseline-PROPOSED`.

**Outcome: 28 of 39 regions blessed, 11 refused.** New golden:
**`tests/visual/goldens/baseline-5ef261f4/`**. Contracts **74/74**. tier1
**5/5 PASS with `DEPLOY_FINGERPRINT OK`**.

---

## META REVIEW — brief premises checked against source, not status docs

The brief's headline premise was that `baseline-6c47e149` is *"stale in default
behaviour"* because a list of defaults *"moved since it was blessed"*. Checked by
`git merge-base --is-ancestor` against the blessed commit, **six of the eight
were already inside the blessed tree**.

| brief premise | verdict |
|---|---|
| `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` ON (`1b23a82b`) moved since blessing | **FALSE.** Already an ancestor of `6c47e149` (landed 07-25T23:22; blessed commit 07-26T05:07). GOLDEN-REBLESS-2 §4 already records it as blessed-under state. |
| `MC2_STATIC_PROP_IBL_NIGHT_SCALE=0.25` (`7dd64716`) moved since blessing | **FALSE.** Already an ancestor. The prior lane did not merely inherit it: §7 shows it landed mid-lane, the pre-IBL golden was **discarded** and all three runs re-captured. |
| `MC2_STATIC_PROP_FIXTURE_LIGHTS`, `MC2_LIGHT_GRID`, `MC2_LIGHT_POOL_MAX=256`, `MC2_LETTERBOX_OPAQUE` moved since blessing | **FALSE.** All four are explicitly listed in GOLDEN-REBLESS-2 §4 as the default-ON set blessed under. |
| `aoRadius` 3.0 → 24.0 is live drift | **INERT.** `MC2_SSAO` is default-**OFF** (`gos_postprocess.cpp:976` + registry); with it off `runSSAO` is skipped entirely and the registry states the OFF path is byte-identical. `aoRadius_(24.0f)` cannot reach a default-launch pixel. |
| `MC2_WATER_SHORE_MEANDER` 75 → 0 (`67b25f47`) | **TRUE, landed after.** The "byte-identical" claim is indeed refuted, and the refutation now lives in the registry entry itself: within-arm floor 0.0000%, between-arm **4.07 / 6.52 / 9.74%**, max channel delta 73/73/115. |
| `MC2_TERRAIN_CONCRETE_DETAIL_NORMAL` → OFF (`62f789c2`) | **TRUE, landed after** — and the brief's own caveat holds: no-op where the normal assets are absent. |
| the golden needs re-blessing | **TRUE — for a different reason.** Not the gate list: **69 commits / ~1,667 lines** of render+shader delta since `6c47e149` (`gos_terrain_indirect` +433, `bdactor` +239, `RendererFeatureRegistry` +171, `gos_postprocess` +129, `terrain_lod_chunk.frag` +105, `ssao_blur.frag` +90, `static_prop.vert` +80, plus `lighting.hglsl`, `txmmgr`, `terrain.cpp`). **That is the staleness case, and it is the framing used here.** |
| `bookmark_honesty` extended to golden poses by `352c6da6` | **TRUE, verified.** It resolves `tests/visual/goldens/*/poses/*.json` via `git ls-files` and WARNs on any set with no vendored `poses/`. The prior lane's open caveat is **closed**. |
| the `capture_determinism` ratchet missed the `visual_verify` layout | **CLOSED since.** Now discovers with `rglob`, grades **all pairs**, defaults `--min-runs 3`, and hard-codes the prior lane's measurement as its rationale. |
| `MC2_LETTERBOX_OPAQUE` unregistered/undocumented; `MC2_SSAO` undocumented | **BOTH FIXED SINCE.** `gate_status` returns `verdict=OK, flags=[]` for both. |

---

## 1. Two instrument-lies caught before they could bless a falsehood

These are the reusable findings. Both would have produced a golden that looked
entirely valid.

### 1a. The install shipped at 800x600 — and nothing would have complained

`mc2-win64-abl-validate/options.cfg` was `ResolutionX/Y = 800/600`. The
predecessor golden was captured at **3840x2160**.

The region rects are **resolution-independent** — `mode:"norm"` fractions, or
`mode:"px"` scaled from a 3840x2160 reference at crop time. So a capture at
800x600 crops *successfully*, grades *successfully*, and blesses *successfully*.
It simply encodes ~17x fewer pixels per region (a 3072x216 reference band becomes
640x60). Nothing in `capture.py`, `verify.py`, `bless.py`, the honesty checker or
the determinism ratchet asserts a capture resolution.

Caught only by reading `width`/`height` in a pose sidecar by hand. The first two
runs were discarded; `options.cfg` set to 3840x2160 (backup
`options.cfg.gr3bak`) and every run re-verified as `{(3840, 2160)}`.

**For the next lane: assert the capture resolution before blessing.** A golden
carries no record of what resolution it *should* have been, so a low-res golden
is indistinguishable from a correct one after the fact.

### 1b. Vendoring paid for itself immediately

The install's own `saved_bookmarks/` was **not** the golden pose set:

- 8 of 10 byte-identical,
- **2 absent** — `mc2_10_saved_204523`, `mc2_10_saved_212258`,
- plus 2 extras (`mc2_10_lance_close`, `mc2_10_saved_204937`) with **no region
  definitions**, which grade nothing and were dropped.

The 2 missing poses were restored from
`tests/visual/goldens/baseline-6c47e149/poses/` and all 10 verified byte-identical
to what was captured. **Had the predecessor not vendored them, this pose set
would have been unreproducible and this re-bless impossible.** The new set is
vendored (`poses/`, 10 files) for exactly that reason.

---

## 2. Build and deploy attribution

| | |
|---|---|
| blessed commit / build sha | **`5ef261f4`** (`claude/nifty-mendeleev`) |
| `MC2_BUILD_GIT_DIRTY` | **0** |
| exe sha256 | `99a12c4188c968d440b7ec664df9937b2d7e5cc326c2b46c5049ba1122ca8ad4` |
| deploy lane | `A:/Games/mc2-opengl/mc2-win64-abl-validate` |
| deploy | `deploy_payload.py --require-clean` → **PASSED**, 807 rows, PDB hash match, manifest `src_commit 5ef261f4dd03` |
| `deploy_status` | **OK** — `exe sha=5ef261f4dd03 matches expected`; no lane lock, no smoke lease |
| tier1 | **5/5 PASS**, exit 0, `[DEPLOY_FINGERPRINT] OK: exe sha=5ef261f4dd03 matches worktree HEAD` |

**Deploy-lane choice.** The brief forbids `v0.3`, `v0.4`, `v0.4c`,
`refbuild-cf97ed52`; a sibling held `v0.4d-rc1` (its `mc2.exe` was observed
running). `deploy_payload.py`'s allowlist admits six folders, leaving exactly one
free non-forbidden target: `mc2-win64-abl-validate` (3 registry lanes citing it
were 16–27 days stale against a 12 h TTL; no lane lock).

**Tree hygiene.** One tracked modification at lane start —
`docs/render-backend-seams/gated-layout-macros.md`, an auto-regenerated
header-line-number refresh, unclaimed per `check_conflict` — committed so the
fingerprint could be clean. The 43 untracked `scratch/`/`debug_state/` dirs are
irrelevant: the fingerprint uses `git status --porcelain -uno`.

---

## 3. A sibling landed a render fix mid-lane — the first golden was discarded

`7b9034cc fix(terrain): CEMENT-PATCHY-CAUSE-1 — match the overlay layer's
concrete lighting to the terrain's` landed **after** the first build
(`4bde3882`), touching `terrain_overlay.frag` and `gameos_graphics.cpp`.

A complete golden (29 blessed / 10 refused) had already been captured, validated
and committed at that point. It was **discarded** — set removed, tree rebuilt at
HEAD, redeployed `--require-clean`, and **all three runs re-captured** — following
GOLDEN-REBLESS-2's precedent for IBL-NIGHT-SCALE-1. The blessed golden is
post-fix and **is not stale on arrival**.

Pre-fix metrics retained as methodology evidence at
`scratch/gr3/preCementFix_reports/`.

Incidental, **not investigated and not claimed causal**: `cement_tile_seams`
moved *into* the stable set (6.40% → 0.0000%) while `mech_drop_shadows`
(0.008% → 0.15%) and `horizon_white_billboards` (0.0000% → 1.73%) moved *out*.

---

## 4. The bless rule, and why all three pairs

> A region is blessed **only** if reproducible across three independent launches
> of the same build. Ceiling `changed_pct <= 0.05 %`, **worst of all three
> pairs** (r1-v-r2, r1-v-r3, r2-v-r3).

All-pairs was decisive on this build — **three regions would have been blessed as
byte-perfect by a two-run check**:

| region | r1-v-r2 | r1-v-r3 | r2-v-r3 | a 2-run check on the zero pair |
|---|---|---|---|---|
| `mc2_10_saved_212258 / light_pools_sourcing` | 9.07% | 9.07% | **0.0000%** | **would have blessed it** |
| `mc2_10_saved_212258 / runway_grid_complete` | 5.16% | 5.16% | **0.0000%** | **would have blessed it** |
| `mc2_10_saved_204611 / horizon_white_billboards` | 1.73% | **0.0000%** | 1.73% | **would have blessed it** |

The *discarded* pre-fix build produced the same lesson on a different pair set
(`light_pools_sourcing` 0.0000% on r1-v-r3 while moving 20.37% / 20.65% on the
other two). This is now the third independent build on which the finding
reproduces, after CAPTURE-DETERMINISM-1 and GOLDEN-REBLESS-2.

**Whole-frame ratchet** (`check-capture-determinism.py --set-dir scratch/gr3
--min-runs 3`, 60 pairs graded) is reported for completeness: it FAILs at its
byte-stable threshold, as expected on `mc2_10`. Region-level grading governs
blessing.

---

## 5. Result: 28 blessed, 11 refused

**Blessed (28):** mc2_01 ×6, mc2_05 ×3, mc2_10 ×19. **27 of the 28 are
byte-identical (0.0000%) across all three pairs**; the single exception is
`searchlight_cones_compound` at 0.0002%.

**Refused (11)** — floors recorded in `golden.json` → `refused_regions`, crops
**deleted** so nothing can silently grade against them:

| bookmark | region | worst floor | r1v2 / r1v3 / r2v3 |
|---|---|---|---|
| mc2_10_saved_204753 | `lamp_cones` | **25.18%** | 25.18 / 16.27 / 24.70 |
| mc2_10_saved_204753 | `runway_edge_lights` | **22.70%** | 22.70 / 7.93 / 21.70 |
| mc2_10_saved_204753 | `light_clusters_census` | **21.16%** | 21.16 / 10.52 / 19.62 |
| mc2_10_saved_204907 | `spotlight_towers` | **19.04%** | 14.14 / 19.04 / 14.54 |
| mc2_10_saved_204611 | `far_water_surface` | **12.41%** | 10.51 / 11.88 / 12.41 |
| mc2_05_saved_205843 | `light_pools_feathered` | **9.70%** | 9.70 / 4.89 / 8.19 |
| mc2_10_saved_212258 | `light_pools_sourcing` | **9.07%** | 9.07 / 9.07 / **0.00** |
| mc2_10_saved_212258 | `runway_grid_complete` | **5.16%** | 5.16 / 5.16 / **0.00** |
| mc2_10_saved_204611 | `horizon_white_billboards` | **1.73%** | 1.73 / **0.00** / 1.73 |
| mc2_10_saved_204907 | `near_base_lighting` | **0.59%** | 0.37 / 0.59 / 0.22 |
| mc2_01_saved_205644 | `mech_drop_shadows` | **0.15%** | 0.14 / 0.15 / 0.15 |

The refused set is dominated by **lamp/searchlight cones, spotlights, runway and
light-pool lighting, water and mech drop-shadows** — consistent with the mover
target-acquisition attribution CAPTURE-DETERMINISM-1 left open.

**Validation.** `verify.py` grading r2 and r3 — neither of which produced the
golden — against it: **28 PASS / 0 FAIL, exit 0, both runs.** Integrity: 28/28
crop sha256 match, **0 zero-byte files**, 0 orphan crops, 0 refused crops present.

---

## 6. Gate state blessed under

**Engine defaults. No `MC2_*` render gate was overridden.** Verified via
`gate_status` against `RendererFeatureRegistry.h` + `docs/tier1_env_vars.md`.

Default-**ON**: `MC2_LIGHT_GRID`, `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN`,
`MC2_STATIC_PROP_FIXTURE_LIGHTS`, `MC2_LETTERBOX_OPAQUE`,
`MC2_LIGHT_POOL_MAX=256`, `MC2_STATIC_PROP_IBL_NIGHT_SCALE=0.25`,
`MC2_VIS_SEMANTICS`.

Default-**OFF** (listed explicitly, so a later flip is reviewable drift rather
than a surprise): `MC2_SSAO` (**therefore `aoRadius=24.0` is inert**),
`MC2_WATER_SHORE_MEANDER` (parked at 0), `MC2_TERRAIN_CONCRETE_DETAIL_NORMAL`
(OFF by user ruling), `MC2_LIGHT_REALISM`, `MC2_PAVED_OVERLAY_CLASS`,
`MC2_STATIC_PROP_FIXTURE_GLOW`, `MC2_PROP_SHADOW_RECEIVE`, `MC2_CEMENT_LIT`.

Flipping any default-OFF gate ON is **expected** to fail regions here.

**Effective environment**, recorded in every pose sidecar as `gate_env` and in
`golden.json` → `capture_env`: `MC2_HEARTBEAT`, `MC2_LOG`,
`MC2_SMOKE_FIXED_TIMESTEP`, `MC2_SMOKE_MODE`, `MC2_SMOKE_SEED`,
`MC2_VISUAL_BOOKMARK_CAPTURE`, `MC2_VISUAL_CAPTURE_DIR`,
`MC2_VISUAL_CAPTURE_FRAME`, `MC2_VISUAL_SETTLE` — **9 harness vars, no stray
gates**.

The three contaminating vars named in the brief (`MC2_STATIC_POP_SPLIT`,
`MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE`, `MC2_GPU_CULL_STATIC_FROZEN_RECORDS`)
**were confirmed live in this session's shell** and unset before every capture.
Independently, CAPTURE-ENV-PIN-1 scrubs all ambient `MC2_*` and reports what it
removed (it reported only `MC2_NO_LAUNCHER`). Both mechanisms agree.

---

## 7. Pose honesty

`check-visual-bookmark-honesty.py`: **exit 0**, *"47 tracked pose file(s) clean
(27 lane bookmarks + 20 vendored golden poses across 4 golden set(s))"*. The new
set's 10 poses are tracked and clean; it raises no WARN.

10 files, 10 distinct poses, real provenance notes, each `_dup` entry the
documented engine workaround. Framing is corroborated by content — the refused
regions move 1–25% run-to-run while the blessed ones sit at 0.0000%, which empty
terrain does not do. **Framing is corroborated, not eyeballed crop-by-crop.**

---

## 8. `v0.5.2-baseline-PROPOSED` — RECOMMEND RETIRE, do not promote

Independent of pixel drift, it fails on provenance:

| property | `v0.5.2-baseline-PROPOSED` | `baseline-5ef261f4` |
|---|---|---|
| `blessed_commit` | **`null`** — unattributable to any source state | `5ef261f4…` |
| vendored `poses/` | **absent** — honesty checker: *"UNAUDITABLE and unrecoverable"* | 10, checker-clean |
| `gate_state` | **absent** | recorded |
| `capture_env` | **absent** | recorded (9 vars) |
| reproducibility / `refused_regions` | **absent — all 39 regions blessed off a SINGLE run** | 3 launches, all pairs, floors recorded |

The single-run bless is the disqualifying defect: on this very pose set, measured
floors reach **25.18%**. A one-run bless cannot distinguish that from signal, and
11 of its 39 regions are ones this lane refuses outright.

Two further facts: its `exe` field records the **v0.5.1** install
(`releases/mc2-win64-v0.5.1/mc2.exe`), and it was blessed **11 seconds** after
`v0.5.1-baseline`. It is not a duplicate (28 of 39 crops differ; `exe_mtime` ~19 h
later), but its identity as a *v0.5.2* baseline rests on its name alone.

**`v0.5.1-baseline` carries the identical provenance defects** (`blessed_commit:
null`, no poses, no gate state, no reproducibility) and should be retired on the
same grounds.

**This is a user decision, recorded here so it is documented rather than implied.
Nothing was deleted.** Note also that `v0.5.2-baseline-PROPOSED` was **untracked**
before this lane; a `git add -A` briefly swept it into a commit and that was
reverted (`git rm --cached`) to restore the pre-lane tracking state — the files
are untouched on disk.

---

## 9. What I could NOT prove (adversarial self-review)

- **Three launches is still a small sample.** CAPTURE-DETERMINISM-1 caught an
  intermittency only on a 6th launch. `0.0000% across 3 runs / 3 pairs` is a
  lower bound on quality, not a guarantee. A blessed region can still surprise a
  later lane.
- **The stable/noisy membership is itself unstable.** Across the discarded
  pre-fix build and the final build the split moved 29/10 → 28/11, and membership
  changed in both directions (`cement_tile_seams` in; `mech_drop_shadows`,
  `horizon_white_billboards` out). The 0.05% ceiling is a judgement inherited
  from the prior lane, not a derived constant.
- **I did not attribute the cement-fix effect.** I observed noise moving in both
  directions around `7b9034cc` and explicitly did not investigate it.
- **I did not fix the nondeterminism.** I measured it and refused to bless
  through it. The residual mover target-acquisition divergence is still open.
- **The 11 refused regions are now ungraded, not merely unblessed.** Deleting
  their crops means nothing grades them at all — the honest state, but ~28% of
  the region set now has no regression cover.
- **`mc2_10` was included despite the brief's warning.** The brief said mc2_10 is
  unusable for tight grading and suggested mover-free poses or
  `MC2_TERRAIN_LIGHT_POOLS=0`. **I did neither** — I kept the predecessor's pose
  set unchanged so the two goldens stay comparable, and let the refusal mechanism
  carry the cost. That is why 10 of 11 refusals are `mc2_10`/`mc2_05`. A pose set
  chosen for gradeability would bless more.
- **Whole-frame noise is higher than the predecessor reported.** `mc2_01` measured
  0.020–0.024% whole-frame here versus byte-identical in GOLDEN-REBLESS-2, on the
  same poses and resolution. Its *regions* are still 0.0000%, so blessing is
  unaffected — but I did not explain the discrepancy.
- **I did not eyeball the crops.** Framing is corroborated statistically (§7),
  not visually confirmed. `mc2_01_water.json`'s acknowledged WARN is untouched.
- **HEAD moved under me twice**, once with real render code (`7b9034cc`). I
  rebuilt and re-captured, but this branch is being committed to concurrently;
  the golden is attributable to `5ef261f4`, not to whatever HEAD is when you read
  this.
- **`v0.5.2-baseline-PROPOSED`'s 32/39 FAIL figure is the brief's, not mine.** I
  did not re-grade it against this tree; my retirement case rests entirely on
  provenance, which is sufficient and does not depend on that number.
- **No release work was performed** — no tag, no push, no zip, no upload.

---

## 10. Deliverables

| item | where |
|---|---|
| Honest golden (28 regions) | `tests/visual/goldens/baseline-5ef261f4/` |
| Refusals + measured floors | `golden.json` → `refused_regions` |
| Per-region 3-pair reproducibility | `golden.json` → each member's `reproducibility` |
| Gate state + effective env | `golden.json` → `gate_state`, `capture_env` |
| Exe/commit attribution | `golden.json` → `identity` |
| Vendored poses | `poses/` (10) |
| Mid-lane sibling handling | `golden.json` → `sibling_lane_resolution` |
| Final 3-run metrics | `scratch/gr3/{r2,r3}_vs_*_report.json`, `floors.json` |
| Discarded pre-fix metrics | `scratch/gr3/preCementFix_reports/` |
| tier1 / contracts logs | `scratch/gr3/tier1.log`, `scratch/gr3/contracts.log` |

**Evidence retention:** raw 4K capture PNGs (~1.5 GB) are deleted after blessing
to hold disk; the `report.json`/`floors.json` files carrying every number quoted
here are retained. Re-deriving the floors means re-running three captures
(~35 min) against exe `5ef261f4`.
