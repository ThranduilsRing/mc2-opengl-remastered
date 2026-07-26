# GOLDEN-REBLESS-2 — STATUS

Re-bless the visual goldens honestly, on the deterministic path, from an
attributable build; audit pose honesty; record the gate state; and report (not
perform) the release re-cut.

**Outcome: 27 of 39 regions blessed, 12 refused.** The refusals are the point.
Every refused region carries a measured run-to-run noise floor from three
launches of an *identical* build, so "we could not bless this" is a number, not
an opinion.

New golden: **`tests/visual/goldens/baseline-6c47e149/`**.
Contracts **69/69** at close.

---

## META REVIEW — premises checked against source, not the brief

| brief premise | verdict |
|---|---|
| "All **16 blessed** golden sets record `MC2_SMOKE_FIXED_TIMESTEP: <unset>`" | **PARTLY FALSE, and the real story is worse.** `tests/visual/golden-sets.json` holds 16 sets but they are **8 blessed / 5 candidate / 3 retired**, not 16 blessed. All 16 *do* record `<unset>` — and **all 16 also carry `git.dirty: true`**, not "several". |
| Those goldens are the thing to re-bless | **FALSE — they are already dead.** All 16 `path` entries point into `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\visual\baselines\…`, a worktree that no longer exists. **16/16 dangling.** That registry cannot be re-blessed; it can only be declared defunct. |
| Every golden was captured on the nondeterministic path | **FALSE for the live goldens.** The live system is `tests/visual/goldens/` driven by `tools/visual_verify/`. `capture.py` defaults `det_clock=True`; `run_visual_capture.py` sets `MC2_SMOKE_MODE=1`, `MC2_SMOKE_SEED=0xC0FFEE`, `MC2_SMOKE_FIXED_TIMESTEP=1`. Both live goldens record `deterministic_clock: true`. The `<unset>` claim is true **only** of the dead registry. |
| The live goldens are fine, then | **NO — they fail differently.** `blessed_commit: null` on both (unattributable), **no env or gate state recorded at all**, and no per-region reproducibility evidence. That is what this lane fixed. |
| The 4 fraudulent bookmark fixtures were deleted | **CONFIRMED**, and `check-visual-bookmark-honesty.py` **passes** (exit 0; 18 clean + 1 acknowledged WARN on `mc2_01_water.json`). |
| `bookmark_honesty` covers the golden poses | **FALSE.** The checker globs `tests/visual/bookmarks/` only. Golden poses come from `<install>/saved_bookmarks/` — **disjoint sets**. It would not catch a fraudulent *golden* pose. Audited by hand instead (§5). |
| mc2_03/mc2_05/mc2_24 "ship only ONE real bookmark (the second is a literal `_dup`)" | **TRUE but not a defect.** Each file's own `_note` says: *"Duplicate entry keeps >=2 bookmarks (single-pose files silently fail)."* A documented engine workaround, not manufactured evidence. |
| mc2_10 nondeterministic, mc2_01 clean | **CONFIRMED and refined.** It is not per-*map*, it is per-*region*: 17 of 28 `mc2_10` regions are byte-stable. |
| `MC2_LETTERBOX_OPAQUE` is default-ON | **default TRUE, hygiene FALSE.** Default-ON confirmed at `code/gamecam.cpp:841`. But it is **UNREGISTERED, UNDOCUMENTED, and carries ENV_DROP_RISK**. `MC2_SSAO` is likewise UNDOCUMENTED. |
| IBL-NIGHT-SCALE-1 may land mid-lane | **IT DID.** See §7 — handled by discarding the pre-IBL golden and re-capturing. |
| Disk is the binding constraint | **CONFIRMED.** 14 GB at start, 13 GB at close. A release re-cut needs ~18 GB. |

---

## 1. What was blessed

One build, three independent launches, identical environment.

| | |
|---|---|
| exe | `A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe` |
| exe sha256 | `51478ef4fcfbd92bebf96c119bae3606c821afe47c0a06992d2091616de64602` |
| exe embedded git sha | **`6c47e149353c`, `dirty=false`** |
| deployed with | `deploy_payload.py --require-clean` → **PASSED**, manifest `src_commit 6c47e149353c`, 806 rows, PDB hash match |
| repo HEAD at bless | `709b1e48` |
| code equivalence | HEAD is 1 commit past the exe and is **docs-only** (`.claude/IBL-NIGHT-SCALE-1-STATUS.md`, 278 lines), verified by `git show --stat`. No C++/GLSL delta → exe **code-identical to HEAD**. |
| env | `MC2_SMOKE_MODE=1`, `MC2_SMOKE_SEED=0xC0FFEE`, **`MC2_SMOKE_FIXED_TIMESTEP=1`**, `MC2_LOG=1`, `MC2_NO_LAUNCHER=1`, `MC2_HEARTBEAT=1` |
| pose settings | trigger frame 431, settle 150, 3840x2160 |
| engine confirmation | **30/30 pose captures reported `fired=True det=True`** |

All recorded in `golden.json` (`identity`, `capture_env`) so the next person can
tell exactly what was blessed.

---

## 2. The bless rule, and why 3 runs and not 2

> A region is blessed **only** if it is reproducible across three independent
> launches of the same build. Ceiling `changed_pct <= 0.05 %`, worst of
> (r1-vs-r2, r1-vs-r3).

Two runs are not enough, and this lane **measured** that rather than citing it —
on this build:

| region | r1-vs-r2 | r1-vs-r3 | what a 2-run check would have done |
|---|---|---|---|
| `mc2_10_saved_204611 / horizon_white_billboards` | **0.0000 %** | **1.0163 %** | **blessed it — it looked byte-perfect** |
| `mc2_10_saved_204753 / lamp_cones` | 14.65 % | 0.13 % | noise is bidirectional |
| `mc2_10_saved_204907 / spotlight_towers` | 14.86 % | 20.04 % | caught, but understated by 5 pts |

This independently reproduces CAPTURE-DETERMINISM-1's central finding — *"any
N-runs-agreed claim systematically understates the floor"* — on a different
build, different poses, and a different instrument. It also reproduced on the
**pre-IBL** build earlier in this lane, where `lamp_cones` was 1.07 % on
r1-vs-r2 and 13.80 % on r1-vs-r3 (`scratch/gr2/preIBL_reports/`).

---

## 3. Result: 27 blessed, 12 refused

**Blessed (27):** all 7 `mc2_01`, 3 of 4 `mc2_05`, 17 of 28 `mc2_10`. Every one
**byte-identical (0.0000 %)** across all three runs except
`runway_grid_complete` 0.0247 %, `distant_terrain_horizon_band` 0.0018 %,
`cement_pads_dark` 0.0002 %.

**Refused (12)** — recorded in `golden.json` → `refused_regions` with their
floors; crops **deleted** so nothing can silently grade against them:

| bookmark | region | worst floor |
|---|---|---|
| mc2_10_saved_204907 | `spotlight_towers` | **20.04 %** |
| mc2_10_saved_204753 | `runway_edge_lights` | **18.14 %** |
| mc2_10_saved_204753 | `light_clusters_census` | **15.07 %** |
| mc2_10_saved_204753 | `lamp_cones` | **14.65 %** |
| mc2_10_saved_204611 | `far_water_surface` | **12.95 %** |
| mc2_05_saved_205843 | `light_pools_feathered` | **8.67 %** |
| mc2_10_saved_204611 | `horizon_white_billboards` | 1.02 % |
| mc2_01_saved_205644 | `mech_drop_shadows` | 0.58 % |
| mc2_10_saved_205952 | `water_shore_grazing` | 0.24 % |
| mc2_10_saved_212258 | `cement_tile_seams` | 0.10 % |
| mc2_10_saved_204523 | `compound_light_wedges` | 0.10 % |
| mc2_10_saved_212258 | `light_pools_sourcing` | 0.08 % |

Consistent with the mover attribution: the refused set is dominated by **light
pools, lamp/searchlight cones, spotlights, water and mech drop-shadows**.

**Validation.** `verify.py` grading r2 and r3 — neither of which produced the
golden — against it: **27 PASS / 0 FAIL, exit 0, both runs.** Integrity: 27/27
crop sha256 match, **0 zero-byte files**, 0 orphan crops.

---

## 4. Gate state blessed under

**Engine defaults. No `MC2_*` render gate was overridden.** Verified via
`mc2-repo-intel gate_status` against `RendererFeatureRegistry.h` +
`docs/tier1_env_vars.md`, not from the brief.

Default-**ON**: `MC2_LIGHT_GRID`, `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN`,
`MC2_STATIC_PROP_FIXTURE_LIGHTS`, `MC2_LETTERBOX_OPAQUE`,
`MC2_LIGHT_POOL_MAX=256`, **`MC2_STATIC_PROP_IBL_NIGHT_SCALE=0.25`** (landed
mid-lane).

Default-**OFF**: `MC2_LIGHT_REALISM`, `MC2_PAVED_OVERLAY_CLASS`,
`MC2_STATIC_PROP_FIXTURE_GLOW`, `MC2_PROP_SHADOW_RECEIVE`, `MC2_SSAO`,
`MC2_CEMENT_LIT` (0.0).

Flipping any default-OFF gate ON is **expected** to fail regions here — a
reviewable drift, not a regression.

Two hygiene gaps found in passing (**not fixed — out of lane**):
- `MC2_LETTERBOX_OPAQUE` — **unregistered, undocumented, ENV_DROP_RISK** (absent
  from the `run_smoke.py` passthrough allowlist, so a gate-ON smoke may silently
  run the gate-OFF path).
- `MC2_SSAO` — registered but **undocumented**.

---

## 5. Pose honesty audit

The contract check passes but **does not cover the golden poses** (§META). Hand
audit against the same criteria:

- 10 files, **10 distinct poses, 0 cross-mission duplicate tuples** — the exact
  signature that exposed the original fraud is absent.
- All 10 carry a real provenance `_note` ("Saved in-game via Save Camera
  Bookmark button").
- Each `<name>_dup` entry is the documented engine workaround, not a second
  claimed pose.
- Framing corroborated by content: grading this build against
  `v0.5.2-baseline-PROPOSED` moved 40–96 % of pixels in the night-lighting
  regions. Empty terrain does not do that.

**Fixed:** the poses previously existed **only** inside a deploy install, which
is not version-controlled — if that dir were wiped the golden could never be
re-captured. They are now vendored to
`tests/visual/goldens/baseline-6c47e149/poses/`.

---

## 6. v0.5.2-baseline-PROPOSED is stale — do not promote it

Graded against the pre-IBL build earlier in this lane: **32 FAIL / 7 PASS**.
Worst: `night_base_lighting` 96.3 %, `near_shore_peninsula` 96.0 %,
`shore_upperright_inlet` 91.8 %, `light_pools_road` 80.6 %.

That is **real content drift, not noise** — the same regions measure 0.0000 %
run-to-run. It is ~70 commits of night-lighting work landing, and IBL-NIGHT-SCALE-1
has since moved it further. `v0.5.2-baseline-PROPOSED` no longer describes the
tree. **Recommend retiring it rather than promoting it**, in favour of
`baseline-6c47e149`. (Promotion of the PROPOSED set was a pending user gate; this
is the evidence that it should not be promoted.)

---

## 7. IBL-NIGHT-SCALE-1 landed mid-lane — and was handled

The brief warned this sibling could stale the goldens before they were blessed.
**It landed while this lane was capturing**: `7dd64716` + `6c47e149` (code), then
`709b1e48` (status doc — the lane is now complete).

`MC2_STATIC_PROP_IBL_NIGHT_SCALE=0.25`, **DEFAULT-ON**, night-scales the day-sky
SH-IBL probe on static props. Day is byte-identical *by arithmetic*
(`nightFactor == 0.0f` in daylight); night props darken materially.

**Handling:** a complete pre-IBL golden had already been captured, validated and
staged in this lane. It was **discarded** — its crops deleted — the tree rebuilt
at `6c47e149`, redeployed with `--require-clean`, and **all three runs
re-captured**. The blessed golden is post-IBL and **is not stale on arrival**.
The pre-IBL reports are retained as methodology evidence at
`scratch/gr2/preIBL_reports/`.

Incidental observation: dimming night props **reduced** noise in two regions —
`light_pools_sourcing` 28.84 % → 0.08 %, `cement_tile_seams` 21.32 % → 0.10 %.
Plausibly less dynamic range to diverge over. **Not investigated; not claimed as
causal.**

---

## 8. What I could NOT prove (adversarial self-review)

- **Three runs is still a small sample.** CAPTURE-DETERMINISM-1 caught an
  intermittency only on a 6th launch. My 27 "stable" regions are stable across
  **3** launches; the tail is unmeasured. `0.0000 % across 3 runs` is a lower
  bound on quality, not a guarantee. A region blessed here can still surprise a
  later lane.
- **The stable/noisy split is not fully stable itself.** Pre- and post-IBL both
  gave 27/12, but **membership shifted** (`runway_grid_complete` and
  `cement_pads_dark` moved into stable; `horizon_white_billboards`,
  `cement_tile_seams`, `light_pools_sourcing` moved out). Some regions sit near
  the ceiling and could flip again. The 0.05 % ceiling is a judgement, not a
  derived constant.
- **I did not fix the nondeterminism.** I measured it and refused to bless
  through it. The residual mover-position divergence CAPTURE-DETERMINISM-1 left
  open is still open.
- **The `capture_determinism` ratchet does not cover this layout.** Run against
  `scratch/gr2` it reports **EMPTY** — it globs `r1/*.png` while `visual_verify`
  writes `r1/<pose>/*.png`. The contract ratchet **would not have caught these 12
  regions**; the 3-run comparison that did is a manual step I performed, **not
  enforced by anything**. A future lane can still bless noise.
- **`bookmark_honesty` still would not catch a fraudulent golden pose.** I
  audited by hand and vendored the poses, but did **not** extend the checker to
  cover `tests/visual/goldens/*/poses/`. The repeat remains possible.
- **The exe is not literally HEAD** (`6c47e149` vs `709b1e48`). I verified
  code-equivalence from `git show --stat` file lists, not by rebuilding at HEAD
  and comparing binaries.
- **I did not run tier1 on this build.** Contracts are 69/69, but the engine
  change in this build (IBL-NIGHT-SCALE-1) was smoked by *its* lane, not mine. I
  did not independently gate it.
- **Framing is corroborated, not proven.** I did not eyeball all 39 crops;
  `mc2_01_water.json`'s acknowledged WARN is untouched.
- **The release re-cut was not executed** (§9). The shortfall is computed from
  measured sizes, not from an attempted run.
- **Deploy-lane contention was not eliminated.** `0.4c` is claimed by four other
  registered lanes and two `mc2.exe` PIDs were briefly co-resident during the
  discarded pre-IBL run 1. The 27 blessed regions were byte-identical across all
  three post-IBL runs regardless, which bounds the concern without eliminating it.
- **HEAD moved twice under me.** I pinned and re-verified each time, but this
  branch is being committed to by other lanes concurrently; the golden is
  attributable to `6c47e149`, not to whatever HEAD is when you read this.

---

## 9. Release re-cut — REPORTED, NOT PERFORMED

Per `docs/RELEASE-PROCESS.md`. **Nothing was tagged, pushed, or uploaded. No zip
was built.** Steps 4 and 8 are user-gated by the runbook itself.

**Staleness.** `release_assets/0.5.2/` was cut at exe mtime 2026-07-23T23:50Z;
the tree has since moved ~70 commits, including night-lighting work that moves
40–96 % of pixels in the graded regions (§6). The zips are materially stale.

**Disk — this is the blocker.** Measured, not estimated:

| item | size | source |
|---|---|---|
| A: free at lane close | **13 GB** | `df` |
| new release zip set | **~7.3 GB** | measured 0.5.2 zip set |
| step-6 gate extracted install | **10.93 GB / 28,459 files** | `release_assets/0.5.2/release_install_manifest.csv` |
| **peak requirement** | **~18.2 GB** | zips + gate install co-resident |
| **shortfall** | **≈ 5 GB** | assuming zero slack |

`scripts/test_release_zip_install.py` (step 6, the mandatory gate) extracts a
full **10.93 GB** install. **A re-cut cannot complete on current disk.**

**Headroom, user-gated.** `release_assets` is 27 GB: `0.5.1` 12 GB, `0.5.2`
7.6 GB, `0.4` 3.2 GB, `0.4-testing` 5.0 GB. Superseded **`0.4` + `0.4-testing` =
8.2 GB** would take free space to ~21 GB — enough. **I did not delete them**;
`release_assets/` and `releases/` are user-gated.

**Process caveat already in the runbook** (still unfixed — verify manually before
publishing): `build_release.sh` predates the launcher and does **not** copy
`mc2-launcher.exe` or the shipped `mods/` tree into the engine zip, and
`test_release_zip_install.py` does not require them.

**Order of operations, if the user wants a cut:** free ≥8 GB → re-verify release
notes against the merged tree (step 0) → build/deploy `--require-clean` → tier1 →
Tracy baseline (§3a) → package → **step-6 gate** → then the user-gated tag and
publish.

---

## 10. Deliverables

| item | where |
|---|---|
| Honest golden (27 regions) | `tests/visual/goldens/baseline-6c47e149/` |
| Refusals + measured floors | `golden.json` → `refused_regions` |
| Gate state blessed under | `golden.json` → `gate_state` |
| Capture env + exe attribution | `golden.json` → `capture_env`, `identity` |
| IBL mid-lane handling | `golden.json` → `sibling_lane_resolution` |
| Vendored poses + honesty audit | `golden.json` → `pose_provenance`; `poses/` |
| Raw 3-run evidence (post-IBL) | `scratch/gr2/{r1,r2,r3}` + each `report.json` |
| Discarded pre-IBL evidence | `scratch/gr2/preIBL_reports/` |
