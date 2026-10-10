<!-- doc-status: stale | reviewed:2026-10-11 -->
# HANDOFF — Model-Override + Tree LOD (resume cold)

**Branch:** `claude/model-override-system-recon-1` (worktree `A:/Games/mc2-model-override-recon`). **HEAD:** `9957a3ab`.
**Running log:** `docs/model-override-mvp-notes.md` (every slice). This file = the fast resume pointer.
**Heed:** `memory/mc2_deploy_validate_env_gotchas.md` — hung `mc2.exe` LOCKS the deploy exe → `cp` silently fails → stale-exe runs (the #1 time-sink). `strings` is broken; bash `grep` mis-reads UTF-16 logs → use **PowerShell `Select-String`**. Build **`--target mclib` THEN `--target mc2`** (mc2-only can skip mclib recompile).

## DONE + verified in-game (v0.3 / mission `mc2_01`)
Modder drops a glTF, declares `replaces`, it renders textured with gameplay/collision stock. Chain (all committed):
- **Registry+validation** (Slice 1): `mclib/model_override_registry.{h,cpp}`, `data/model_overrides/models.json`.
- **Dual-shape collision safety** (Task 0 = CASE A): collision reads stock `treeShape[]`/`bldgShape[]`; override only on `*RenderShape`. `calcCellsCovered`/`markTerrain` UNTOUCHED.
- **3 render seams fixed:** register render shape pre-`finalizeGeometry`; `isStaticEligible` bdAnim gate skip (`566097f0`); untextured override packets `layerForPacket=-1` → routed to valid layer via `isOverride` flag (`4171be63`).
- **glTF texture binding** (`e0cdcdf0`): importer `DeriveMC2TextureName` → `a_`-prefix = alpha-cutout leaf convention; `LoadOverrideRenderShapeTextures`. Lush leaf-card-preserving tree renders textured w/ alpha-cut leaves (verified A/B `.claude/lush_apron_tree_zoom.png`, `vlx_textured_livetrees.png`).
- **GPU-INSTANCE-SKIP-POOLS-1** (`313df6aa`): static props ARE GPU-instanced (geometry once in VBO + O(1) `lightDataIndex`); skipped the vestigial per-instance `TransformMultiShape_PositionsOnly` pool alloc for registered types → TGL pools 99%→0%. `MC2_LEGACY_INSTANCE_POOLS=1` reverts. (Pools were bumped 16M/8M in `mission.cpp` — now UNNEEDED, revertable.)

## THE BLOCKER (where the next session starts)
**`Render.GpuStaticProps` = 2.29 s/frame** on the override forest. Greybeard + render-spine pinned it: **no LOD** (trees pinned LOD0 → full 706k-tri mesh at all distances) + alpha-test leaf **overdraw** (no depth pre-pass). NOT lighting (that stale "32-slot UBO" theory was wrong — corrected `fc4c363c`; the light SSBO is unbounded, registered overrides are O(1) baked).

TREE-OVERRIDE-LOD-MVP-1 ran the **full pipeline**: recon (`docs/model-override-lighting-lod-recon.md`) → spec (`docs/tree-override-lod-spec.md`) → render-spine review (EXECUTE) → plan (`docs/tree-override-lod-mvp-1-plan.md`) → plan review (REVISE→folded) → execute. **Tasks 1–3 plumbing is DONE and no-op** (`697cbd7d`, `c1fbc9ac`): `treeRenderShape[MAX_LODS]`, `staticReg[MAX_LODS]`, `activeLOD` (pinned 0), per-LOD register/bake/import from `getTreeRenderShape(lod)`, minimal manifest `lods[]` parse, a 235k-tri `tree_lush_lod1.glb`.

**Task 3 K×M light-slot GATE = STOP (HARD, owner-mandated):** registering+baking LOD1 alongside LOD0 makes each instance's LOD1 take a **fresh** light slot (`lod_distinct_slots=148` = 1:1 with K instances; light table +148). **Why:** the per-instance `lightDataIndex` dedup is content-keyed on the gathered per-leaf `TG_HWLightsData` (`txmmgr.cpp:1290`), which DIFFERS per LOD mesh (different first SHAPE_NODE leaf / per-vertex light accumulation) → memcmp misses → K×M growth. (Single-LOD `U==K` is pre-existing & benign — stock does it too; the LOD *multiplication* is the blocker.) Full numbers: `docs/model-override-lighting-ownership-recon.md`.

## NEXT — the lighting-ownership slice (unblocks LOD)
Make per-instance `lightDataIndex` resolve through a key INDEPENDENT of LOD mesh so all LODs of an instance (ideally all instances of a type in a terrain cell) collapse to a bounded slot set. Options (`lighting-ownership-recon §5`): (1) position-quantized/cell-keyed shared light; (2) instanced GPU light gather (no per-instance CPU slot); (3) recipe-keyed reuse. **Re-gate criterion:** rerun `MC2_LIGHTSLOT_TRACE=1` with the 2-LOD `tc1_1` manifest → require `lod_distinct_slots ≈ 0-new` and `table_count` flat across LOD count, then re-open **LOD Tasks 4–6** (distance `currentLOD` select + `MC2_FORCE_LOD` gate + black-tree regression gate).
- This touches the SHARED static-prop light path (all props, not just overrides) → likely its own recon→spec→review→plan→review→execute.
- **Coordinate with the parallel TrackV whole-frame-CPU session** — it hit the same per-instance `U=K` finding (see the TrackV handoff in MEMORY.md; `StaticPropRegistryFlush`/`LightDataUpload` ownership).

## Perf-measurement convention (owner ruling)
Use **`cost-split`** CostSplit buckets (`.claude/skills/cost-split-recon-bucket-design.md`; shipped pattern `gos_terrain_indirect.cpp`, env `MC2_*_COST_SPLIT`) for frame-rate/call-count/zone decomposition — **summary cadence at 10 frames** (not 600) so `--validate --frames 20` emits it. Honor 100ns floor / <10% overhead. Relay the summary line back each run.

## Key file:line anchors (re-grep — lines drift)
`TreeAppearance::registerStatic` `bdactor.cpp:4806`; tree `markVisible` `:4226`; `getTreeRenderShape` `mclib/bdactor.h:603`; `treeRenderShape[MAX_LODS]` `mclib/bdactor.h:603`; tree pinned `currentLOD=0` `:3834`; `lodDistance[]` read (×5 zoom-push) `:3634`; light dedup `addLightDataStructure` `mclib/txmmgr_lights.cpp:586` (memcmp `:1290`); `mc2CacheOrBakeStaticGpuLight` `mclib/bdactor_lightbake.cpp:186`; `finalizeGeometry` one-shot immutable VBO + `s_geometryFinalized` `gos_static_prop_batcher.cpp`. LOD-swap-black cure (re-arm `needsFullBakeNextFrame`) `docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md`.

## Deploy / run
Build (VS cmake `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`, `build64`, RelWithDebInfo) → deploy to `A:/Games/mc2-opengl/mc2-win64-v0.3` (v0.4 also valid; NOT v0.3's `mission.fst`-only set — `mc2_01` works on both). Run `mc2.exe --validate --frames 20 --log X.json --screenshot X.tga -mission mc2_01 2> X.txt`. Deployed demo manifest = lush 6-type (tc1_1..4, palm1, palms → tree_lush.glb); shipped repo manifest stays `{"overrides":[]}` (no-mod identity). Env-gated traces left in: `MC2_LIGHTSLOT_TRACE`, `MC2_MODOVERRIDE_TRACE`, `MC2_SUBSTRATE_COALESCE_TRACE`, SEAMPROBE.

## Loose ends / debt (non-blocking)
- Revert the 16M/8M pool bump in `code/mission.cpp`/`mission2.cpp`/`txmmgr.h` (unneeded post-skip-pools) — or keep until lighting-ownership lands.
- Branch (KHR_texture_transform UV offset) not applied on override branch material — minor mis-tile.
- Building LOD generalization deferred (trees-only MVP).
- Add a model-override row to `docs/asset-pipeline.md` §7.
- Stale tree_export scripts in `.claude/` (`tree_export.py`, `tree_export_lush.py`, `tree_export_lush_lod1.py`) + `.claude/gltfpack_build/` (gltfpack.exe) are the working asset tooling.
