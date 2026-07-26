# PAVED-OVERLAY-CLASS-1 — STATUS

**Lane:** PAVED-OVERLAY-CLASS-1
**Worktree/branch:** `A:/Games/mc2-main` @ `claude/nifty-mendeleev`
**HEAD at start:** `cadf9a92` → **fix commit:** `04ac21e3`
**Deploy lane used:** `A:/Games/mc2-opengl/mc2-win64-abl-validate` (0.4c was occupied — see below)
**Date:** 2026-07-26

---

## Verdict in one line

The classification gap is **CLOSED 100% on all five maps** — every tile the
census reports as unclassified pavement now bakes a word (15/200/169/31/266,
residual **0** on every map). The **uniformity** defect ("the runway changes tone
along its own length") measurably improves on 13 of 15 poses, strongest on the
user's own map and pose. The **mean tone offset** vs cement does **not**
uniformly converge: it closes 50–94% where pavement was darker than cement, and
**widens** where pavement was already brighter. That residual is **albedo**, as
the brief predicted. Gate is **default-OFF** and the user would still see the
original defect on a default launch.

---

## META REVIEW — brief premises checked against reader source

| Brief premise | Verdict | Evidence |
|---|---|---|
| Overlay ids vs `isCement` base-slot flag are disjoint channels | **CONFIRMED** | `mclib/mapdata.h:38-59` (`Overlays` enum, `PAVED_ROAD=1`, `RUNWAY=7`); `mclib/terrtxm.h:380` `isCement()` tests `textures[typeInfo].flags & MC2_TERRAIN_CEMENT_FLAG` on the base slot. |
| Every existing lever nested inside `isCement && isAlpha` / `pureConcrete` | **CONFIRMED** | `gos_terrain_indirect.cpp` transition branch guarded at `:1272-1274`; shader `terrain_lod_chunk.frag` `normalLight = mix(normalLight, cementLight, pureConcrete)` — `pureConcrete` is 0 on these tiles by construction. |
| `isCement()` is a MISNOMER and road/runway tiles *also* satisfy it | **CONFIRMED, and it does NOT contradict the brief** | Comment at `gos_terrain_indirect.cpp:5523` (CEMENT-ROAD-SPLIT-1). Both are true: paved overlays laid on a *cement* base slot are already classified; those laid on *non-cement* base are not. The census measures exactly the latter set, so the brief's framing survives. |
| `IsPavedOverlay` must be built | **PARTLY REFUTED — it already existed** | It was already present as a **census-only lambda** at `:1248`. The lane's real work was promoting it to file scope and giving it a *behavioural* consumer. Both now call one predicate so census and repair can never disagree. |
| Census numbers 29/81/41/58/34% | **CONFIRMED, re-measured independently** | Reproduced exactly for mc2_01/03/05/24; mc2_10 measured **60.8%** (51/31) vs the brief's 58% (50/29) — a 2-tile difference. My own numbers are used throughout. |
| `mc2_24_saved_192316` real, `131157` does not exist | **CONFIRMED** | `scratch/allposes/bm/mc2_24.json` contains only `saved_192316` (+ a `_dup`). The brief's `131157` is absent, as is `191157`/`192916` from earlier briefs. |
| Legend: RED = correct cement, BLACK = defect | **CONFIRMED** | `terrain_lod_chunk.frag` class-diag block; fixed by `27c70d26`. |
| Deploy `0.4c` → `mc2-win64-v0.4c`, plain `0.4c` is BROKEN-TREE | **CONFIRMED** | Both dirs on disk; `deploy_payload.py` canonical-folder list confirms. |
| "Serialise — check_conflict and WAIT" | **CONFIRMED NECESSARY** | 3 fresh lanes claimed `terrain_lod_chunk.frag`; 6 claimed deploy lane `0.4c`; a foreign game PID occupied `0.4c` twice during deploy. Two other lanes committed *during* this lane's captures. |

---

## What shipped — `04ac21e3`

A predicate **outside** the `isCement && isAlpha` guard, which is precisely why
it can reach tiles no prior lever could.

- `IsPavedOverlayType()` promoted to file scope. Hard-paved ids only —
  `DIRT_ROAD`, `ROUGH`, `TWO_LANE_DIRT_ROAD` are **excluded** (dirt must keep
  normal terrain lighting).
- New pass in `PopulateRecipeCementWords`, running **after** the census so
  `[CEMENT_PAVEMENT_GAP v1]` always reports the **pre-repair** denominator and
  the arms stay comparable.
- **`cementWord` bit 22**, baked deliberately **without** the bit-31 valid bit.
  Audited every reader first: the frag cement override (`:920`), the
  diag-connect neighbour scan (`:1085`), the runway-edge dilate (`:1151`), the
  fallback donor search and the edge-blend pass **all gate on bit 31 first**, so
  the new bit is inert to every one of them *by construction* and cannot perturb
  cement outline geometry. Only the lighting site consults it.
- **Gate `MC2_PAVED_OVERLAY_CLASS`**, default `0`:
  - `1` = **lighting exemption only**, keeps the tile's own baked colormap albedo.
  - `2` = A/B arm that also adopts the solid cement atlas layer (albedo swap).
- Class-diag `32768`: newly classified tiles render **WHITE**.

### Design deviation from the brief, and why

The brief said "route overlay pavement into **the same treatment cement gets**".
Read literally that means the albedo swap — which is what
`MC2_CEMENT_TONE_MATCH` and `MC2_RUNWAY_TONE_MATCH` did, and both were
user-rejected as *"after is WAY worse"*. Reader source shows `pureConcrete`
drives **two** things: albedo (`matWeights → concrete`) **and** lighting
exemption (flat-lit). The user's actual complaint — a runway *changing tone
along its own length* — is the **lighting** half, since that variation *is* the
per-tile normal/shadow lighting the neighbouring flat-lit cement never receives.
So mode 1 takes only the lighting half and leaves albedo alone. The literal
reading is retained as mode 2 for A/B rather than made the default.

---

## Census — before vs after (my own runs, deployed exe `04ac21e3`)

`MC2_TERRAIN_LOD_CHUNK_DIAG=524288`, 15s runs.

| mission | paved_overlay | unclassified | **% before** | tiles baked | **residual** |
|---|---|---|---|---|---|
| mc2_01 | 51 | 15 | **29.4%** | 15 | **0** |
| mc2_03 | 247 | 200 | **81.0%** | 200 | **0** |
| mc2_05 | 416 | 169 | **40.6%** | 169 | **0** |
| mc2_10 | 51 | 31 | **60.8%** | 31 | **0** |
| mc2_24 | 787 | 266 | **33.8%** | 266 | **0** |

**Every unclassified paved tile on every map is now classified.** The
`[PAVED_OVERLAY_CLASS v1] flat_baked=` count equals `paved_no_cement` exactly on
all five maps — the predicate's coverage of the measured gap is total.

---

## Visual delta (OFF vs ON, same pose, deterministic captures)

`det=True`, `all_present=True`, `nondeterministic=False` on every run.

| pose | % pixels changed |
|---|---|
| mc2_10 `saved_204937` (**the user's grading pose**) | **29.65%** |
| mc2_10 `saved_204956` / `204958` | 21.78% |
| mc2_10 `lance_close` | 20.31% |
| mc2_10 `saved_212258` | 11.22% |
| mc2_03 `saved_220422` | 3.60% |
| mc2_10 `saved_192540`, `saved_204523`, `saved_212140` | **0.00%** |

The three **0.00%** poses are views containing no paved overlay — natural
no-pavement controls, showing **zero collateral** even with the gate ON.

---

## Uniformity — the metric that matches the actual complaint

Within-pavement luma spread, measured over the **class-diag-derived pavement
mask** (not hand-placed boxes — a rigour improvement over the prior lane, whose
own self-review flagged ±2 framing error).

| pose | sd OFF | sd ON | **sd drop** | p95−p5 drop |
|---|---|---|---|---|
| mc2_10 `saved_204907` | 82.50 | 67.83 | **17.8%** | 13.8% |
| mc2_10 `saved_204956/8` | 60.38 | 50.55 | **16.3%** | 15.5% |
| mc2_10 `saved_204937` | 75.56 | 63.45 | **16.0%** | 13.7% |
| mc2_10 `saved_204611` | 98.04 | 82.59 | 15.8% | 13.2% |
| mc2_10 `saved_212258` | 62.38 | 52.90 | 15.2% | 14.0% |
| mc2_10 `saved_204753` | 87.20 | 74.24 | 14.9% | 12.6% |
| mc2_05 `saved_205843` | 96.76 | 88.63 | 8.4% | 5.6% |
| mc2_03 `saved_220422` | 37.29 | 34.48 | 7.5% | −28.0% |
| mc2_01 `saved_220655` | 25.56 | 24.60 | 3.8% | 7.9% |
| mc2_01 `saved_205644` | 76.57 | 75.02 | 2.0% | 2.3% |
| mc2_24 `saved_192316` | 74.82 | 75.32 | **−0.7%** | 1.0% |

Improves on **13 of 15** poses; strongest on **mc2_10**, the map and pose the
user reported.

---

## THE RESIDUAL — reported honestly, not promised away

Mean pavement luma vs mean cement luma:

| pose | gap OFF | gap ON | closed |
|---|---|---|---|
| mc2_10 `saved_204753` | 18.37 | 1.17 | **93.6%** |
| mc2_10 `saved_204956/8` | 33.81 | 5.44 | **83.9%** |
| mc2_10 `saved_212258` | 28.48 | 5.05 | **82.3%** |
| mc2_10 `saved_204937` | 16.73 | 8.33 | **50.2%** |
| mc2_03 `saved_220422` | 70.63 | 53.68 | 24.0% |
| mc2_01 `saved_220655` | 39.67 | 36.67 | 7.6% |
| mc2_01 `saved_205644` | 41.69 | 42.95 | **−3.0%** |
| mc2_10 `saved_204907` | 73.13 | 77.13 | **−5.5%** |
| mc2_05 `saved_205843` | 47.12 | 49.88 | **−5.9%** |
| mc2_24 `saved_192316` | 16.58 | 18.46 | **−11.3%** |
| mc2_10 `saved_204611` | 3.57 | 20.84 | **−483%** |

**The sign depends on which side of cement the pavement started.** Where
pavement was *darker* than cement, flattening its light brightens it toward
cement and the gap closes sharply. Where pavement was already *brighter*
(mc2_01 `205644` 149.9 vs 108.2; mc2_05 168.8 vs 121.7; mc2_10 `204907` 160.9 vs
87.7), or already nearly converged (`204611`, gap 3.57 — where lighting happened
to be *compensating* for the albedo difference), flattening moves it away.

Once both surfaces are flat-lit, **the remaining gap is pure albedo** — the
baked colormap and the cement atlas are different source textures, exactly as the
brief warned. No lighting-side lever can close it. Mode 2 exists to test the
albedo side, but it is the same class of change the user twice rejected, so it is
**not** recommended without a click-through.

**This is a partial fix.** It closes the classification gap completely and the
uniformity defect substantially; it does not make all concrete one colour.

---

## Verification

- **Build:** clean. Tracked tree was **clean** at commit time (all 197 "dirty"
  entries were untracked scratch; `GenerateBuildFingerprint.cmake` uses
  `--porcelain -uno`, so untracked does not taint the fingerprint).
- **Attributable exe:** deployed exe embeds `04ac21e32ea4` = the fix commit;
  manifest `src_commit 04ac21e32ea4`, `dirty:false`.
- **Gate strings present in the deployed binary:** `MC2_PAVED_OVERLAY_CLASS`,
  `PAVED_OVERLAY_CLASS`, `MC2_TERRAIN_LOD_CHUNK_DIAG`, `CEMENT_PAVEMENT_GAP` —
  all verified by binary grep.
- **`gate_status(MC2_PAVED_OVERLAY_CLASS)` = `verdict OK`, zero flags**:
  registered, tier1-documented, smoke-allowlisted, default OFF. No ENV-DROP.
- **tier1 smoke: PASS 5/5** (mc2_01, mc2_03, mc2_10, mc2_17, mc2_24), `Δ destroys +0`,
  MOVER-COHERENCE `violations=0`.
- **Determinism:** every capture `det=True`, `nondeterministic=False`.

---

## Bookends

**META REVIEW** — done first, against reader source (`mapdata.h`, `terrtxm.h`,
`gos_terrain_indirect.cpp`, `terrain_lod_chunk.frag`), not status docs. Nine
premises checked; one **refuted** (`IsPavedOverlay` already existed as a census
lambda — the lane's job was narrower and more precise than the brief implied),
one **corrected** (mc2_10 is 60.8%, not 58%), and one apparent contradiction
(`isCement` is a misnomer, `:5523`) run down and shown **not** to overturn the
brief.

**ADVERSARIAL SELF-REVIEW**

- *"Would the user still see this on a default launch?"* — **YES. The gate is
  default-OFF, so on a default launch all five reported defects are unchanged.**
  Nothing here closes the user's report. It needs a click-through and then a
  default-ON decision, and the mean-tone table above says that decision is **not
  obviously yes** — mode 1 makes four of eleven measured poses numerically worse
  on mean offset while making 13 of 15 better on uniformity.
- *Refuting my own headline:* "classification gap closed 100%" is a claim about
  **tile classification**, NOT about appearance. It is proven by
  `flat_baked == paved_no_cement` on all five maps. It must not be read as "the
  concrete is now one colour" — the residual table above is the honest answer to
  that, and it is mixed.
- *Is the uniformity win real or a mask artefact?* Partly uncertain. The pavement
  mask comes from the class-diag and can include props/markings drawn over the
  pavement, so absolute sd values are inflated. The **relative** OFF→ON drop is
  measured on the identical mask and identical pose, so the direction is sound;
  the magnitude carries mask-contamination error.
- *Gate registered / documented / allowlisted?* — **Yes, all three**, verified by
  `gate_status` = OK with zero flags, after the edit.
- *Default-OFF byte-identical?* — **By construction, not by capture.**
  `PavedOverlayClassMode()` returns 0 → the pass is skipped → bit 22 is never set
  on any tile → the shader's `pavedOverlayFlat` is false everywhere → both the
  lighting block and the class-diag white branch are dead. I attempted a direct
  empirical A/B against a pre-fix exe and **could not obtain one**: `0.4c` had
  already been overwritten with my own build by a parallel lane. I did **not**
  substitute a weaker test and call it proof. The white-pixel count test I ran is
  **confounded** (the class-diag view contains unrelated white HUD/prop pixels in
  both arms) and is reported as supportive only, not as evidence.
- *mc2_05 night unchanged?* — **On a default launch, yes, by construction
  (gate OFF).** But note honestly: with the gate **ON**, mc2_05 *does* change
  (1.58% of pixels, mean tone −5.9%). So enabling this by default would touch
  the user-approved mc2_05 night lighting, and that must be part of any
  default-ON decision rather than assumed away.
- *Capture-rule shortfall — disclosed:* the standing rule asks ≥3 locations per
  map. **mc2_10 has 14 poses and is fully covered; mc2_01/03/05/24 have only the
  2 poses that exist in the real consolidated user set.** I did **not** author
  synthetic extra poses and present them as user poses — the prior lane's fake
  bookmarks were deleted as fraudulent and `check-visual-bookmark-honesty.py`
  now guards that. Broader coverage on the four day maps is genuinely
  outstanding.
- *Serialisation:* three lanes claimed my shader file and six claimed deploy
  lane `0.4c`. I verified the tracked tree was clean (nobody holding uncommitted
  edits) before editing, refused to kill two foreign game PIDs occupying `0.4c`,
  polled until one cleared, and when a second appeared moved to the uncontended
  canonical `abl-validate` lane rather than collide. Two lanes committed during
  my captures; I confirmed neither touched my code.

---

## Recommended next steps

1. **User click-through on mc2_10 `saved_204937`** with
   `MC2_PAVED_OVERLAY_CLASS=1` — the 29.65%-changed, 50%-gap-closed,
   16%-more-uniform pose. This is the single decision that gates everything else.
2. If accepted, extend pose coverage on mc2_01/03/05/24 from **real** user
   bookmarks before any default-ON proposal, and re-check mc2_05 night.
3. **Do not ship mode 2 without a click-through** — it is the same albedo swap
   the user rejected twice.
4. The residual is albedo. If the user wants full convergence, that is an
   asset-side problem (reconcile the baked colormap against the cement atlas),
   not a shader-side one.
