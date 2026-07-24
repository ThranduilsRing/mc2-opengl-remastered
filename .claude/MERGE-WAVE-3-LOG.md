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

Final build/deploy at HEAD `fa3a5304` (clean tree). Two post-merge governance
reconciles were needed and committed before the final build:
- `18311749` shader_injectors: added inventory rows for the two new compile
  macros `MC2_SHADOW_FADE_RAMP0_F` / `MC2_SHADOW_ALT_SCALE_F` injected by the
  merged `c41e82d2` (RUNTIME_CONSTANT class, mirroring MC2_SHADOW_FADE_CONT_F).
- `fa3a5304` regen gated-layout-macros.md (gos_postprocess.h grew ~10 lines in
  the merge; the build-time stamper re-emitted VK-island member line numbers).

Verdicts (all against exe fingerprint `fa3a5304`, deploy lane 0.5.1):
- **Tier1 smoke: PASS 5/5** (mc2_01/03/10/17/24), DEPLOY_FINGERPRINT +
  DEPLOY_COHERENCE OK on all, MOVER-COHERENCE 0 violations.
- **Contracts: 65/65 PASS** (`scripts/check-contracts.sh`).
- **visual_verify vs `tests/visual/goldens/v0.5.1-baseline`:** 7 poses PASS,
  3 FAIL (9 regions), 3 NO_MANIFEST (the 3 extra install bookmarks with no
  golden). **The merge introduces ZERO new golden drift** — see §5 (control
  capture with all new gates OFF fails the identical 9 regions; every region's
  verdict is the same full-vs-control). The 9-region drift is pre-existing
  default-state evolution, NOT a regression (eyeballed: soft seamless palm
  shadows, clean night airbase, no rendering garbage).
- **Tracy GL baseline:** 1377 frames / 15s = **91.80 fps** at the mc2_10
  canonical pose (`-6656,-6656,150.625,10.9094,6045.2358@600`), free-timestep,
  uncapped (MC2_FPS_CAP=0), no PRESENT_VERIFY. Written to
  `release_assets/0.5.2/tracy/` (baseline.json + summary.md + v0.5.2-gl.tracy).
  VK arm skipped — Vulkan is not compiled into this build (islands fail-soft to
  GL). The prior baseline `release_assets/tracy-baselines/2026-07-23-proof` is
  degenerate (gl arm 1 frame / fps 0.07 / suspect) and on install v0.4c — not a
  valid regression target, so a fresh baseline was captured per RELEASE-PROCESS
  §3a rather than a `regress` compare.

Stray shell env `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE/FROZEN_RECORDS` +
`MC2_STATIC_POP_SPLIT` leaked into the session shell (source unknown, likely
user profile); all three are OUTSIDE the smoke allowlist so they were inert
(ENV-DROP warnings only) and did not reach the exe. Harmless to every gate.

## 4. RELEASE RE-CUT — v0.5.2

Patch bump v0.5.1 -> **v0.5.2** (fix round on the same feature set; supersedes
the held 0.5.1). Engine/shaders only — game data unchanged, so the 6 data zips
are byte-identical to v0.5.1's; only the engine zip carries new bytes.

- RELEASE_README.md header bumped to v0.5.2 (repo + copied into lane).
- Release notes: `.claude/RELEASE-NOTES-DRAFT-v0.5.2.md` (tracked) = shipped
  `release_assets/0.5.2/RELEASE_NOTES.md`.
- Deploy: `scripts/deploy_payload.py` exe+shaders+manifest to lane
  `A:/Games/mc2-opengl/releases/mc2-win64-v0.5.1` at `fa3a5304`
  (`--require-clean --allow-new-target`).
- Package: `DEPLOY=<lane> SRCDATA=A:/Games/mc2-srcdata OUTDIR=release_assets/0.5.2
  bash scripts/build_release.sh`. Engine zip enforces mc2-launcher.exe (present)
  + example mods staged from the REPO mods/ tree (4 mods).
- **Zips at `A:/Games/mc2-main/release_assets/0.5.2/`:** engine 608M (incl.
  launcher + mods/), gamedata 4.2G, burnins-4x pt1 1.5G + pt2 861M, movies 161M
  (215 BIK), art 89M, tgl.
- **ZIP GATE: PASS** — `test_release_zip_install.py` zips-only install (28459
  entries) + mc2_01 smoke PASS, verdict=PASS. `release_install_manifest.csv` +
  `release_install_report.json` archived alongside the zips.
- NOT DONE (user-gated, by design): git tag v0.5.2, push, GitHub Release +
  upload. NEVER pushed/tagged this session.

## 5. GOLDEN RE-BLESS PROPOSAL

**Do not silently overwrite** — a proposal for user review; the committed
`tests/visual/goldens/v0.5.1-baseline` is left intact as the backup.

Finding: the v0.5.1 golden no longer byte-matches the current default build, and
this is NOT caused by the WAVE-3 merge. Proof: a control capture of the 3 failing
poses with all three new default-ON fixes forced OFF
(`MC2_SHADOW_FADE_RAMP0=0 MC2_SHADOW_ALT_SCALE=0 MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP=0`)
still FAILs the identical 9 regions vs the golden. The drift is pre-existing
default-state evolution from fixes that landed on nifty AFTER the v0.5.1 golden
was blessed, several of which ship **default-ON**:
- `9f8c8086` two-sided beacon/runway edge-light glow cards
  (`MC2_LIGHT_BILLBOARD_TWOSIDED`, default ON) -> horizon_white_billboards,
  runway_edge_lights, light_clusters_census, lamp_cones.
- `c18eda8b` WATER-GRAZE-REFL-1 grazing-angle sky reflection -> horizon_waterline,
  far_water_surface, shore_central, shore_botright.
- WAVE-3's own default-ON fixes further nudge (not flip) some regions:
  SHADOW-FADE-RAMP0/ALT-SCALE -> tree_shadows_palm (softer seamless shadow);
  GHOST-CONE-RECIPE-SKIP -> light_clusters/runway/lamp_cones actually move
  CLOSER to golden (mean_delta drops) but stay above threshold due to the
  billboard/water drift.
NOTE: the task brief expected the golden to "mostly stand" because the session
fixes were assumed default-OFF. Two of them (billboard two-sided, water-graze)
are in fact default-ON — that assumption should be revisited.

Drifted regions (all verified intended, no regression — contact sheets eyeballed):
| pose | region | sev | why |
|---|---|---|---|
| mc2_01_saved_205644 | horizon_waterline, shore_central, shore_botright | med | WATER-GRAZE-REFL grazing reflection |
| mc2_01_saved_205644 | tree_shadows_palm | high | SHADOW-FADE-RAMP0/ALT-SCALE softer seamless shadow |
| mc2_10_saved_204611 | horizon_white_billboards | high | two-sided glow cards |
| mc2_10_saved_204611 | far_water_surface | med | WATER-GRAZE-REFL |
| mc2_10_saved_204753 | light_clusters_census, runway_edge_lights, lamp_cones | med | two-sided glow + ghost-cone-skip |

PROPOSED golden (uncommitted, on disk for review):
`tests/visual/goldens/v0.5.2-baseline-PROPOSED/` — blessed from the full-default
capture `scratch/vv/run_wave3` (39 region crops; identical/PASS regions carry
over byte-for-byte). Contact sheets for the changed regions are under
`scratch/vv/run_wave3/contact_sheets/<pose>/<region>__{candidate,golden}.png`.
To promote after review:
```
git mv tests/visual/goldens/v0.5.2-baseline-PROPOSED tests/visual/goldens/v0.5.2-baseline
# then repoint the smoke visual-advisory + release-process golden ref to v0.5.2-baseline,
# commit with blessed_commit=<HEAD>, and keep v0.5.1-baseline as the archived backup.
```
v0.5.1-baseline is untouched (backup). No golden was overwritten this session.
