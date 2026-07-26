# REVIEW-LANES-LOG-2 — bring the 0.5.2 REVIEW pack's lanes/efforts log current

Lane: REVIEW-LANES-LOG-2 · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
Base HEAD `b8211bce`. Documentation only — no engine source touched, no build, no deploy,
no captures. Tracked tree was clean at start and at end.

## What this lane was sent to do

§14 of `release_assets/0.5.2/REVIEW/INDEX.html` documented 52 lanes. Roughly 20 more had
run since, and several **overturned earlier entries**, so the page was not merely
incomplete — in places it was wrong. Extend the tracked manifest (never hand-edit the
HTML), correct the superseded entries, add an "instruments that lied" section, and list
the open user gates at the top.

## Premise corrections found in META REVIEW (before doing the work)

| Brief said | Source said |
|---|---|
| Add VERIFY-INTEGRITY-3, QUIET-DEFAULT-BUILD-1, CEMENT-ATLAS-CRASH-1 | **Already present** as `fixed` rows. Verified their content, left them alone. |
| Add ROAD-OVERLAY-LIGHT-1, SHADOW-SUNCLAMP-1, PROP-LIGHT-CLASS-4, LIGHT-CLUSTER-PERF-1 | **Already present as `in-flight` stubs** saying "no result yet". These needed *updating to their landed result*, not adding. |
| "620 gates strippable … not the 101 previously believed" | **Confirmed**, and the attribution matters: DETERMINISM-RATCHET-2 overturned VERIFY-INTEGRITY-3's 101 — "off by roughly an order of magnitude". |
| "a camera-pinning gate stripped by the smoke runner" | **True only of `run_smoke.py`.** CAPTURE-DETERMINISM-1 explicitly *refuted* the same claim for `run_visual_capture.py`, which copies the environment and strips nothing. Published with that scope. |
| "`PASS (5/5)` beside `DEPLOY_FINGERPRINT MISMATCH` for eight lanes" | Confirmed — IBL-NIGHT-SCALE-1 records "the sixth lane", PROP-BASE-LIGHT-DIR-1 "the eighth". |
| BUILDING-FACE-POSE-1 "if it lands while you work" | **Did not land** — no status doc, not in the lane registry. Recorded as `in-flight` with an honest "no result at render time" note, since PROP-BASE-LIGHT-DIR-1 names it as a prerequisite. |

## Delivered

**19 lanes added, 6 updated from `in-flight` to their landed result.** §14 now carries 71
rows. Added: TERRAIN-SHADOW-GUARD-1, PERF-HARNESS-TRUTH-1, PAVED-OVERLAY-CLASS-1,
CONE-SPILL-GRADE-1, NIGHT-TAGS-GREEN-1, CAPTURE-DETERMINISM-1, LIGHT-REALISM-FLIP-1,
POOL-SPILL-STATIC-1, BEACON-FIXTURE-ID-1, FIXTURE-GLOW-1, NIGHT-AMBIENT-BALANCE-1,
IBL-NIGHT-SCALE-1, RUNWAY-LIGHT-MISSING-1, SHADOW-DECISIONS-1, SSAO-RETUNE-1,
PROP-BASE-LIGHT-DIR-1, GOLDEN-REBLESS-2, DETERMINISM-RATCHET-2, BUILDING-FACE-POSE-1.

**Two new sections.** `#open_gates` at the very top (five decisions only the user can make,
each with its evidence already gathered) and `#instruments` after §14 ("Instruments that
lied" — eleven measurement failures, the session's real throughline). Both wired into the TOC.

**Six correction notes** on earlier sections, each tagged `REVIEW-LANES-LOG-2 CORRECTION`
and superseding rather than erasing the original text:

* §12 — `MC2_CEMENT_LIT` is **not** the fix: runway luma is bit-identical (66.51) at every
  strength, and in daylight the gate *widens* the gap.
* §11 — the "vehicles are dark" thread is aimed at the wrong subject **and** the wrong
  cause: they are **static props**, and the gap is **albedo** (truck texture ~3.3× darker).
  Five lanes chased it.
* §9 — nothing was ever missing: **mc2_10 has no runway lights authored**; 365 exist
  campaign-wide (mc2_23 has 112, mc2_01 has 56). Grade on mc2_23, never mc2_10.
* §8 — prop self-shadow **cannot** fix the building-face complaint (a face turned away has
  `ndl = 0`); `base_light` is effectively zero and `ambientTerm` is the real term;
  `MC2_SSAO` is **latent-broken, not merely off** (radius 3.0 wu vs a 42.67 wu cell, and
  **no blur pass at all**).
* §7 — `c1f98b45` cone/pool coupling is **regressive** (deletes 7 of 9 night pools), now
  default-OFF behind `MC2_CONE_SPILL_COUPLING`.
* §14 — the audit's "no code exists" claims for foliage alpha-test shadows, the LOD flash
  cure, and pool consumers were all **false**; each had already shipped.

User-REJECTED (`MC2_CEMENT_TONE_MATCH`, `MC2_RUNWAY_TONE_MATCH`, runway-blend rev-1,
cone-spill coupling) and user-APPROVED (shoreline meander 75, runway rev-2 dilation,
light-realism variant B, pool cap 256, mc2_05 night lighting) work is labelled as such so a
later lane cannot revive or regress either unknowingly.

## Generator defect found and fixed

`MC2_LIGHT_POOL_MAX` and `MC2_LIGHT_GRID_DIM` are **numeric knobs** (source defaults **256**
and **64**) whose registry `defaultOn` boolean is meaningless and reads false — so the page
published them as a bold **OFF**. That is precisely the drift class this pack exists to
prevent. `render_default_cell` now detects a `Default <n>` declaration in the registry doc
string and publishes that value instead. The guard is deliberately tight — values of 0 and 1
are never reinterpreted, since a boolean gate cannot declare a default of 256 — so the
blast radius is exactly the 2 affected rows, measured before the change.

Also corrected a now-stale count in REVIEW-EXHAUSTIVE-1's own row: the registry failed to
cover 18 gates then and covers all but **16** now, because LIGHT-CLUSTER-PERF-1 registered
`MC2_LIGHT_POOL_MAX` and DETERMINISM-RATCHET-2 registered `MC2_LETTERBOX_OPAQUE`.

## Verification

| Check | Result |
|---|---|
| `--check` image manifest ↔ disk | **130 / 130**, 0 orphans, 0 missing |
| Broken local links (href/src) | **0** |
| HTML tag balance | **all balanced** |
| Dangling TOC anchors | **0** |
| Two consecutive renders | **byte-identical** |
| Gate defaults read from source at render time | **proven** — flipping `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` in a *copy* of `RendererFeatureRegistry.h` and rendering with `--registry` flipped the published cell ON → OFF. Non-destructive; the real header was never modified. |
| All 31 cited commit SHAs resolve | **verified** with `git cat-file -e` |
| Every gate cited resolves from source | 21 gates: 19 from the registry, 2 numeric knobs; 0 rendered as NOT IN REGISTRY |

## Deliberately NOT done

**No new captures**, per the brief. The existing evidence set is one executable old and the
disk is at ~13 GB. That staleness is now stated *on the page itself* in the open-gates
section rather than left implicit — the prose and gate defaults are current, the pictures
are not.

No push, no `--amend`, no build, no deploy, no smoke. Nothing under `release_assets/` or
`releases/` was deleted.

## Open user gates (also rendered at the top of the page)

1. **Grade the pool-spill sweep** — `MC2_LIGHT_REALISM_POOL_SPILL` at **1.3 vs 2.2**. 1.3
   keeps the cone and beacon wins and cuts blowout 8.29% → 1.27%.
2. **Grade the fixture glow** — `MC2_STATIC_PROP_FIXTURE_GLOW=3`, **mc2_23**, ground level.
3. **Retire `v0.5.2-baseline-PROPOSED`?** It fails 32/39 regions and has **no vendored
   poses**, so it is unauditable.
4. **Free 8.2 GB** — a re-cut peaks at ~18.2 GB against ~13 GB free.
5. **Tag, push, upload.**
