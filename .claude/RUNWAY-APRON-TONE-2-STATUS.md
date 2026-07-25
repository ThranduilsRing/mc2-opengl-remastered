# RUNWAY-APRON-TONE-2 — STATUS

**Lane:** RUNWAY-APRON-TONE-2
**Worktree/branch:** `A:/Games/mc2-main` @ `claude/nifty-mendeleev`
**Commits (local only, NEVER pushed):** `608dea4d`, `6b94443e`
**Verification install:** `A:/Games/mc2-opengl/mc2-win64-v0.4c`, deployed with
`--require-clean` from the clean build at `608dea4d`
(`MC2_BUILD_GIT_DIRTY 0`), manifest `src_commit 608dea4db465` — **no `-dirty`
suffix**.
**Date:** 2026-07-25

---

## The complaint

Review pack §9(d), mc2_10 close ground-level (bookmark `saved_204937`, alt 1257):

> "I mean we have runway being a massively different color than the runway on the
> concrete"
> "all of the runways should be the same color. idk why on mc2_10 we have grey
> runways and we are calling that ok. they should ALWAYS be the color that they
> are on top of concrete like in mc2_01."
> "again I don't understand why the runway under the plane is grey when it is
> black on top of the concrete. also we are missing part of the runway. and the
> weird colored edge is wrong as well."

---

## The headline: the working theory was WRONG, and the measurement disproves it

The lane was briefed on — and initially built — an **atlas art-tone** fix: the
cement catalog was believed to pack a dark *asphalt* runway family alongside a
light *concrete* apron family, with `MC2_CEMENT_TONE_MATCH`'s `[0.70,1.40]` gain
clamp preventing the dark family from ever reaching the light one. That reasoning
is internally sound and the clamp analysis is correct. **It is also not what is
happening.**

A new always-on census, `[CEMENT_CELL_TONE v1]`, reports every catalog cell's
concrete-band tone:

| Map | cells | usable | median band lum | **dark cells** |
|---|---|---|---|---|
| mc2_10 | 19 | 19 | 147.2 | **0** |
| mc2_24 | 110 | 110 | 146.3 | **0** |

**There is no dark asphalt family on either map.** Every cement cell is light
concrete around luminance 147. No atlas-side tone operation can reach the dark
runway, because the dark runway never samples the atlas.

### What is actually happening (measured)

`MC2_TERRAIN_LOD_CHUNK_DIAG=32768` (cement class: red = SOLID cement-atlas,
green = TRANSITION, **uncoloured = baked colormap only**) at the user's pose:
the **entire runway under the parked aircraft is uncoloured**. It is never
cement-classified. The apron above it is solid red.

Sampled luminance in the beauty frame at that pose:

| Region | mean RGB | luminance |
|---|---|---|
| Apron concrete (pale) | 175.9, 171.6, 158.2 | **171.3** |
| Apron concrete (upper) | 147.2, 146.9, 130.4 | **145.1** |
| Dark band on apron ("N21") | 84.8, 79.0, 69.6 | 79.6 |
| **Runway under the plane** | 61.1, 65.7, 72.2 | **65.0** |
| Tan edge strip beside runway | 78.9, 72.4, 66.5 | 73.7 |
| Bare terrain (dirt) | 52.5, 54.7, 56.9 | **54.3** |

The runway sits at **terrain brightness** (65 vs bare terrain 54), not at atlas
brightness (147). Because it *is* terrain — baked colormap, drawn through the
full terrain lighting term. The apron beside it is cement-atlas art that is
**lighting-exempt by construction**:

```glsl
normalLight = mix(normalLight, 1.0, pureConcrete);  // cement: fully flat-lit slab
```

**Root cause: a LIGHTING mismatch between two sources, not an art-tone mismatch.**
One surface obeys the scene light; the neighbouring surface ignores it entirely.
At this dusk-lit pose the lit source falls to 65 while the lighting-exempt source
holds 147 — a 2.3x split across a boundary the player reads as one runway.

### One mechanism explains all three of the user's observations

1. **"grey here, black on the concrete"** — the runway crosses the
   cement-classified/colormap boundary. Flat-lit atlas on one side, lit colormap
   on the other. Same road, two renderers.
2. **"missing part of the runway"** — the same classification failure, taken to
   its limit. `PopulateRecipeCementWords` bakes a cement word for a TRANSITION
   tile only when a corner resolves to a SOLID cement layer *and* a mask derives.
   Census: **mc2_10 `dropped_no_layer=278` of 320 transition tiles = 87%**
   (mc2_24: 266 of 778 = 34%). Where the colormap also has no road art baked,
   the pavement simply is not drawn.
3. **"weird colored edge"** — the tan strip (lum 73.7) is the colormap/cement
   boundary itself, sitting between lit colormap road (65) and flat-lit cement
   (147). `MC2_RUNWAY_EDGE_BLEND` was verified **OFF** in every capture and is
   not implicated.

---

## What shipped

### Variant A — `MC2_RUNWAY_TONE_MATCH` (`608dea4d`), default OFF

Wide-gain absolute-target atlas normalisation, plus tunables
`MC2_RUNWAY_TONE_STRENGTH` (0..1) and `MC2_RUNWAY_TONE_TARGET="r,g,b"`. Written
against the original theory. Highlights pass a soft knee (linear to 190,
asymptotic to 255) so lane markings survive a >2x gain instead of clipping flat.

**Disposition: shipped but NOT recommended.** With `dark_cells=0` on every map
tested there is nothing for it to correct; it would only re-tone the apron. It is
retained because the wide-gain path and the `MC2_RUNWAY_TONE_TARGET` cross-map
mechanism are the right tool *if* a third-party or mod map does pack a dark
family — the census will say so. Default OFF ⇒ atlas byte-identical.

### Variant B — `MC2_CEMENT_LIT` (`6b94443e`), default 0.0

The fix that follows from the measurement. A 0..1 uniform lerping cement back
into the terrain's own lighting term:

```glsl
float cementLight = mix(1.0, normalLight, u_cementLit);
normalLight = mix(normalLight, cementLight, pureConcrete);
```

At `0.0` this collapses exactly to the historical flat-lit slab — bit-identical
to stock. At `1.0` apron and colormap-runway share one light, so their
relationship stays consistent under sun, dusk and night instead of diverging with
time of day. Cost: one extra uniform upload per frame.

### Always-on census `[CEMENT_CELL_TONE v1]`

Per-cell band RGB + slot + `nameId`, atlas median, target, dark-cell count. Dark
outliers print unconditionally. No gate — a map can be judged without one.

---

## Verdicts

| Check | Verdict | Evidence |
|---|---|---|
| Census arms and is correct | **PASS** | mc2_10 `cells=19 usable=19 median_lum=147.2 dark_cells=0`; mc2_24 `cells=110 usable=110 median_lum=146.3 dark_cells=0` |
| Original asphalt-family theory | **DISPROVEN** | `dark_cells=0` on both airbase maps |
| Runway is not cement-classified | **PASS** | `DIAG=32768` class view: runway uncoloured, apron solid red |
| Tone split is a lighting split | **PASS** | runway 65.0 vs bare terrain 54.3 vs flat-lit apron 145.1–171.3 |
| Missing pavement is the dropped-transition defect | **PASS** | mc2_10 `dropped_no_layer=278`/320 = 87%; mc2_24 266/778 = 34% |
| `MC2_RUNWAY_EDGE_BLEND` implicated in the edge | **CLEARED** | verified OFF in every capture |
| mc2_24 tone-match still cures the patchwork | **PASS** | `cells=110 adjusted=96 ref_rgb=149.0,146.4,138.4`, three-arm captures distinct |
| Gate OFF = stock | **PASS by construction** | atlas post-pass gated; `u_cementLit=0.0` collapses to the stock expression |
| **`MC2_CEMENT_LIT` ON arm** | **NOT VERIFIED — BLOCKED** | see below |

### Blocked, stated plainly

`MC2_CEMENT_LIT` is **landed but never captured**. Parallel lanes
(`CONE-SPILL-COUPLING-1`, `EDGE-LIGHT-EMISSION-1`) committed into the shared
worktree mid-session; the tree went dirty and a clean-fingerprint build of
`6b94443e` could not be produced, so it was never deployed under
`--require-clean`. The **diagnosis** above is fully evidenced from the clean
`608dea4d` exe. The **cure** is not. Do not recommend it, and do not describe it
as verified, until it is captured at the 204937 pose plus a night pose on a quiet
worktree.

---

## The real fix for the missing pavement — designed, NOT landed

Admit alpha/transition cement slots into the catalog atlas:

1. Drop the `if (tt->isAlpha(slot)) continue;  // pure-cement only` skip in
   `BuildCementCatalogAtlas`.
2. Give each transition slot its own atlas layer (changes atlas size/packing —
   `gridSide` is already power-of-2 with a 1024-cell budget, so headroom exists).
3. Honour the cell's alpha in the cement branch of `terrain_lod_chunk.frag` so
   road art **composites** over the terrain instead of replacing it.
4. Relax the corner-must-be-solid condition in `PopulateRecipeCementWords` once
   transition slots have layers of their own.

This is the only thing that closes the 87%/34% drop rate. `MC2_CEMENT_TRANSITION_FALLBACK`
recovers 33 of 266 on mc2_24 (12%) because dropped tiles form contiguous runs
with no neighbour to borrow from — it is an honest partial and stays default OFF.
**Assessed as too large to land safely in this lane** alongside the diagnosis
work; specified here precisely so the next lane can pick it up cold.

---

## Gates

| Gate | Default | Effect |
|---|---|---|
| `MC2_RUNWAY_TONE_MATCH` | OFF | wide-gain absolute-target atlas tone normalisation (not recommended — nothing to correct on stock maps) |
| `MC2_RUNWAY_TONE_STRENGTH` | 1.0 | taste dial for the above |
| `MC2_RUNWAY_TONE_TARGET` | unset | absolute cross-map target `"r,g,b"` |
| `MC2_CEMENT_LIT` | 0.0 | cement participates in terrain lighting (**the measured fix — unverified**) |

## Files touched

- `GameOS/gameos/gos_terrain_indirect.cpp` — census + wide-gain pass + 3 readers
- `GameOS/gameos/gos_terrain_lod_chunk.{cpp,h}` — `u_cementLit` uniform + reader
- `shaders/terrain_lod_chunk.frag` — cement lighting participation
- `RenderCore/RendererFeatureRegistry.h` — 4 gate registrations
- `scripts/run_smoke.py` — 4 passthrough allowlist entries
- `release_assets/0.5.2/REVIEW/INDEX.html` + `12_mc2_24_cement/` — new §12

## Review pack

New section **12 — mc2_24 airbase concrete** in
`release_assets/0.5.2/REVIEW/INDEX.html`: patchwork before/after, missing-road
swaths with the census numbers and the honest 33-of-266 partial, and the
re-diagnosed runway/apron tone split with the class-diag view. All 12 image refs
verified to resolve; file-wide 82 image refs, 0 missing; 14/14 `<section>` tags
balanced.
