# ROAD-OVERLAY-LIGHT-1 — STATUS

**Lane:** ROAD-OVERLAY-LIGHT-1
**Worktree/branch:** `A:/Games/mc2-main` @ `claude/nifty-mendeleev`
**HEAD at start:** `4b343e72` → **HEAD at end:** `27c70d26`
**Commits landed:** 1 (`27c70d26`, comment-only)
**Date:** 2026-07-26

---

## Verdict in one line

`MC2_CEMENT_LIT` is a **CLEAN NEGATIVE** — captured for the first time across
four lanes, at three strengths, day and night: it closes only ~24% of the tone
gap at night and **widens** it in daylight, because it is multiplied by
`pureConcrete` and therefore **cannot touch overlay-drawn pavement at all**. The
real defect is now measured and visually proven: **the runway is 100%
unclassified, and even the "apron" is 56-69% unclassified.** The fix must be
classification, not lighting.

---

## META REVIEW — brief premises checked against reader source

| Brief premise | Verdict | Evidence |
|---|---|---|
| Root cause = overlay ids vs `isCement` base-slot flag, disjoint channels | **CONFIRMED** | `gos_terrain_indirect.cpp:1218-1246`, `PopulateRecipeCementWords`; visually proven by class diag below. |
| Legend inverted; RED = solid cement, BLACK = defect | **CONFIRMED, now fixed** | Shader truth `terrain_lod_chunk.frag:1202-1215`. Fixed at **two** sites (`:1228` and `:1374`), not the one the brief named. |
| Both repair levers nested inside `isCement && isAlpha` | **CONFIRMED** | `:1309` inside `:1264`; `:1401` early-out. |
| "Use `MC2_CEMENT_LIT`, likely better and already half-built" | **REFUTED BY CAPTURE** | Fully wired (env→`u_cementLit`→frag), but measured negative in both directions. See below. |
| "87% dropped may badly UNDERCOUNT" | **PARTLY REFUTED** | On mc2_10 the overlay class is only **29** tiles vs the transition-drop's 278/320 — there the transition class *dominates*. The undercount claim holds on mc2_03 (81%), not universally. |
| user bookmarks `mc2_24_saved_191157` / `192916` | **DO NOT EXIST** | Consolidated user set `scratch/allposes/bm/mc2_24.json` contains only `saved_192316`. Used that + two clearly-labelled authored poses. |
| "tests/visual/bookmarks/*.json deleted as fraudulent" | **CONFIRMED, and the guard shipped** | `scripts/check-visual-bookmark-honesty.py` now reports 14 clean / 1 acknowledged-unverified. |
| Deploy trap: `0.4c` → `mc2-win64-v0.4c` | **CONFIRMED — both dirs exist on disk** | Used the explicit resolved path throughout. |

---

## THE DECISIVE NUMBERS — `[CEMENT_PAVEMENT_GAP v1]`, first run ever

`MC2_TERRAIN_LOD_CHUNK_DIAG=524288`, deploy `0.4c`, 15s loads.

| mission | paved_overlay | paved_no_cement | **% unclassified** | any_overlay | cement_total |
|---|---|---|---|---|---|
| mc2_01 | 51 | 15 | **29.4%** | 62 | 106 |
| mc2_03 | 247 | 200 | **81.0%** | 274 | 316 |
| mc2_05 | 416 | 169 | **40.6%** | 519 | 558 |
| mc2_10 | 50 | 29 | **58.0%** | 299 | 320 |
| mc2_24 | 787 | 266 | **33.8%** | 787 | 778 |

Every map has an unclassified-pavement population. mc2_03 is the worst (81%).
mc2_10's is numerically small (29) yet visually dominant — see the class diag:
the tiles that carry the *runway itself* are in that 29.

---

## `MC2_CEMENT_LIT` — the ON arm, captured at last

Poses lifted verbatim from the real user set (`scratch/allposes/bm/mc2_10.json`),
including **`saved_204937`, the exact pose in the user's report**. All arms
deterministic (`det=True`), engine capture fired.

### mc2_10 @ `saved_204937` (NIGHT) — mean luma

| arm | apron slabs | runway | **gap** |
|---|---|---|---|
| OFF (0.0) | 90.43 | 66.51 | **+23.92** |
| 0.35 | 88.40 | 66.51 | +21.89 |
| 0.6 | 86.95 | 66.51 | +20.44 |
| 1.0 | 84.63 | 66.51 | **+18.12** |

**The runway value is bit-identical across all four arms (61.56 / 71.46).** That
is direct empirical proof the gate cannot reach overlay pavement — exactly as
the `pureConcrete` multiply predicts. At maximum strength it closes 24% of the
gap and stalls; the residual +18.12 is **albedo**, not lighting.

### mc2_24 @ `apron_wide` (DAY) — the direction reverses

| region | OFF | ON 1.0 | delta |
|---|---|---|---|
| concrete | 131.40 | 145.89 | +14.49 |
| road | 115.02 | 128.18 | +13.16 |
| **gap** | **+16.38** | **+17.71** | **WIDER** |
| no-pavement control | 35.14 | 35.88 | **+0.74 (collateral clean)** |

`normalLight = mix(0.35, 1.20, diffuse)` exceeds 1.0 on well-lit day surfaces, so
cement *brightens*. **Night narrows the gap, day widens it.** A lever whose sign
depends on time of day is not a uniformity fix.

**Recommendation: do NOT ship `MC2_CEMENT_LIT` as the lever. Keep default 0.0.**
Not user-rejected like the TONE_MATCH gates — it is simply the wrong mechanism.
This closes the item that blocked three prior lanes.

---

## Root cause, now VISUALLY proven (`MC2_TERRAIN_LOD_CHUNK_DIAG=32768`)

`scratch/roadlight1/out/CLASSDIAG/mc2_10_saved_204937.png`, read with the
**corrected** legend:

| region | classification |
|---|---|
| runway (both strips) | **100% BLACK — colormap-only, zero classification** |
| apron slab L | 56% BLACK, 26% RED, 17% GREEN |
| apron slab R | 69% BLACK, 30% RED |
| upper base area | solid RED/GREEN (correctly classified) |

The base apron is a solid red field; the runway is completely uncoloured. **The
boundary between them is the user's "weird coloured edge", and the uncoloured
strips are the "missing part of the runway".** This also explains the weak ~7%
night response: only ~28% of "apron" pixels are cement, so `pureConcrete` gates
the lever out of the majority of the very surface it was meant to fix.

---

## What shipped

`27c70d26` — comment-only. Corrects the inverted class-diag legend at **two**
sites and records the full class→colour table inline at the census head.
**Zero behaviour change.**

## What did NOT ship, and why

No fix for the overlay-pavement class. Two honest blockers:

1. **The tree is contaminated by a live sibling lane.** `gos_postprocess.cpp`
   was clean at my preflight (02:52Z) and was written at 02:54Z by
   **SHADOW-SUNCLAMP-1** — a `MC2_SHADOW_GUARD_CLAMPED_SUN` change that is
   **DEFAULT ON and specifically alters mc2_10 shadows**, i.e. my primary map.
   Building would have silently folded their unreviewed work into my evidence,
   and `deploy --require-clean` is impossible. Not mine to commit or stash.
2. The correct fix is a **new predicate keyed on `getOverlay`/`IsPavedOverlay`,
   outside the `isCement && isAlpha` guard** — a real classification change
   needing a new gate, registration, docs, allowlist and a clean build. Landing
   that against a contaminated tree would be unverifiable.

**How the evidence was obtained anyway (the unlock the prior three lanes missed):**
the deployed `0.4c` exe is clean-built from `06ffbcf4` (manifest `dirty:false`),
and `git diff 06ffbcf4..27c70d26` over all `*.cpp/h/frag/vert/comp/hglsl` yields
**only comment lines** plus one docs file — proven zero behaviour delta. So the
deployed binary is functionally identical to my HEAD, and captures against it are
honest without a build. All four gate strings verified present in that exe.

---

## Recommended next lane

1. **Serialise with SHADOW-SUNCLAMP-1** — let it land or drop before any
   terrain build. Its default-ON mc2_10 shadow change will confound any
   pavement capture taken on that map.
2. **Build the overlay-pavement classifier.** New `IsPavedOverlay(getOverlay(...))`
   predicate feeding a cement word, placed *outside* the `isCement && isAlpha`
   guard (every existing lever is provably inside it). New gate, default OFF.
   Size it with the census table above; start on **mc2_03 (81%)**, verify on
   **mc2_10 @ saved_204937**.
3. **Do not revisit `MC2_CEMENT_LIT`.** Closed with capture evidence, both
   directions, day and night.
4. Expect the residual gap to be **albedo**: the runway's baked colormap and the
   cement atlas are different source textures. Classification alone may not
   fully converge tone — measure before promising.

---

## Bookends

**META REVIEW** — done first, against source/git/gate-registry. Eight premises
checked; the central one (`MC2_CEMENT_LIT` is "likely better") was **refuted by
capture**, and three others corrected (two legend sites not one; non-existent
mc2_24 bookmarks; undercount claim not universal).

**ADVERSARIAL SELF-REVIEW**

- *"Would the user still see this on a default launch?"* — **YES. All five
  defects, unchanged.** This lane shipped one comment-only commit. It produces
  **evidence and a rejected lever**, not a fix. Nothing here should be read as
  closing the user's report.
- *Gate registered / documented / allowlisted?* — **No new gate was added**, so
  no such claim is made. `MC2_CEMENT_LIT` pre-existed and was independently
  re-verified `verdict=OK` (registered, tier1-documented, smoke-allowlisted);
  `MC2_TERRAIN_LOD_CHUNK_DIAG` `verdict=OK`, smoke-allowlisted, so no ENV-DROP.
- *CLEAN-fingerprint build?* — **I did not build, and I do not claim a clean
  build.** `DEPLOY_FINGERPRINT` reported **HARD FAIL** on every run (exe
  `06ffbcf4` vs HEAD `27c70d26`). That is disclosed, not hidden; the zero-delta
  proof above is what licenses the captures, and it is reproducible by anyone.
- *Gate string in the deployed binary?* — verified present for all four
  (`MC2_CEMENT_LIT`, `MC2_CEMENT_ALPHA_ATLAS`, `CEMENT_PAVEMENT_GAP`,
  `MC2_TERRAIN_LOD_CHUNK_DIAG`). The prior lane's "zero occurrences" finding was
  against a since-replaced exe.
- *Deployed to the LEASED path?* — yes, explicit
  `A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe`. Both `mc2-win64-0.4c` and
  `mc2-win64-v0.4c` exist on disk; the trap was live and was avoided.
- *mc2_05 night unchanged?* — **by construction**, not by assertion: the only
  commit is comment-only and `MC2_CEMENT_LIT` default remains `0.0`. A capture
  would add no information beyond the zero-delta proof.
- *Self-refutation attempted and accepted:* mc2_10 first reported
  `crash_silent` under the diag bit. Re-running twice with the bit gave PASS/PASS
  and once without gave PASS ⇒ **flaky, NOT diag-caused.** The tempting "the diag
  crashes mc2_10" finding was dropped.
- *Known weakness:* the luma region boxes are hand-placed, so the absolute
  numbers carry maybe ±2 of framing error. The **class-diag percentages and the
  bit-identical runway values** are robust and carry the argument on their own;
  the conclusion does not rest on the box placement.
