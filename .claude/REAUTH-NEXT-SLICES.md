# REAUTH-NEXT-SLICES — opus handoff after TERRAIN-REAUTH-UNPIN-1

Status: TERRAIN-REAUTH-UNPIN-1 SHIPPED on `claude/reauth-1` (`9b44f32a` Half A tool+bakes,
`8a0b9cdd` Half B engine objfade, `0bd24c5a` env registry). All default-OFF behind
`MC2_TERRAIN_VISUAL_DISPLACE`; byte-identical unset. slice_gate PASS (baseline 2/2,
gate-on 2/2 on spare lane v0.4), workbench 3/3 PASS, pytest 145/145.
Bakes staged for manual install: `staging/TERRAIN-REAUTH-UNPIN-1/` (INSTALL.txt inside).
Do NOT deploy 0.5-testing without the user.

## Shipped baseline numbers (what "better" must beat)

| mission | corr | extrema viol | crease bilinear->smooth (->final w/ detail) | drift mean/p99/max wu | detail RMS on rock | load amplitude mean wu (old bilinear ~5.9 mean) |
|---|---|---|---|---|---|---|
| mc2_01 (reauth only) | 0.9996 | 0/106 | 16.6 -> 4.9 (8.6) | 0.56 / 9.2 / 26.3 | — | 5.55, moved>5wu 21.6% |
| mc2_17 (+mountainify) | 0.9998 | 0/600 | 5.8 -> 1.2 (4.0) | 0.68 / 5.9 / 14.5 | 3.77wu (rock 23.9% area) | 6.11, moved>5wu 35.6% |
| mc2_24 (+mountainify) | 1.0000 | 0/573 | 36.1 -> 10.1 (30.8) | 0.93 / 9.8 / 25.0 | 3.95wu (rock 26.0% area) | 10.49, moved>5wu 40.4% |

Objfade grounding proof (in-engine `[VISUAL_DAMP v1] ... drift-near-objects`): inner
(damp==0) zone p99=0.000 max=0.000 wu on all three; fade annulus p99 4.98–8.56 wu.

## Knob reference (tools/terrain_beautify/visual_heightfield.py)

`--reauth --shape-tolerance 0.10 --max-drift 24 --reauth-passes 150
 --objfade-radius-wu 256 [--mountainify --mountainify-amp 14 --seed 1337]`
Gates recomputed from the .r32 by `terrain_workbench.py` (corner_unpinned,
shape_fidelity_corr>=0.99, extrema_preserved, facet_crease_reduced>=15%,
mountain_detail_present, blockiness_reduced). Tests:
`tools/terrain_beautify/test_visual_heightfield_reauth.py` (13).

## Slice 1 — REAUTH-PER-MISSION-TUNING-1

One global recipe fits nobody perfectly. Add a per-mission recipe sidecar
(e.g. `tools/terrain_beautify/recipes/<stem>.json`: tolerance, max-drift, passes,
mountainify on/off, amp, seed) consumed by the CLI; defaults = today's globals.
Known tuning targets from the shipped bakes:
- mc2_24: final crease 30.8 vs bilinear 36.1 — only ~15% net because amp-14 detail
  re-adds energy. Try amp 8–10 or slope-gated amp ramp; keep detail_rms >= 2wu.
- mc2_01: authored PYRAMIDS present (pyramid_top_score guard held at 0.749) — pyramid
  maps likely want lower --reauth-passes (~60) so facet edges stay reasonably crisp.
- mc2_17: max corner move only 14.5wu of 24 budget — gentle map; could take
  tolerance 0.15 for softer valleys.
Gate: workbench PASS per mission + eyeball contact sheets; no engine change needed.

## Slice 2 — MOUNTAINIFY-RIDGE-AWARE-V2

Current detail = isotropic ridged multifractal, amplitude = rock/slope mask only.
V2: orient detail along real ridge/drainage direction — coarse-grid Hessian
eigenvectors (or D8 flow accumulation) give a direction field; stretch the noise
domain along the ridge axis (2–3:1 anisotropy) so spurs run downslope instead of
popcorn bumps. Optional cheap thermal-erosion relaxation (talus angle clamp) as a
post pass. HARD constraints to keep verbatim: extrema guarantee (shape_tolerance),
tanh drift bound, water exclusion, footprint/overlay feather-pin, determinism from
--seed, workbench gates. Add a new gate: ridge_alignment (mean |dot(detail grad,
ridge dir)|) must beat isotropic baseline.

## Slice 3 — CORNER-CLAMP-TASTE-LADDER

The corner drift budget is taste, not correctness. Bake one mission (suggest mc2_24,
biggest relief) at a ladder: --max-drift 8 / 16 / 24 (shipped) / 36 / 48 with
matching workbench runs; also a --shape-tolerance rung 0.05 / 0.10 / 0.15.
Produce a single side-by-side contact sheet + cross-sections per rung
(tests/terrain/tune_* pattern from this lane is a good template) and let the user
pick per-biome defaults. Corner-unpinned gate ceiling is 1.25*max_drift — bump the
workbench want-line in lockstep. Cheap slice: tool + images only, no engine change.

## Slice 4 — REAUTH-BATCH-BAKE-ALL-MISSIONS

After taste ladder settles defaults: enumerate all mission stems in the missions dir
(same source as --missions-dir), bake reauth(+mountainify per recipe), run workbench
on every one, and land as one content commit. Practical notes:
- runtime ~seconds/mission; do 5-mission batches, workbench summary roll-up JSON.
- watch: water-heavy maps (shoreline band interplay — reauth already excludes water
  but check foam lines), pyramid/authored-geometry maps (pyramid_top_score guard),
  maps with dense bases (damp map coverage — inner-zone cell count sanity).
- smoke: tier1 gate-on pass on a spare lane before merge; per-mission drift log line
  `[VISUAL_DAMP v1] ... drift-near-objects` inner p99 must stay 0.000.

## Engine follow-ups (small, ordered by value)

1. Mover damp stamps re-upload side^2 floats (~57KB) per frame from code/mission.cpp —
   fine at 120^2, but dirty-rect or stamp-count early-out is a free win.
2. Damp SSBO = binding 27, GpuBufferOwner id 39 (RenderCore/RenderResourceRegistry.h);
   fold into GPU-BUFFER-OWNER arc conventions when that lands.
3. mode-1/mode-2 vert fix (Half B) made LOD1 far-band displacement live for the first
   time — if far shimmer is ever reported, first suspect the coarse far-band path,
   not the LOD0 bake line.
4. False crash_silent reminder: artifacts 2026-07-01T22-13-32 was lane contention
   (3 concurrent mc2.exe, healthy heartbeats to 29.2s/30) — rerun before believing.

## Evidence pointers

- Workbench: tests/terrain/workbench/{mc2_01,mc2_17,mc2_24}/workbench_report.json (3/3 PASS)
- Bake reports: tests/terrain/beautify/<stem>.beauty/visual_height_report.json
- Smokes: tests/smoke/artifacts/2026-07-01T{21-58-29,21-59-52} (01+24 off/on),
  2026-07-01T22-15-{10,58} (mc2_17 gate-on PASS x2, 48.6/51.4 fps)
- Tuning scratch (untracked, keep until taste ladder done): tests/terrain/tune_*
