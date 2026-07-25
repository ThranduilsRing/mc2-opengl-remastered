# AUDIT-TRUTH-1 — STATUS

Lane: `AUDIT-TRUTH-1`. Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`.
Base: `ce68d5e4`. Head after lane: `808d3730` (+ verification commits, see below).
Source of work: `A:/Games/mc2-opengl-src/.claude/SESSION-AUDIT-2026-07-25.md` §3, §4, §5.

Scope: **correctness and truth defects only.** No taste work, no new visual features.

---

## Per-task disposition

| Task | Verdict | Commit |
|---|---|---|
| 1. Ungate the cement diagnostic | **DONE** | `31ff3e8d` |
| 2. Stop docs recommending rejected work | **DONE** | `a0c49ee8` |
| 3. Make the REVIEW pack tell the truth | **DONE (untracked file — see caveat)** | n/a, `release_assets/` is gitignored |
| 4. Codify the standing capture rule (+ bookend reviews) | **DONE** | `808d3730` |
| 5. Bake the user's approvals | **DONE** | `32b164eb`, `cdd91ad7`, `a441b2fa` |
| 6. Fix deploy attribution | **BLOCKED — cannot be done, see below** | — |

---

## TASK 1 — ungated diagnostic (`31ff3e8d`)

`a25037cf` shipped `[CEMENT_PAVEMENT_GAP v1]` as an **unconditional `printf` plus a
per-tile `getOverlay` walk** inside `PopulateRecipeCementWords()`. Its own comment
said "no gate". It ran on **every mission load in a default build**.

Folded into the existing registered diag facility rather than inventing a new gate:
**`MC2_TERRAIN_LOD_CHUNK_DIAG` bit `524288` (0x80000)**, previously unused
(existing bits: 1, 40, 41, 1024, 32768, 65536, 262144). Unset ⇒ the walk **and** the
print are both skipped ⇒ byte-identical to the pre-`a25037cf` render.
Documented in `docs/tier1_env_vars.md` beside the other cement diag bits.

**Sibling coordination.** `GameOS/gameos/gos_terrain_indirect.cpp` carried 13
uncommitted lines from **CEMENT-UNIFORM-2** (alpha cells leaking into the solid layer
map, in `BuildCementCatalogAtlas()` ~L1697–1830). Their work was **not committed, not
reverted, not absorbed**. My edits are in a disjoint region (~L1228–1370), and I staged
**only my three hunks** via a line-range-filtered patch applied with `git apply --cached`.
Verified after commit: the sibling's +13 remain unstaged in the worktree.

> **Left alone deliberately:** the neighbouring `[CEMENT_TRANSITION_DROP v1]` printf is
> *also* unconditional ("census (always)"), and is documented as printing regardless of
> its gate. It predates this lane and was outside the named scope. **It is still an
> ungated per-mission-load print** and should be given the same treatment.

---

## TASK 2 — docs recommending rejected work (`a0c49ee8`)

- `docs/tier1_env_vars.md:725` opened with `MC2_CEMENT_TONE_MATCH` "**Cures the
  concrete patchwork**". Now leads with an explicit **USER-REJECTED / DO-NOT-PROMOTE**
  banner ("after is WAY worse"), mechanism text demoted to what it *attempts*.
- `MC2_RUNWAY_TONE_MATCH` given the same banner (inherits the verdict; additionally
  census-proven inert on stock maps, `dark_cells=0`).
- `.claude/SESSION-GATE-INVENTORY-2026-07-24.md:52` graded TONE_MATCH "**Candidate**"
  for promotion → now **REJECTED, keep OFF**. Section headers/counts and note A
  reconciled with the source changes this lane made.
- `docs/tier1_env_vars.md:522` and the `MC2_RUNWAY_EDGE_BLEND` **registry description**
  both still described **rev-1 erosion**. Corrected to the shipped **rev-2 dilation**.

**Premise check (and a correction to the brief).** The brief said the registry
description was stale. I verified from source and git: `RendererFeatureRegistry.h`
already recorded **both tone-match rejections correctly** — only the
`MC2_RUNWAY_EDGE_BLEND` registry entry was stale. Rev-2 confirmed shipped by
`8e59de64` ("DILATE concrete outward, not erode it") and `3c6aad71` (reach 0.5→1.0
tile). Also corrected a *path* claim the docs got wrong: the dilation lives in
`terrain_lod_chunk.frag` (the **LIVE** renderer); `gos_terrain.frag` (indirect) had
the rev-1 erosion **removed** and is plain full replacement.

---

## TASK 3 — REVIEW pack (`release_assets/0.5.2/REVIEW/INDEX.html`)

**§0 rebuilt from READER SOURCE.** The old table listed `MC2_LIGHT_MOVER_VIS_GATE`
and `MC2_LIGHT_CONE_VIS_GATE` as default-**ON**; `89f0ed4f` had **parked both
default-OFF** (`cone_glow.cpp:171,185`). It misreported in the dangerous direction — a
reviewer would have blessed v0.5.2 believing the light leak was fixed when it is
default behaviour in every binary that exists. Restructured into:

- **0a** gates that actually ship default-ON (what a normal launch executes)
- **0b** gates CORRECTED, with the old claim shown beside the source truth
- **0c** gates that are default-OFF, i.e. **NOT delivered**

Added gates §0 omitted entirely: `MC2_VIS_SEMANTICS`, `MC2_FORCE_ALL_VISIBLE`,
`MC2_CEMENT_ALPHA_ATLAS`, `MC2_CEMENT_LIT`, `MC2_RUNWAY_TONE_MATCH`,
`MC2_STATIC_PROP_NIGHT_TAGS`, `MC2_CEMENT_MISSION_LIGHT`, `MC2_LIGHT_CULL_RADIUS`,
`MC2_SHADOW_ALT_SCALE`, `MC2_SHADOW_FADE_RAMP0`, and `MC2_ROAD_PBR` (**no reader
exists**). Added the `HKCU:\Environment` contamination warning and the
`MC2_FORCE_ALL_VISIBLE` harness-blinding hazard.

**§12** no longer sells a rejected arm: the "Recommended" row and both A/B captions now
lead with the rejection; the figure is kept as a record of what was refused. Added the
class-diag caveat (paved `&Overlays` tiles over a non-cement base are absent from the
drop denominator, so the instrument was partly misleading the user).

**Orphans wired.** 5 `12_mc2_24_cement` **full frames** into §12 (only crops were
linked — violating the pack's own full-frame rule); the other 43 into a **new §13**,
grouped by lane with arm-accurate captions, plus a TOC entry.

**Validated:** 130/130 images referenced, **0 broken paths**, all tags balanced
(section/table/figure/figcaption/div/p/h2-h4/tr/td/th/a/b/code/i).

### Brief premises corrected (verified against disk)
- **`edge_light_emission_1` has NO README.** The brief said "10 PNGs with its README".
  There is no non-PNG file in that directory and no mention of `edge_light_emission`
  anywhere in the tree. Captions are **inferred from filenames and image geometry** and
  are labelled as such on the page.
- **The orphan count was 48, not 20.** The extra orphans were 18 `light_realism_1`
  files — including **pose 212019 in its entirety, full frames and all** — and 6
  `09_runway_marker_lights` arms. A stray `INDEX.html.pre-allposes.bak` suggests that
  wiring was started and abandoned mid-run.

### ⚠ Caveat — this work is NOT under version control
`release_assets/` is in `.gitignore`, so `INDEX.html` **could not be committed**. The
edits are on disk only. A pre-edit copy is at
`…/scratchpad/INDEX.before-audittruth.html`. If the review pack matters, it needs to
be either force-added or re-generated from a tracked generator.

### Lane coordination
`REVIEW-ALLPOSES-1` claims this file. Registry shows it (and every other named sibling)
registered under **the same PID as this session**, i.e. prior *sequential* lanes of one
session, all ≥1.8h stale, and `INDEX.html` was **clean in git** — no live holder. Edited
directly rather than handing off.

---

## TASK 4 — standing rules codified (`808d3730`)

**Standing Capture Rule** previously existed as one paragraph in an **untracked** file
in the ROOT repo (`SESSION-LEDGER-2026-07-18.md:369`, `git status` `??`) — the repo
whose own CLAUDE.md says "do not take rules from here", one `git clean` from oblivion,
zero hits in `mc2-main`. Now in `docs/visual-verify.md` as the required grading
protocol: full-frame wide verdict shot at normal gameplay altitude, ≥3 locations per
map including a control area, day AND night, across mc2_01/mc2_03/mc2_10/mc2_24.

The contradicting `docs/visual-verify.md` guidance ("Goldens: crops, not frames —
crop-only is the default and recommended path") was **resolved rather than deleted**:
crop-only is correct for **automated regression diffing**, and that section is retained
but explicitly scoped, with a table separating it from human verdict grading. Recorded
the anti-patterns that burned us.

**Subagent bookend reviews** (additional standing rule received mid-lane) added to
`docs/disciplines.md` as two imperative checklists — META REVIEW before starting,
ADVERSARIAL SELF-REVIEW before reporting done, the latter leading with *"if the user
launched the game right now with default settings, would they still see this?"*

**Propagated so future dispatches inherit both:** pointers in the canonical
`CLAUDE.md` preflight section, and fillable sections appended to
`docs/LANE_COMPLETION_REPORT.template.md` including an explicit
`user_sees_it_on_default_launch` field and `premises_corrected`.

---

## TASK 5 — user approvals baked in

### Light realism variant B (`32b164eb`)
The user graded A vs B and chose **B**; source shipped **A**, so enabling the gate gave
the arm they did *not* pick. All six constants are function-local `static` lambdas with
env sub-overrides, so this is a pure default change:

| constant | A (was) | B (now) | file |
|---|---|---|---|
| `realism_cone_opacity` | 0.55 | **0.40** | `mclib/cone_glow.cpp:256` |
| `realism_cone_feather` | 1.5 | **2.5** | `mclib/cone_glow.cpp:269` |
| `s_lightRealismPoolSpill` | 1.6 | **2.2** | `gos_terrain_lighting.cpp:952` |
| `s_lightRealismPoolCore` | 0.15 | **0.0** | `gos_terrain_lighting.cpp:960` |
| `realism_beacon_alpha` | 45 | **70** | `mclib/cone_glow.cpp:647` |
| `realism_beacon_radius` | 28 | **40** | `mclib/cone_glow.cpp:659` |

`MC2_LIGHT_REALISM` itself **stays default-OFF** — flipping it costs −34% avg night FPS
and is the user's call. **No default-visible change** (all six are consumed only inside
paths the OFF gate skips). The competing **`CONE_OPACITY=0.15`** proposal is recorded
as an **alternative** in the code comment and docs, not as the default.

### Shore meander 75 (`cdd91ad7`) — DELIBERATE default-visible change
`ShoreMeanderWu()` returned `0.0f` unset, so the user-approved `A=75` shipped
**disabled**, with the approval living only in a status doc — and this pack's own A/B
drove amplitude **1**, not 75. Unset now yields **75.0f**; explicit `=0` still restores
the legacy staircase. **Noted in docs and in the REVIEW pack: mc2_01's waterline creeps
~10% further up the beach at 75.** GL path only; Vulkan water untouched.

> Note: there is no separate enable gate — the amplitude *is* the gate — so "gate can
> stay as-is" resolves to "explicit `=0` still disables".

### `MC2_LIGHT_BILLBOARD_TWOSIDED` retired (`a441b2fa`) — DELIBERATE default-visible change
**Flipped to default-OFF; NOT deleted.** Deletion was rejected because the gate only
ever *adds* faces to `listOfVisibleFaces`, two call sites still consume it
(`tgl.cpp:2591`, `:3053`), and it is the only way to A/B the two-sided hypothesis. OFF
== the stock/legacy backface cull, so this moves the default **toward** stock. CPU
transform path only; the GPU static-prop equivalent `MC2_LIGHT_BILLBOARD_FACING` is a
separate gate and already default-OFF.

---

## TASK 6 — deploy attribution: **BLOCKED, stopped as instructed**

The task's own escape clause applies verbatim: *"If the tree cannot be made clean
because a sibling holds uncommitted source, say so and stop — do not force it."*

`git status --porcelain -uno` at lane end returns exactly one line:

```
 M GameOS/gameos/gos_terrain_indirect.cpp
```

Those are **CEMENT-UNIFORM-2's 13 uncommitted lines**, which I am explicitly forbidden
to commit, revert or absorb. While they sit there **every build is fingerprinted
`-dirty`**, so no clean-fingerprint build exists to deploy and `--require-clean` will
refuse. **Not attempted. Not forced.**

The defect stands and is unfixed: `mc2-win64-v0.4d-rc1/mc2.exe` self-reports
`98ba41555e3f` while all 806 rows of its `.deployed_manifest.csv` say
`6aaba337…-dirty`, and `98ba4155` is an **ancestor of** `89f0ed4f` — so that exe does
not contain the parking commit its lane claimed. **Anything graded from that lane
remains unattributable.**

**To unblock:** CEMENT-UNIFORM-2 commits or discards its 13 lines. Then one clean
build (`MC2_BUILD_GIT_DIRTY 0`) + `deploy_payload.py … --require-clean`, md5-verified.

---

## Verification

| Gate | Result |
|---|---|
| `tools/build_contract_harnesses.py --run` | **16/16 PASS**, 0 not-passing (12/12 built, 0 failed) |
| `scripts/check-contracts.sh` | **65 passed, 0 failed** (exit 0) |
| `run_smoke.py --tier tier1 --duration 30` | **5/5 PASS** (mc2_01/03/10/17/24, 0 destroy-delta, MOVER-COHERENCE 0 violations) — **but see the fingerprint caveat below; this did NOT test my build** |
| Build (`cmake --build build64 --target mc2`) | **exit 0** — all source edits compile |

**Baseline clarification.** 16/16 and 65/65 are **two different suites**, not a
disagreement: 16 is the C++ contract-harness count from
`build_contract_harnesses.py`, 65 is the check count from `check-contracts.sh`.
Both were run and both pass. Do not use either number as a bar for the other.

### ⚠ THE tier1 5/5 DOES NOT VERIFY THIS LANE — read before citing it

The run reported `PASS (5/5 passed)` and then:

```
[DEPLOY_FINGERPRINT] HARD FAIL: smoke ran an exe whose fingerprint does not match
this worktree HEAD (stale / wrong-lane / built-from-dirty).
  exe sha=608dea4db465   expected=808d37304cb5
```

The smoke exercised the **already-installed `608dea4d` exe**, which predates every
commit in this lane. So:

- **It is a valid no-regression signal for the existing install.** It is **not**
  evidence that anything this lane changed is correct.
- **None of the three default-visible changes were executed by that binary** —
  not shore-meander 75, not the TWOSIDED retirement, not the diag gating.
- This is precisely the failure class the audit named ("verified via smoke" claims
  that are void) and that the new bookend checklist exists to catch.

I could not fix it: deploying my build requires a clean tree, which is blocked by
CEMENT-UNIFORM-2's uncommitted source (TASK 6). **Do not cite this tier1 as
verification of AUDIT-TRUTH-1.** The build itself compiled clean (`exit 0`), which is
the only positive signal this lane has for its own source changes.

### Environment contamination confirmed live

The runner emitted, for every mission:

```
[ENV-DROP] WARNING: MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE='1' ... NOT in the smoke allowlist
[ENV-DROP] WARNING: MC2_GPU_CULL_STATIC_FROZEN_RECORDS='1'   ... NOT in the smoke allowlist
[ENV-DROP] WARNING: MC2_STATIC_POP_SPLIT='1'                 ... NOT in the smoke allowlist
```

This is direct confirmation of audit §3(e): those three presence-keyed vars really are
set in the machine environment (`HKCU:\Environment`) and really do reach every normal
launch. **Clear them before grading anything.**

### Foreign process note

`cursor-guard` reported a foreign `mc2.exe` (pid 39312) from another lane throughout.
It was **not touched** (own-PID kills only). The guard was **not disabled**.

### Default-visible behaviour change audit
Exactly **three** deliberate items, all TASK 5, all user-approved or stock-restoring:

1. `MC2_WATER_SHORE_MEANDER` default `0.0` → **75** (user-approved).
2. `MC2_LIGHT_BILLBOARD_TWOSIDED` default **ON → OFF** (retires a non-stock,
   never-approved default; OFF is the stock cull).
3. `[CEMENT_PAVEMENT_GAP v1]` census **stops running** on every default mission load
   (removal of an unintended default behaviour — strictly a restoration).

Everything else is either docs-only or sits behind a default-OFF gate:
the six light-realism constants are unreachable while `MC2_LIGHT_REALISM` is OFF.

---

## Open / not done

- **TASK 6** — blocked on CEMENT-UNIFORM-2 (above).
- **`INDEX.html` is untracked** (`release_assets/` gitignored) — TASK 3's work is not
  under version control.
- **`[CEMENT_TRANSITION_DROP v1]` is still an ungated per-mission-load printf.**
  Out of named scope; same class of defect as TASK 1.
- `scripts/check-claude-md-pointer.sh` reports FAIL against the **ROOT** repo's
  `CLAUDE.md` (`A:/Games/mc2-opengl-src/CLAUDE.md`). **Pre-existing and untouched by
  this lane** — I edited `A:/Games/mc2-main/CLAUDE.md` only.
- Captions in REVIEW §13a are **inferred** (no README exists for that capture set).
- Nothing in this lane was verified **visually**. Per the very rule this lane codified:
  the three default-visible changes above have **not** been graded against the Standing
  Capture Rule (no full-frame wide shots, no day+night, no 4-map sweep). That grading
  is impossible until the tree is clean enough for an attributable build — the same
  blocker as TASK 6.
- **NEVER pushed.** No `--amend`. All commits local.

---

## Late correction — a sibling landed mid-lane (`ceb5bb06`)

`ceb5bb06` (**VIS-SEMANTICS-FINISH-2**, 15:04) landed between my preflight and my
first commit, becoming the parent of `31ff3e8d`. Caught by re-reading `git log` at
lane end, which is exactly the "check whether a later commit already superseded the
task" item in the new META REVIEW checklist.

It **partially supersedes** a claim I had written into REVIEW §0b. The stale-emitter
turn guard in `QueueConeGlow` no longer consults the parked `MC2_LIGHT_CONE_VIS_GATE`;
it now arms off `vis_job_a_armed()` = `MC2_VIS_SEMANTICS` (default ON) ||
`MC2_LIGHT_CONE_VIS_GATE=1`, disarmed by `MC2_FORCE_ALL_VISIBLE=1`.

**§0b corrected accordingly.** What still stands, verified against source at lane end:

- `MC2_LIGHT_MOVER_VIS_GATE` and `MC2_LIGHT_CONE_VIS_GATE` **are still PARKED
  default-OFF** (`cone_glow.cpp:168`, `:182`). The original §0 defect — the page
  claiming both were default-**ON** — was real and is fixed.
- The cure now rides on `MC2_VIS_SEMANTICS`, which per §0a is **unverified and present
  in no deployed binary**. So it is a source-level fix awaiting a build and an A/B, not
  something the user has today.
