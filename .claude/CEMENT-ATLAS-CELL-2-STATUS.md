# CEMENT-ATLAS-CELL-2 — STATUS

**Lane:** CEMENT-ATLAS-CELL-2 · branch `claude/nifty-mendeleev` · 2026-07-28
**Commits:** `d6ae45f9` (both arms + classifier) · `4aadd393` (build fix) ·
`7c80c004` (atlas dump + detector recalibration) · `ad24a93b` (review pack §19)
**Deliverable:** `release_assets/0.5.2/REVIEW/INDEX.html` → **§19 "Concrete tone —
ARM A vs ARM B (your choice)"** (`#19_cement_concrete`), 85 blocks.
**Verdict:** both arms built, both **default OFF**, both captured, presented side
by side. **No winner picked — that is the user's call from the frames.**

---

## 1. The blocker from CEMENT-ATLAS-CELL-1 is REFUTED

The previous lane stopped because it believed runway markings were themselves
cement `terrainType`s in their own catalog cells, so normalising cells would
erase them. The user's ruling said otherwise. **Both source and measurement
confirm the user.**

**From source** (`gos_terrain_indirect.cpp`):

- `g_cementLayerIndexBySlot` — the solid-cement map ARM B repoints — is filled
  only from slots where `isCement && !isAlpha` (enumeration skips alpha at
  L1868-69), and alpha cells are explicitly skipped again when the map is built
  (`if (cementSlotIsAlpha[k]) continue`, CEMENT-UNIFORM-2).
- `TileIsRoadArt()` states it directly: a slot present in that map is
  `// solid cement, not road`. Road/runway art is either an `&Overlays` id or a
  cement-**alpha** transition cell.
- The overlay layer is a genuinely separate path: `u_overlaySidecar` in
  `terrain_lod_chunk.frag` and the standalone `terrain_overlay.frag` pass, each
  with its own sampler and blend — exactly the *"their own layer … handled by
  the shader"* the user described.
- Alpha cells are **appended after every solid cell** (`nSolidCells` is the split
  point), so they are outside the `[0,NT)` range both arms iterate.

**From measurement** — a new `MC2_CEMENT_ATLAS_DUMP` diagnostic writes the
assembled atlas so the art can be inspected rather than assumed. On mc2_24:

> **Not one of the 110 solid cells has a single texel more than 45 luma ABOVE
> its band mean.** Largest excursion measured: ~11 luma.

There is no painted marking art inside solid concrete cells to erase on this
map. Census reports `family_marked=0`. A map that *does* pack paint into a solid
cell is detected (>2% of texels >60 luma above band) and excluded from ARM B by
index, and the census prints those cells per-`nameId` so they can be audited.

**No cells needed excluding by name on any of the six maps captured.**

---

## 2. A measured finding that reframes the problem

Dumping the atlas contradicted the premise both arms were built on.

| mc2_24 solid cells (n=110) | value |
|---|---|
| band-mean luminance, median | **146.3** |
| p05 – p95 | **133.1 – 147.6** |
| full min – max | 107.1 – 147.6 (40.6 spread) |
| cells with a large **in-cell dark region** | **78 / 110** |
| that dark region | 36–52% of the tile at **L≈60** vs concrete **L≈147** |

Cross-cell *base tone* is already nearly uniform — 90% of cells sit inside a
14-luma band. The 40.6 full range is driven by a handful of cells whose band
mean is dragged down by their own dark region.

**So the dominant reason concrete "magically becomes a different color" on
mc2_24 is the large dark art authored INSIDE many cells, not cross-cell tone
variation.** That is why ARM B (replaces the whole cell) moves 11–15 luma while
ARM A (rescales base tone only) moves 1–2.

---

## 3. The two arms

| | ARM A | ARM B |
|---|---|---|
| Gate | `MC2_CEMENT_CONCRETE_TONE=1` | `MC2_CEMENT_CONCRETE_CANON=1` |
| Default | **OFF** (byte-identical) | **OFF** (byte-identical) |
| Acts on | atlas texels of concrete-family cells | `g_cementLayerIndexBySlot` |
| Effect | per-cell gain onto family median, clamp [0.80,1.25], soft knee | plain-concrete slots repointed at one canonical unmarked cell |
| Keeps per-cell art | **yes** | **no** |

Both share one new **concrete-family classifier** (`ClassifyConcreteFamily`): a
cell joins only if near-neutral (chroma ≤ 0.20) **and** its band luminance is
within [0.72,1.32] of the family centre (seeded from the upper/apron cluster,
then re-medianed over the admitted set).

ARM B repoints the **slot→layer map** rather than the shader, so every
downstream reader agrees by construction — word bake, `cemLayerForVertex`, and
`TileIsRoadArt` (which only tests `!= 0xFFFF`, so road classification is
provably unaffected).

### ⚠ Honest caveat on ARM A — stated in the pack, not buried

On mc2_24 ARM A's census is `family=110 adjusted=96 ref_rgb=149.0,146.4,138.4`
— **numerically identical** to the census of the already-rejected
`MC2_CEMENT_TONE_MATCH` (*"after is WAY worse"*). Every solid cell on that map
passes the concrete test, so concrete-only scoping buys nothing there and ARM A
**is** the rejected lever with a narrower clamp. It differs only where a map
packs genuinely non-concrete cement cells. My brief required showing a
difference or saying so plainly: **on the user's reference map there is no
meaningful difference, and I expect ARM A to be rejected again.**

---

## 4. Measurements

### Atlas cross-cell spread (the "43 → 135" number, at source)

| map | OFF | ARM A | ARM B |
|---|---|---|---|
| mc2_01 | 61.1 (std 10.38) | 61.1 (std 7.50) | atlas unchanged — **one cell in use** |
| mc2_03 | 4.0 (std 1.31) | 0.7 (std 0.25) | ″ |
| mc2_05 | 20.6 (std 2.60) | 0.7 (std 0.25) | ″ |
| mc2_10 | 7.2 (std 1.90) | 1.3 (std 0.43) | ″ |
| mc2_23 | 61.1 (std 15.72) | 59.1 (std 10.91) | ″ |
| mc2_24 | 40.6 (std 7.19) | 13.0 (std 2.02) | ″ |

ARM A's clamp cannot close a 61-luma gap (mc2_01/mc2_23) — it reduces *spread of
the bulk* (std 15.7→10.9) without moving min/max. ARM B's effective in-use
spread is **zero by construction**, which no atlas statistic expresses.

### Per-pose, two-run floor first

| map | pose | floor | ARM A px / ΔL | ARM B px / ΔL | hard-step OFF/A/B |
|---|---|---|---|---|---|
| mc2_01 | day_runway_ground | **0.0000%** | 27.3% / 3.77 | 18.5% / 10.82 | 0.072 / 0.128 / 0.083 |
| mc2_01 | day_runway_mid | **0.0000%** | 23.3% / 2.86 | 16.0% / 9.34 | 1.403 / 1.491 / 1.408 |
| mc2_01 | beach_north_wide | **0.0000%** | 0.0 / 0.00 | 0.0 / 0.00 | 0.132 / 0.132 / 0.132 |
| mc2_03 | sam_lightfacing | 0.0608% | 30.0% / 0.36 | 17.7% / 6.03 | 1.355 / 1.354 / 1.339 |
| mc2_03 | industrial_lightaway | **0.0000%** | 20.2% / 0.22 | 16.9% / 4.39 | 1.027 / 1.026 / 1.026 |
| mc2_03 | civstore_lightaway | **0.0000%** | 0.0 / 0.00 | 0.0 / 0.00 | 1.845 / 1.845 / 1.845 |
| mc2_05 | saved_102009 | ⛔ 14.43% | 16.9% / 4.47 | 19.8% / 5.48 | 1.102 / 1.095 / 1.093 |
| mc2_05 | east_compound_lightfacing | 0.1259% | 12.8% / 0.24 | 17.7% / 1.49 | 1.372 / 1.390 / 1.382 |
| mc2_05 | control_no_buildings | **0.0000%** | 0.0 / 0.00 | 0.0 / 0.00 | 0.000 / 0.000 / 0.000 |
| mc2_10 | saved_204937 | ⛔ 27.34% | 23.7% / 5.83 | 21.4% / 5.09 | 2.272 / 2.215 / 2.265 |
| mc2_10 | lance_close | ⛔ 30.58% | 29.8% / 3.39 | 30.6% / 3.80 | 2.494 / 2.448 / 2.395 |
| mc2_10 | saved_212019 | 0.2467% | 0.0 / 0.00 | 0.0 / 0.00 | 0.614 / 0.614 / 0.614 |
| mc2_23 | beacons_mid | ⛔ 7.72% | 25.7% / 16.14 | 20.1% / 14.39 | 1.474 / 1.430 / 1.480 |
| mc2_23 | runway_ground | **0.0000%** | 63.7% / 2.40 | 38.7% / 8.29 | 0.340 / 0.341 / 0.382 |
| mc2_24 | industrial_lightfacing | **0.0000%** | 52.9% / 0.83 | 26.9% / 14.76 | 3.029 / 3.028 / 3.042 |
| mc2_24 | saved_192316 | 0.0184% | 43.9% / 1.69 | 21.7% / 11.58 | 3.078 / 3.178 / **3.697** |
| mc2_24 | water_near | **0.0000%** | 0.0 / 0.00 | 0.0 / 0.00 | 0.000 / 0.000 / 0.000 |

⛔ = **FLOOR-INVALID**: the two-run floor exceeds the effect, so the pose carries
**no claim**. Marked as such in the pack. mc2_10's dynamic poses in particular
are unusable — floor 27–31% against arm effects of 21–31%.

**Hard-step %** (adjacent-pixel luma jump >16 — the metric that sank
`MC2_PAVED_OVERLAY_CLASS` at 0.6–10.8pp): **ARM A ≤ +0.06pp, ARM B ≤ +0.62pp**.
Both at or below the bottom of that rejected range; on several poses both
*reduce* it.

**No-pavement controls** (5 poses): both arms measure **exactly 0.00%** changed.
Neither gate touches anything but concrete — the scope ruling holds empirically.

---

## 5. RenderDoc — what I did instead, and why

I did **not** take a RenderDoc capture. Justification, since the brief demands
one either way:

The question a capture would answer is *"which atlas cell does this tile
sample?"* Two cheaper instruments answered it more directly:

1. **`MC2_CEMENT_ATLAS_DUMP`** (new) dumps the assembled atlas plus a per-cell
   slot/alpha CSV — the actual texture the shader samples, at source, without a
   graphics-debugger frame. This is what disproved the markings premise and
   what caught my own miscalibrated detector.
2. **ARM B is itself a causal experiment on the binding.** Changing only the
   cell index in `g_cementLayerIndexBySlot` moved concrete by 11–15 luma and
   moved no-pavement control poses by **exactly 0.00%**. That is a stronger
   demonstration that these tiles sample the cement atlas at the indexed cell
   than reading an eid/binding off a capture would be.

What this does **not** establish, and I am not claiming it does: the SPIR-V
program identity and descriptor binding at a specific eid. If the user wants
that specific confirmation, it remains open. The pre-composite/backbuffer
confound the brief warns about does not apply here — captures come from the
engine's own `gos_visual_capture` FBO readback, the same instrument every prior
lane in this pack used.

---

## 6. Process notes (things that went wrong, recorded)

- **The brief's "tree clean" premise was false.** 218 dirty files at preflight
  (all untracked scratch/docs), and `safe_to_touch=false`.
- **⚠ The shared worktree was under LIVE sibling edits mid-session.**
  `gos_postprocess.h/.cpp` changed every ~20s in a non-compiling state (an SSAO
  lane). My first build silently succeeded from a dirty tree
  (`MC2_BUILD_GIT_DIRTY=1`); the second failed on the sibling's broken header.
  **Any exe built there would have embedded a sibling's unreviewed changes into
  all three of my arms.** Resolved by isolating: `git worktree add
  A:/Games/mc2-cac2`, cloned + repointed the CMake cache, cold-built clean
  (`sha 7c80c004ada9`, `DIRTY=0`). This is the same failure class the brief
  flagged, and it did fire.
- **My first marking detector was wrong by design** — it tested `|ΔL| > 45` in
  both directions, flagged 78/110 cells, and left ARM B remapping only 31
  already-uniform cells, i.e. **visually inert in exactly the way that
  mattered**. Caught only by dumping the atlas. Recalibrated to bright-side-only
  at delta 60 (`7c80c004`); ARM B then remaps 109/110.
- `run_smoke` selects its deploy lane by LRU — it picked `0.4` (stale) rather
  than where I deployed. Needs `--deploy 0.4d-rc1` explicitly.
- Disk fell to **5.6 GB**; captures were pruned map-by-map. Pack images are
  1920-wide (76 MB total) rather than raw 3840×2160 (~25 MB/frame).

---

## 7. Gate hygiene

Both gates: `gate_status` verdict **OK** — registered in
`RendererFeatureRegistry.h`, documented in `docs/tier1_env_vars.md`,
smoke-allowlisted in `scripts/run_smoke.py`, default **OFF**, readers found.
The initial `ENV_DROP_RISK` flag (which would have silently run my ON arms as
OFF) was fixed before any capture.

**tier1 5/5 PASS**, `[DEPLOY_FINGERPRINT] OK: exe sha=7c80c004ada9 matches
worktree HEAD` logged on every one of the five missions.

### Limit of the "default OFF = byte-identical" claim

This is established **by construction and inspection, not by a golden diff.**
Verified: in the CEMENT-ATLAS-CELL-2 block, the only writes to `atlasBuf`
(`row[px] = …`) sit inside `if (ConcreteToneEnabled() …)`, and the only write to
`g_cementLayerIndexBySlot` sits inside `if (ConcreteCanonEnabled() …)`. With
both gates off the block only reads and emits the census, and the census itself
is behind `MC2_DIAGF` (`MC2_DIAG_VERBOSE`), so it is silent by default.

**Not done:** `scripts/run_golden_parity.py` against the blessed golden. I did
not build a pre-change exe to diff against, so "byte-identical" has not been
*measured*. The residual cost that is real, not hypothetical: the classifier
statistics run unconditionally once per mission load (110 cells × 64×64 texels
on mc2_24). Zero per-frame cost; load-time cost unmeasured.

---

## 8. Open / user-gated

- **The choice itself** — OFF vs ARM A vs ARM B, from §19. Not the lane's call.
- ARM A is expected to be rejected on mc2_24 (see §3 caveat). If the user wants
  a tone arm that is genuinely new, the measured target is the **in-cell dark
  regions** (78/110 cells), which neither arm addresses as such.
- mc2_10 needs a **static** pose set before it can support any claim; its
  current bookmarks have a 27–31% two-run floor.
- Isolated worktree `A:/Games/mc2-cac2` (~1.5 GB with `build64`) left in place —
  it is the only tree that builds this lane cleanly while the shared tree is
  contended. Remove with `git worktree remove A:/Games/mc2-cac2 --force` when
  the shared tree settles.
- Evidence copies: `.claude/cac2-evidence/` (per-map metrics JSON, the §19
  manifest builder, the metrics script).
