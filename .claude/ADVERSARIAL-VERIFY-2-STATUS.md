# ADVERSARIAL-VERIFY-2 — status

Lane: ADVERSARIAL-VERIFY-2 (refute, do not build). Worktree `A:/Games/mc2-main`,
branch `claude/nifty-mendeleev`, HEAD `248cecc2`. No code changes; this doc only.

Mission: audit what landed since `ADVERSARIAL-VERIFY-1` (`cf97ed52`), and close
the gap that pass left open — it measured **no** effect independently.

## 0. Headline

- **Contracts 74/74**, run by me (`scripts/check-contracts.sh`), 0 failed.
- **The reference build cannot measure any of the audit set.** `pe_gate_scan.py`
  on `mc2-win64-refbuild-cf97ed52/mc2.exe`: **0/6 present, 6 absent**, self-test
  OK (pos x1 / neg x0). `cf97ed52` (21:43) predates all six audited commits. The
  brief's premise that this reference can reproduce a claim is **false**.
- **I did close the measurement gap**, but on a different artifact: I
  independently re-measured the overlay-cement A/B pair and then tried to refute
  the lane's own explanation of the residual. It survived.
- **Biggest live risk is not in any audited gate — it is in the grader.**
  `tools/visual_verify/verify.py` silently nearest-neighbour-upscales a candidate
  crop to golden dimensions. Nothing anywhere asserts capture resolution.

## 1. Verdict table (ranked by user impact)

| # | Claim | Source of truth checked | My independent finding | Verdict |
|---|---|---|---|---|
| 1 | Golden `baseline-5ef261f4` is a sound reference | `tests/visual/goldens/baseline-5ef261f4/golden.json`; PNG IHDR bytes; `tools/visual_verify/{verify,bless}.py` | Counts **verified 28 blessed / 11 refused** (28+11=39 = all defined regions). Refused set has **0 files** on disk. Resolution **is 4K** — confirmed from pixels (crops 3456/3400 px wide; 3072x216), not just the field. All 28 sha256 match. BUT the **800x600 trap is real and open**: `verify.py:76-120` `diff_crops()` silently upscales on dim mismatch, no warn/FAIL. **No tool asserts resolution anywhere.** And every provenance block (`identity`, `capture_env`, `bless_rule`, `refused_regions`) is **hand-authored**, not tool-emitted — `bless.py` writes only `kind…members` | **OVERSTATED** — this baseline is genuinely 4K and clean, but nothing *makes* the next one so |
| 2 | Golden used all-pairs three-run rule, `bookmark_honesty` passing | grep across all `*.py`/`*.sh`; ran the checker | `bookmark_honesty` **is** enforced (`scripts/check-visual-bookmark-honesty.py`, wired at `check-contracts.sh:136`); I ran it, clean; the predecessor's glob gap is closed. **All-pairs three-run rule is PROSE ONLY** — zero code hits for `pairs_graded`/`worst_changed_pct`/the 0.05% ceiling outside the JSON itself. `golden.json:source_capture` points at `scratch/gr3/r1/capture_run.json` which **does not exist**; the authoritative per-run manifests are gone, so the three-run provenance is unfalsifiable from disk | **UNVERIFIABLE** (three-run rule); bookmark_honesty **CONFIRMED** |
| 3 | Reference build `refbuild-cf97ed52` can be smoked to reproduce a claim | `pe_gate_scan.py` (self-test OK) | **0/6 gates present.** All six audited commits postdate `cf97ed52`. `build64/RelWithDebInfo/mc2.exe` (00:51) has **5/5 present** and is the only binary that could measure this set — but it cannot run from the worktree | **REFUTED** (premise) |
| 4 | `MC2_SSAO_BLUR` default-ON; `aoRadius` 3.0→24.0 | `gos_postprocess.cpp:976-983, 767, 2858`; registry | Default **ON** confirmed (`!(blurEnv && blurEnv[0]=='0')`). `MC2_SSAO` is **OFF**, and `runSSAO()` does `if (!ssaoEnabled_) return;` at **:2858** → both are structurally inert in the shipped default. **Finding: `aoRadius_(24.0f)` at :767 is an UNCONDITIONAL member-default change, not gated by anything** — it is inert only because SSAO is off. Any path that enables SSAO (ImGui, per-mission profile, `gos_SetSsaoRadius`) gets 24.0, an 8× radius, with no gate to revert it. Tier1 for the owning commit `be445b02` is **VALID** (`OK: exe sha=31e1a9e81cc0`, matching) | **CONFIRMED but under-gated** |
| 5 | `MC2_OVERLAY_CEMENT_SHADOW_MATCH` — two-shader seam; own gate 0.0000% | `shaders/terrain_overlay.frag:107-129`; `gameos_graphics.cpp:10783-10789`; `scratch/cpc1/m24_{off,on}.png` | Seam claim **structurally confirmed** in source: overlay frag keys off bit31 of the *same* cement word the chunk frag uses, RenderDoc capture cited. Default-OFF path uploads 0 → first compare short-circuits. **I re-measured the A/B pair myself: 2.19% changed, maxAbs 245** — the lane reports 1.98% and attributes it to a moved vehicle. **I tried to refute that attribution** via connected components: 22 components, largest = **77.1%** of the diff, top-3 = **95.8%**. That is a compact-blob signature, not distributed pavement change. **The attribution survives.** The 0.0000% claim is correctly scoped in the doc to the three mc2_23 runway poses, not to mc2_24 | **CONFIRMED** (and honestly scoped) |
| 6 | `MC2_LIGHT_POS_COLUMN_FIX` — 7 consumers read a column the upload zeroes; fix is a measured null | `mclib/txmmgr.cpp:2418-2428`; `.claude/LIGHT-POS-COLUMN-1-STATUS.md` | **Half 1 CONFIRMED from source**: the OFF branch explicitly writes `[12]=[13]=[14]=0.0f`; the fix copies `[3],[7],[11]` (row 3 of each column = translation in `entries[(column<<2)+row]`) into the mat4 translation slots. Arithmetically correct, cannot double-correct. **Half 2 is narrower than the brief states**: the census found zero POINT/zero SPOT on **one pose set (mc2_05)**, and the lane itself discloses (§6) that the trace is once-per-session, may sample before night spotlights register, and that `carnage/weaponbolt/turret` dynamic lights — the case that would actually bite — were never tested. Note **INFINITEWITHFALLOFF (type 2)** is also an affected type and is not addressed by "zero POINT, zero SPOT" | **CONFIRMED but OVERSTATED as justification** — default-OFF is justified by *unproven benefit*, not by the null proving safety |
| 7 | `MC2_CEMENT_ROAD_ART_DETECT` — Arm B destructive (`canon_road_frac=0.312`); Arm B+C returns to OFF exactly 0.0% | `gos_terrain_indirect.cpp:1205-1211`; `scratch/cic1/classify.py` | Default **OFF** confirmed. Gate is a pixel no-op with other arms off. **The load-bearing number is NOT reproducible**: `classify.py` requires `<pfx>.rgba` + `<pfx>.csv` from `MC2_CEMENT_ATLAS_DUMP`, and **no `.rgba` or atlas `.csv` exists anywhere under `scratch/`**. Reading their classifier, the mechanism is self-consistent: `canon` selection skips cells with bright `marks` but **never excludes high-`roadfrac` cells**, so a road cell *can* be chosen as canonical by construction — which is exactly why `canon_road_frac` is non-zero. The destructive conclusion follows from the code; the *number* is unaudited | **UNVERIFIABLE** (evidence deleted); mechanism plausible |
| 8 | `MC2_CEMENT_ATLAS_DUMP` registration + `DEMOTED_ANCHORS` demotion is structural, not attrition | `scripts/check-sampler-bindings.py`; `git show 8df6c3dc` | Demotion premise **independently true**: `ssaoTex` is declared in `ssao_apply.frag:15` **and** `ssao_blur.frag:35`, bound to **two different units** (`gos_postprocess.cpp:2915`→1, `:2961`→0). `ssao_blur.frag` came from a real sibling feature (`be445b02`), not a checker dodge. **STRUCTURAL.** Two caveats: (a) the claimed `len(files) > 1` guard at `:344` is the **trip wire that fired**, not a gate on the demotion — `DEMOTED_ANCHORS` at `:122` is **inert, nothing reads it** (single grep hit, its own definition); (b) FAIL-level anchors dropped **3→2 with no compensating check**; re-keying by `(name,file,receiver)` to anchor both was available and not taken | **CONFIRMED with caveats** — structural, but the anti-attrition mechanism is documentation, not enforcement |
| 9 | Contracts 74/74 | ran `scripts/check-contracts.sh` | **74 passed, 0 failed** | **CONFIRMED** |

## 2. Trap sweep (the eight named traps)

1. `PASS (5/5)` beside `DEPLOY_FINGERPRINT MISMATCH` — **found one**: the
   LIGHT-POS/`31e1a9e8` lane. It is **honestly declared**, not laundered: the
   lane refuses to call it a pass and argues the delta is one `.md`. By the rule
   it is still **INVALID**. Note `ADVERSARIAL-VERIFY-1` said it did not enumerate
   the eight lanes — **that list is still unenumerated by anyone.**
2. Artifact-vs-exit-code — checked artifacts (`golden.json`, capture PNGs,
   `scratch/gr3/contracts.log`), not exit codes.
3. **800x600 golden trap — REAL AND OPEN.** See row 1. This baseline is 4K, but
   the grader has no guard.
4. Recorded-vs-measured floors — I measured one myself (row 5, 2.19% vs their
   1.98%: consistent). The three-run floors are **not code-enforced** (row 2).
5. Ambient `MC2_*` contamination — **CLEAN**. The three vars appear under
   `ambient_unset_by_operator`; `capture_env_declared` + `capture_env_ratchet`
   both PASS.
6. `MC2_DIAGF` needs `MC2_DIAG_VERBOSE=1` — noted; no conclusion here rests on a
   silent DIAGF probe.
7. Failed build printing `deploy COMPLETE` — no deploy performed this lane.
8. Commit scoop — spot-checked with `git log -S`. `RoadArtDetectEnabled`,
   `overlayCementShadowMatch`, `s_lightPosColumnFix` are all **sole-author,
   clean, live at HEAD**. Two real findings: `febe661a` shipped a registry row
   **without its enum slot** (added 74 s later by `31e1a9e8`, breaking a sibling
   lane's build), and `8df6c3dc`'s message headlines a registration row that
   actually landed in `c7e0499c` — **disclosed** in a parenthetical, not laundered.

## 3. Default-ON gates whose lane never ran a valid tier1

Only one gate in the audited set is default-ON: **`MC2_SSAO_BLUR`**. Its owning
commit `be445b02` **did** run a valid tier1 (`OK`, matching sha). **No default-ON
gate in this set lacks a valid tier1.**

Adjacent flag: `aoRadius_ = 24.0f` is a default-ON *behaviour* change that is not
a gate at all (row 4) and therefore has no gate-level tier1 story.

## 4. Conclusions reached via `strings`

**None in this lane.** All binary claims used `scripts/pe_gate_scan.py` with its
positive/negative self-test passing on every run. `strings` is not installed.

## 5. Adversarial self-review — what I could NOT verify, and why

- **No live effect measured on a running game.** The refbuild lacks all six
  gates (row 3); `build64`'s exe cannot run from the worktree; deploying to a new
  lane is user-gated. My one independent measurement (row 5) re-derives from the
  lane's own captured PNGs — **their pixels, my analysis**. That is weaker than a
  fresh capture and I do not claim otherwise.
- **`canon_road_frac=0.312` is unaudited** — the `.rgba`/`.csv` it needs are gone
  (row 7). Reproducing it requires a game run with `MC2_CEMENT_ATLAS_DUMP`.
- **The three-run golden provenance is unfalsifiable** — `scratch/gr3/r1/` is
  gone (row 2). The cross-run reports corroborate but are not the source.
- **Every fingerprint verdict in row-6/trap-1 is the lane's own self-report**,
  re-read but not re-derived; I did not locate raw runner logs under
  `debug_state/`/`run/` to confirm the quoted `DEPLOY_FINGERPRINT` lines.
- **SPIR-V byte-identity in `8df6c3dc` unverified** — needs `build_variants.py`,
  a build action.
- **`shaders/vulkan/ssao_apply.frag:27`** is a third `ssaoTex` decl; I did not
  confirm whether the checker's walk includes `shaders/vulkan/`. Does not change
  row 8 (two GL decls already suffice).
- **The mc2_24 "moved vehicle" attribution is supported, not proven.** 77%/96%
  blob concentration is consistent with a vehicle; I did not visually confirm the
  blob is a vehicle.

## 6. Non-blocking observations (not defects I was asked about)

- `mc2_10_saved_204907/distant_ridge_base.png` (300x100, 167 B) is **pure black,
  all 30,000 px = (0,0,0)** — a golden that can only fail if something *lights*
  those pixels, while counting toward "28 blessed".
- `golden.json` top-level `"blessed_commit"` is `null` (only `identity.` is set).
