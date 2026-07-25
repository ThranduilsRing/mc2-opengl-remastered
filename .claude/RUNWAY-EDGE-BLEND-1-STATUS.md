# RUNWAY-EDGE-BLEND-1 — STATUS (MASTER-FAIL #6)

**Branch:** claude/nifty-mendeleev @ `d6cc4d9e` (mc2-main)
**Gate:** `MC2_RUNWAY_EDGE_BLEND` (Feature, **default OFF / killswitch**)
**Date:** 2026-07-23

## Problem
Master-FAIL #6 (HIGH): runway/landing-pad cement edges render as hard
full-tile stairstep cliffs where cement meets terrain, with no blend.

## Fix (SDF half-tile feather from the cement mask)
Cement pads/runways are drawn per-quad from the cement-word `_wp3` field
(`cementWord`). Solid cement tiles do a full-tile replacement, so a solid
cement tile that abuts **bare terrain** (no cement neighbour, no authored
transition art — the MISSING-RUNWAY / airbase-perimeter class) is a hard
square → the stairstep cliff.

- **CPU** (`gos_terrain_indirect.cpp`, `PopulateRecipeCementWords`): a gated
  second pass packs a 4-neighbour **"open border" mask** into `cementWord`
  bits 19:16 for exactly those solid boundary tiles. A border is "open" only
  when the neighbour carries **no** valid cement word (neighbours with any
  valid word — solid OR transition — count as cement, so authored transition
  edges stay with the legacy blend / marching-squares path). Written to both
  the dense recipe (`_wp3`) and the `cementWordsF` SSBO.
- **Shader**: reads the mask and ramps cement → base terrain over the outer
  **half tile** (`smoothstep(0, 0.5, dist)`) — a discrete SDF from the cement
  mask. Distances are seam-free (a border shared by two cement tiles is never
  open for either). Applied in **both** terrain paths:
  - `shaders/gos_terrain.frag` (indirect path) — commit `bc29eea8`
  - `shaders/terrain_lod_chunk.frag` (LIVE LOD-chunk path) — commit `d6cc4d9e`

**Killswitch is CPU-side:** gate OFF ⇒ the second pass never runs ⇒ no mask
bits ⇒ both shaders' `featherAlpha` collapse to 1.0 ⇒ byte-identical stock
full replacement. **No new GL uniform** was added (avoids the contended
`gameos_graphics.cpp`); the gate rides entirely on the mask bits.

## Key finding (scope)
The **LIVE terrain renderer is the LOD-chunk path** (`terrain_lod_chunk.frag`,
confirmed `[TerrainLodChunk] shader loaded` in capture logs), NOT the indirect
path the task originally pointed at. The initial indirect-only fix produced
**0 change** in the visible `pad_edge` region — hence the chunk-path extension
(`d6cc4d9e`), which is where the fix actually becomes visible in-game.

The chunk path already shapes **transition** tiles with a deliberate hard
marching-squares diagonal cut (CEMENT-HARD-EDGE-1, user-verified crisp
outline). This slice **does not touch** that — it only feathers the
**solid-cement-vs-bare-terrain** case, the genuine residual stairstep.

On the two available airbase bookmarks, solid-cement-vs-bare tiles are RARE
(mc2_01: 4 tiles; mc2_10: 0 tiles) because those maps ring their pads with
transition art. The fix will feather many more tiles on maps whose runways
are authored as solid cement abutting terrain (the class that triggered #6).

## Verification (tools/visual_verify, full-zoom crops, deterministic)
Install: `releases/mc2-win64-issuefix-test` (isolated lane, exe `d6cc4d9e`).
Evidence: `<session-scratchpad>/{off,on,on2,off2}/` + crops `re_*`, `rw_*`.

| Check | Verdict | Evidence |
|---|---|---|
| Bake armed (gate ON) | PASS | `[RUNWAY_EDGE_BLEND] event=edge_mask_baked feathered_solid_tiles=4` (mc2_01), `=0` (mc2_10) |
| Feather visible on LIVE chunk path (day, mc2_01) | PASS | solid apron/runway edge ramps cement→terrain over a soft diagonal; terrain diff 2490px, max Δ185 (a blend, not a swap) at x[2450-3834] y[391-606] |
| Night airbase (mc2_10_204611) | N/A (no solid-vs-bare tiles) | 0 feathered tiles; pose has no eligible edges |
| **Killswitch OFF = stock** | PASS | new-exe gate-OFF vs stock baseline: **0 terrain px changed, max Δ0** |
| `pad_edge` region gate-OFF | PASS | 0 px changed |

Verify-by-exaggeration was not run as a separate build; the A/B diff already
localizes the feather as a clean diagonal ramp, and the max-Δ185 (vs a 255
hard swap) confirms partial-coverage blending rather than replacement.

## Residual risk / follow-ups
- **Effect is small on transition-ringed pads.** The visible edge on most pads
  is the transition marching-squares diagonal (hard 0/1 alpha, deliberate). If
  the user still reads that as "no blend", a **follow-up taste slice** could
  soften the chunk-path transition alpha into a smoothstep band — but that
  risks regressing CEMENT-HARD-EDGE-1 and should be user-gated, not done here.
- Night verification is mechanism-only (no eligible tiles in the one night
  airbase bookmark). A night pose over a solid-cement runway would strengthen it.
- Deploy exe carries a **dirty fingerprint** (the shared mc2-main worktree has
  172 unrelated dirty files); the committed source (`d6cc4d9e`) is clean. This
  is a verification-only install, not a release exe.

## Files
- `GameOS/gameos/gos_terrain_indirect.cpp` — gate `RunwayEdgeBlendEnabled()` + CPU 2nd pass
- `shaders/gos_terrain.frag` — indirect-path feather
- `shaders/terrain_lod_chunk.frag` — LIVE chunk-path solid feather
- `RenderCore/RendererFeatureRegistry.h` — gate registration
- `docs/tier1_env_vars.md` — gate docs
- `scripts/run_smoke.py` — passthrough allowlist

Commits (local only, NOT pushed): `bc29eea8`, `d6cc4d9e`.

---

# REVISION 2 — RUNWAY-EDGE-DILATE-1 (2026-07-24, lane RECAP-MECHSHADOW-RUNWAY-1)

**Revision 1 above is REJECTED by the user.** Verdict on the mc2_01 day apron
pair: *"gate on is worse. the answer is usually MORE concrete, not less."*

## Root cause of the bad look
Rev-1 ramped cement -> terrain **inside** the solid pad tile
(`smoothstep(0, 0.5, dist)` over the outer half tile). That is an EROSION: it
dissolves the outer margin of the apron the player can see, so the pad reads
smaller and its edge reads blurry/dirty. Measured at the mc2_01 apron pose,
3840x2160: **5958 px changed, mean luminance -6.55, only 25.2% of changed px
brighter** — i.e. the gate was subtracting concrete. That matches the complaint
exactly.

## Revision 2 (commits `8e59de64`, `3c6aad71`)
Same gate `MC2_RUNWAY_EDGE_BLEND`, same CPU bake, **no C++ change**.

- `shaders/terrain_lod_chunk.frag` (LIVE path):
  - Solid cement tiles go back to **crisp FULL replacement** out to their own
    tile boundary. No erosion anywhere.
  - New `RUNWAY-EDGE-DILATE-1` block runs on the **bare-terrain tile**: if an
    orthogonal neighbour is a solid cement tile whose CPU-baked open-border
    mask (cementWord bits 19:16) flags this side, blend that neighbour's cement
    over the terrain — alpha **1.0 exactly at the shared edge**, falling to 0
    one tile out. Seam-continuous with the crisp cement tile by construction.
  - Net: the concrete footprint **GROWS** by up to a tile with a soft outer
    margin. The stairstep is softened by adding, never by removing.
  - Neighbour bit convention is mirrored (E neighbour flags me as its W, etc.).
- `shaders/gos_terrain.frag` (indirect path, NOT the live renderer): rev-1
  erosion removed; solid cement is stock full replacement. That path is
  per-quad off a thin record with no neighbour access, so it cannot host the
  dilation; documented in place.

**Killswitch unchanged and still CPU-side**: gate OFF => no mask bits baked =>
no neighbour is ever flagged open => the dilate block is a no-op =>
byte-identical stock. Transition tiles and the CEMENT-HARD-EDGE-1
marching-squares cut are untouched.

## Verification (exe built at `3c6aad71` (deploy manifest records src_commit `ea999e8b` — a PARALLEL lane advanced HEAD between build and deploy; the shader source deployed is `3c6aad71`, md5-verified against the install), install `mc2-win64-abl-validate`, 3840x2160)

| Check | Verdict | Evidence |
|---|---|---|
| Gate arms per arm | PASS | OFF: no `edge_mask_baked` line; ON: `feathered_solid_tiles=4` |
| Direction reversed vs rev-1 | **PASS** | rev-1: mean dLum **-6.55**, 25.2% brighter. rev-2: mean dLum **+2.75**, 68.7% brighter |
| Concrete GROWS, not erodes | PASS | rev-2 changed px are majority brighter/greyer = cement laid over terrain |
| Verify-by-exaggeration (reach 1.5 tiles) | PASS | 2068 px, mean dLum **+61.68**, **99.7% brighter** — scales cleanly, only ever adds |
| Killswitch OFF = stock | PASS | by construction (CPU bake gated); OFF arm shows zero mask bits |

Reach knob: shipped at **1.0 tile**. At the spec'd 0.5 it measured only 70 px
at 4K on this pose — real but below perceptual threshold; 1.5 was the
exaggeration arm.

## HONEST RESIDUAL — magnitude on the available bookmark
Direction is fixed and the erosion is gone, but the shipped effect on mc2_01 is
**close to imperceptible at gameplay zoom**, for the reason already recorded in
rev-1: mc2_01's apron perimeter is authored **transition** art (the deliberate
crisp outline this gate does not touch), so only **4 tiles** map-wide are
solid-cement-abutting-bare-terrain, and they sit distant at a grazing angle.
The BEFORE/AFTER pair in the review pack genuinely looks near-identical; that
is the truth of the map, not a capture failure. Grading the look still needs a
map whose runways are authored as solid cement.

Also noted: of the 536 changed px in the rev-2 arm, the largest cluster
(x~2400, y~320) is **shoreline water animation**, not the fix — the fix's own
contribution is smaller still. Recorded so nobody over-reads the pixel count.

## Follow-up left on the table (NOT done — needs a user call)
The edge the user can actually SEE on these pads is the transition
marching-squares outline. Dilating a soft skirt OUTWARD from transition tiles
too would give visibly "more concrete" at the real apron boundary without
eroding the crisp outline — but it softens a shipped, user-approved look
(CEMENT-HARD-EDGE-1), so it is a taste call, deliberately not taken here.

## Files (revision 2)
- `shaders/terrain_lod_chunk.frag` — crisp solid + RUNWAY-EDGE-DILATE-1 block
- `shaders/gos_terrain.frag` — rev-1 erosion removed
Commits (local only, NOT pushed): `8e59de64`, `3c6aad71`.
