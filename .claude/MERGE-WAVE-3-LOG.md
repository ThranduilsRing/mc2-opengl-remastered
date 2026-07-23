# MERGE-WAVE-3 LOG (2026-07-23)

Target: `claude/nifty-mendeleev` @ A:/Games/mc2-main (start HEAD `3adac7fe`).
Local only — NO push, NO tag. Conventions per MERGE-WAVE-1/2-LOG.md
(union functionality, prefer origin-branch versions, EOL-normalize, regenerate
generated files rather than hand-merging).

Lane: `WAVE-3-MERGE-RECUT-1` (deploy lane 0.5-testing).

Pre-merge housekeeping: 2 tracked doc edits stashed
(`WAVE-3 pre-merge stash`: .claude/agents/DOMAINS.md,
.claude/archive-2026-07-05/HANDOFF-mech-preview-fbo.md) so the merge/build tree
is commit-clean for the fingerprint. Restored at end of wave.

## 1. claude/deferred-sweep-1 (new tip `f96f3a8c`, delta since wave-2)

Wave-2 (`52f4e504`) merged the deferred branch at its then-tip `110281e7`.
Wave-3 brings only the delta `110281e7..f96f3a8c` (8 commits, fetched via
`git fetch A:/Games/mc2-deferred-1 claude/deferred-sweep-1` — merged FROM the
committed ref, the deferred worktree's working files were never touched):

- `c41e82d2` SHADOW-FADE-RAMP0-1 + SHADOW-ALT-SCALE-1 (seamless dist-fade,
  sun-altitude-scaled softness)
- `209073e3` CONE-AIM-DIAG-1 + CONE-AIM-FLIP-1 + GHOST-LIGHT-DUMP-1 (diag tooling)
- `09dafc9f` + `ae2b4811` GHOST-CONE-RECIPE-SKIP-1 (spotlight cone meshes no
  longer baked into static recipes; SpotLight_ TYPE geometry registered empty)
- `7c43e469` CONE-LEAF-NAME-DIAG + cone-leaf-name invariant
- `73755184` FAR-BACKDROP-NIGHT-LIGHTING-1 — **REVERTED, see §2**
- `e485f8e1`, `f96f3a8c` docs (batch-7 / batch-8C status)

**Merge commit: `b13a7cab`** (--no-ff).

Conflicts (2) + resolutions:
- `shaders/include/shadow.hglsl` — UNION. HEAD carried the
  `//PORT-BEGIN:calcDynamicShadowCsm` split-path marker; the branch inserted
  the SHADOW-ALT-SCALE-1 `sunAltSin = clamp(lightDir.z,0,1)` block at the same
  spot. Kept BOTH (marker first, then the sunAltSin block). `sunAltSin` is
  consumed downstream in the already-merged csmSampleFullMap/csmSampleLayer
  calls, so it must be defined.
- `docs/flags.auto.yaml` — REGENERATED via
  `scripts/flag-registry-auto-gen.py` (not hand-merged).

All other overlapping files auto-merged (gameos_graphics.cpp, gos_postprocess,
gos_static_prop_batcher, gos_terrain_lighting, gos_terrain_lod_chunk,
gameos.hpp, RendererFeatureRegistry.h, bdactor, gvactor, mech3d,
check-env-registry.sh, run_smoke.py).

## 2. SPECIAL HANDLING — 73755184 disposition (REVERTED)

`73755184` FAR-BACKDROP-NIGHT-LIGHTING-1 adds gate `MC2_BACKDROP_NIGHT_LIGHT`
**default ON**: a per-channel black floor `{0.10,0.11,0.14}` uploaded via a new
`u_worldAmbientFloor` uniform + a `max()`-floor in `gos_vertex.frag`, applied on
untextured depth-tested world draws (the far-backdrop horizon path).

Per the task brief this is an **A/B-verified NO-OP superseded by nifty
`16c2244a` (MC2_NIGHT_FOG_DIM)**: with night-fog-dim present the horizon is
already lifted above the proposed floor, so ON == OFF visually.

Disposition: **reverted entirely** in **`6e7bfda9`** (rather than merely flipping
the default), because:
- it guarantees the default build is byte-identical (no per-frame uniform
  upload, no shader floor at all — nothing to prove equal to zero);
- it removes a redundant gate overlapping the live night-fog-dim path;
- it keeps RendererFeatureRegistry.h + flags.auto.yaml free of a dead gate.

Revert backs out all 4 files (gameos_graphics.cpp, RendererFeatureRegistry.h,
gos_vertex.frag, flags.auto.yaml); yaml regenerated. Post-revert grep confirms
zero `MC2_BACKDROP_NIGHT_LIGHT` / `worldAmbientFloor` / `BackdropNightFloor`
references remain in GameOS/RenderCore/shaders.

## Preservation check
All 15 session fixes to preserve remain ancestors of HEAD (a8ea6726, ea93c1ad,
bc29eea8, d6cc4d9e, 5b12ddd7, 9f8c8086, c18eda8b, 588c446a, 51e4c326, a7fd0c21,
98ed1d32, 6af210b0, 16c2244a, d32a438f, 3adac7fe — all OK).

Post-merge HEAD after revert: `6e7bfda9` (+ this log commit).

## 3. POST-MERGE VERIFY
(build / tier1 / contracts / visual_verify / Tracy — appended below.)

## 4. RELEASE RE-CUT
(version + artifact paths — appended below.)

## 5. GOLDEN RE-BLESS PROPOSAL
(appended below.)
