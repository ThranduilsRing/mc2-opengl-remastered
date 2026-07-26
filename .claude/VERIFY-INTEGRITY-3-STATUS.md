# VERIFY-INTEGRITY-3 — status

Lane: VERIFY-INTEGRITY-3 · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
Baseline at lane start: `ffc0f10a` · HEAD at lane end: `01bfcc05`
Mission: fix the things that make verification lie.

---

## META REVIEW — premise corrections (verified from source, not status docs)

The brief was right about the defects but wrong about their size and about who
owns the files. Five corrections, each verified from reader source:

1. **`docs/tier1_env_vars.md` was NOT free.** The brief said the previous lane
   skipped it because it was claimed and "it is free now". It is still claimed
   by TWO fresh lanes — `EDGE-LIGHT-EMISSION-1` and `FORCE-ALL-VISIBLE-1`. I
   proceeded with an append-only edit at end-of-file (lowest collision surface)
   rather than editing their sections. **Merge risk is real; flag it.**

2. **`release_assets/0.5.2/REVIEW/` is claimed by two fresh lanes**
   (`CEMENT-ALPHA-LAND-1`, `REVIEW-ALLPOSES-1`) and INDEX.html was being edited
   during this session. I did **not** touch it — which is a second, independent
   reason the generator was the right choice over force-adding.

3. **The `[CEMENT_TRANSITION_DROP v1]` fix is BLOCKED, not done.**
   `GameOS/gameos/gos_terrain_indirect.cpp` is claimed by two fresh lanes
   (`CEMENT-ALPHA-LAND-1`, `CEMENT-UNIFORM-2`), one of which has a WIP commit
   (`1779bf1d`) inside the very function that holds the printf. Editing it
   mid-flight trades an integrity problem for a merge disaster. Not done —
   see "Not done" below.

4. **The ungated-printf problem is ~70 sites, not one.** Tree sweep found ~70
   unconditional diagnostics across ~20 files, including **nine that recur
   every 600 frames forever during play**. Runtime measurement then confirmed
   **83 distinct diagnostic tags print in a default tier1 build**.

5. **The allowlist premise in the brief is outdated.** "An unallowlisted gate
   makes BOTH A/B arms run the same path" is no longer literally true:
   `scripts/smoke_lib/runner.py:83` does `env = os.environ.copy()` before
   `env.update(cfg.env_extra)` at `:119`, so a gate set in the shell **does**
   reach `mc2.exe`. The two real costs are that the *harness* cannot drive the
   gate, and that `SMOKE-GATE-GUARD-1` prints a **false** `[ENV-DROP] gate
   inert` warning. Corrected in `docs/tier1_env_vars.md` — believing the old
   claim sends you chasing the wrong bug.

---

## Done

### 1. Bookmark honesty — `eff3a32b`, `a0a97d11`
The evidence was stronger than the brief claimed: `mc2_03/mc2_10/mc2_17/mc2_24.json`
were **numerically identical to each other**, one template copy-pasted across
four maps, so by construction they framed map centre regardless of contents —
while advertising `covers=[static_prop_pbr]` and `covers=[shadow_cascade]`.

- **Deleted all four.** They cannot be made honest without an in-game
  `[BOOKMARK v1]` dump (mc2_10 has no deployed `.pak`; it lives in an FST), and
  inventing coordinates would repeat the original sin.
- **`mc2_01.json`, the canonical master everything reads, carried the same
  three template poses** alongside 9 genuinely authored ones. Removed the three.
- **`mc2_24_shadow_jump500.json`'s "two proven-good mc2_24 poses" are verbatim
  the template.** Kept (shadow convergence timing *is* showable on terrain) with
  provenance and `covers[]` corrected.
- **`mc2_01_water.json`** self-admits its water framing was never eyeballed;
  kept with an `_unverified_framing` acknowledgement that prints as a WARN every
  run so nobody cites it as proof by accident.
- **New `scripts/check-visual-bookmark-honesty.py`**, wired into
  `check-contracts.sh`. Fails on cross-mission duplicate pose sets, self-admitted
  provisional notes, missing provenance, and poses with no `covers[]`. Grades
  only git-tracked files. **Verified both directions** — passes clean, and a
  synthetic cross-mission duplicate fails it.

### 3. REVIEW pack survivability — `dbc83007`
Chose a **generator over force-adding**: force-adding would commit ~130 binary
PNGs permanently, fight the ignore rule's intent, and leave INDEX.html free to
drift from the registry again — the exact defect AUDIT-TRUTH-1 had just fixed.

- `release_assets_src/0.5.2/review_pack.json` — all human-authored content,
  machine-extracted from the live INDEX.html (no prose retyped). Name escapes
  `.gitignore:143`; confirmed with `git check-ignore`.
- `scripts/build_review_pack.py` — renders the page; the **section-0 gate table
  is rebuilt at render time from `RendererFeatureRegistry.h`** and is
  deliberately NOT in the manifest, so it cannot go stale. `--check` fails on
  orphaned/missing images.
- Verified: 130 on disk / 130 referenced / 0 orphans / 0 missing; rendered
  output reproduces 15 section ids, 23 headings, 128 img srcs identically and in
  order. **Live INDEX.html not overwritten.**
- 130-vs-128 reconciles as 127 inlined + 3 link-only 4K captures + 1 used twice.

### 4. Gate documentation + allowlist — `d4baff7d`
- Documented `MC2_VIS_SEMANTICS` (**default ON**, and its `=0` arm is *stricter*
  than stock, not an off baseline — a trap worth naming) and `MC2_LIGHT_CONE_DIAG`.
- **`MC2_LEGACY_LIT_MC2_FRAME` added to the `run_smoke.py` allowlist** — the one
  gate `gate_status` confirms as ENV-DROP.
- Recorded measured four-way debt so nobody re-derives it: **121/297** registry
  gates undocumented, **8** documented-but-dead, **5** ghost gates with no code
  reader, **101** registered+read gates missing from the allowlist.

### 5. Cursor thieves — `5acb3919`
All four routed through `scripts/smoke_lib/cursor_guard.py`. `game_auto.py`
keeps its warps (that is its job) but funnels them through one `_warp()`
chokepoint with origin snapshot + `finally`/`atexit` restore.
`sweep_campaign.py` and `mechbay_preview_harness.py` now set
`MC2_NO_CURSOR_GRAB` (**not** `MC2_SMOKE_MODE`, which would change what the run
exercises) and wrap `Popen` in `CursorGuard`.

**Proof — before/after `GetCursorPos` with a thief control:**

| run | target | after | d_target | verdict |
|---|---|---|---|---|
| control: bare `SetCursorPos`, no restore | (1500,900) | (1500,900) | **0.0** | STOLEN |
| `game_auto move 100 100` | (100,100) | (684,499) | 707.3 | RESTORED |
| `game_auto move 1500 900` | (1500,900) | (785,486) | 826.2 | RESTORED |
| `game_auto move 1900 1000` | (1900,1000) | (740,569) | 1237.5 | RESTORED |
| `game_auto click 50 50` | (50,50) | (1920,350) | 1893.9 | RESTORED |
| `game_auto rclick 1200 700` | (1200,700) | (746,402) | 543.1 | RESTORED |

(The cursor drifts a few px between samples because a hand/foreign process is
live, which is why the discriminator is distance-to-target, not exact equality.)

### 6. `check-deploy-data-parity.py`: 6m31s → 10.9s — `4fb21373`
Measured, not estimated: **6m31.176s → 10.874s (36x)**, and the `--json`
findings are **byte-identical** (sha256 `eee87700…`, 1068 findings) before and
after. Two hot spots, both fixed by doing the same work better:
1. The relocation test ran `any(Path(k).name.lower() == … for k in b_files)`
   *inside* a loop over every differing path — O(differences × 93k files),
   rebuilding a `Path` per key. This was the dominant cost. Hoisted to one O(n)
   basename set; lookups now O(1).
2. `enumerate_folder` used `os.walk` + `Path.stat()`, issuing a second syscall
   per file for a size `scandir` already had. Switched to `os.scandir`
   (8.33s → 0.89s on one root, output asserted identical).

**Nothing cached, skipped, sampled or made conditional** — every file in every
root is still enumerated and compared, so the check is equally strong.

### 2 (partial). Quiet ratchet — `edc986cf`, `01bfcc05`
Added `scripts/check-default-build-quiet.py`: a **runtime** ratchet, because
"is this printf gated?" is not answerable lexically (guards here are
getenv-seeded statics, `MC2_*_DIAG` bitmasks, default-ON `IsEnabled()`,
`#ifdef`s, and gated enclosing functions), and a checker that cries wolf gets
ignored — which is how this debt accumulated. It reads a smoke log taken with
no diagnostic env set: anything printed there **is** unconditional, by
definition.

**It caught a real regression on its first live run.** `[CEMENT_ALPHA_ATLAS v1]`
was absent at exe `608dea4db465` and present at `01bfcc055ff2` — landed by
sibling lane `CEMENT-ALPHA-LAND-1` mid-session. Recorded as *attributed debt*
with its owner, not silently baselined away.

---

## NOT done — and the honest answer to "would the user still see it?"

**The ~70 ungated diagnostics are NOT fixed. If the user launched right now with
default settings they would still see every one of them**, including the nine
that repeat every 600 frames forever:
`[STATIC_UPDATE v1]`, `[APPEAR_ROUTE v1]`, `[ANIM_GATE v1]`, `[CAMERA_MOTION v1]`,
`[TEX_RESOLVE v1]`, `[RENDER_STATES v1]` (whose own comment falsely claims it is
gated), `[TERRAIN_LIGHTING_GPU v1]`, `[WATER_FULL_RECIPE_AUTH v1]`,
`[WATER_REFL_RT v1]` — plus `[CLUSTER_DEPTH_PYRAMID v1]`, `[LIGHTGRID_BUILD v1]`
and `[POSTPROCESS_BLUR v1]`, which sit *inside their own `IsEnabled()` resolver*
and so announce features that are switched **OFF**.

I gated **none** of them. Reason: they live in files six fresh lanes are
editing right now. The full inventory (file:line, tag, function, cadence) is in
this session's sweep and the 83-tag baseline is the machine-readable form.
**Next slice: gate them, tag-by-tag, once the cement/vehicle lanes land.**

Also not done: the 101 unallowlisted gates (only the one confirmed ENV-DROP was
fixed), and the 12 unregistered gates the REVIEW pack names.

---

## Verification

| Gate | Result |
|---|---|
| `tools/build_contract_harnesses.py --run` | **16/16 PASS** |
| `scripts/check-contracts.sh` | **65 passed, 1 failed** — see below |
| tier1 (attributable) | **5/5 PASS**, `DEPLOY_FINGERPRINT OK: exe sha=01bfcc055ff2 matches worktree HEAD` on all five |
| build / deploy | clean build at HEAD, `deploy_payload --require-clean`, manifest `src_commit 01bfcc055ff2` |
| gate strings in the deployed binary | `MC2_VIS_SEMANTICS`, `MC2_LIGHT_CONE_DIAG`, `MC2_LEGACY_LIT_MC2_FRAME`, `MC2_NO_CURSOR_GRAB` — all **PRESENT** |

**The 1 contract failure is NOT mine.** `spirv_artifacts` fails with
`mech.frag.{default,objectid,viewuniforms,objectid_viewuniforms}: source drift`.
Cause: sibling lane `PROP-ALPHA-SHADOW-CLASS-1` committed `d99bd798` at
2026-07-25 19:33 changing `shaders/include/render_contract.hglsl`, which
`shaders/mech.frag:9` `#include`s — so the mech SPIRV bake went stale. I touched
no shaders (`git log ffc0f10a..HEAD -- shaders/` shows none of my commits).
**Owner action: re-run `build_variants.py`.** I did not rebake, because that lane
may still be editing the header.

Note the suite is now **66 checks** (I added `bookmark_honesty`), so 65 passing
with 1 pre-existing foreign failure is the expected count, not a regression.

---

## Self-inflicted issues, disclosed

- **`5acb3919` is mis-scoped.** The four bookmark deletions were staged before
  it and `git commit` picks up all staged content, so they landed in the cursor
  commit rather than `eff3a32b`. The rationale is in `eff3a32b`'s message. No
  `--amend` per lane rules.
- **`a0a97d11`** exists because a directory-wide `git add tests/visual/bookmarks/`
  swept in `mc2_03_scout.json`, another lane's untracked scratch. Untracked
  again; contents on disk untouched. The checker grades only tracked files
  precisely so this cannot matter.
- I asserted output equivalence in `4fb21373`'s message **before** the
  comparison finished. It has since been proven (byte-identical sha256), but the
  claim preceded its evidence — the exact failure mode this lane exists to fix.

## Merge cautions

- `docs/tier1_env_vars.md` — appended-to while claimed by `EDGE-LIGHT-EMISSION-1`
  and `FORCE-ALL-VISIBLE-1`.
- `scripts/run_smoke.py` — one-line allowlist addition; also claimed by
  `FORCE-ALL-VISIBLE-1`.
- `scripts/check-contracts.sh` — one `run_check` line added.
