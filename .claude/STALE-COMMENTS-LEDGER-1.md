# STALE-COMMENTS-LEDGER-1

**REPORT ONLY** (DOCS-RETIREMENT-SWEEP-1, 2026-07-01). Code comments that contradict
current behavior, found during doc-hygiene review. **Do NOT batch-fix from this doc** —
each entry is owned by the named arc/lane; fix the comment when next touching the file.
All file:line refs verified today against the **canonical nifty worktree**
(`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`). Re-grep before editing —
lines shift.

| # | File:line (nifty) | Comment claims | Actual truth (verified 2026-07-01) | Owning arc |
|---|---|---|---|---|
| 1 | ~~`GameOS/gameos/gos_postprocess.cpp:37`~~ **RESOLVED 2026-07-01 (LIGHTING-STAGE1 CSM-VERIFY-TUNE-1)** | `MC2_SHADOW_CSM : master gate, DEFAULT OFF. OFF => legacy single dynamic map path is byte-identical` | Code at `:45` is `!(v && v[0]=='0' && v[1]=='\0')` — **DEFAULT ON**; only `=0` disables. Default-ON since 2026-06-18 (`8ff13a36`); `docs/tier1_env_vars.md:103` correctly documents ON. Comment corrected in this worktree (`A:/Games/mc2-controlmap-sample-1`) to state default-ON; re-verified at runtime via `gaea_peaks_01` smoke log with no `MC2_SHADOW_CSM*` env set — `[CSM] init nearLayers=2 arraySize=8192 fullMapSize=4096 separate=1` / `[CSM] layers=3 casters_per_layer=0` both fire. | lighting / CSM |
| 2 | ~~`GameOS/gameos/gos_postprocess.h:199`~~ **RESOLVED 2026-07-01 (LIGHTING-STAGE1 CSM-VERIFY-TUNE-1)** | `Gate (MC2_SHADOW_CSM, default OFF). When OFF the legacy single-map path…` | Same gate as #1 — **default ON**. Comment corrected alongside #1 (same commit). | lighting / CSM |
| 3 | `mclib/terrain.cpp:1229` | Load-time printf reports `"(Stage 1: SSBO only, geometry unchanged)"` for the visual-height bake | **FALSE under the displace gate**: Stage 2 shipped — with `MC2_TERRAIN_VISUAL_DISPLACE` on, `terrain_lod_chunk.vert` samples binding 26 and **moves geometry** (checkerboard pixel-oracle-proven). The stale text repeatedly caused the pipeline to be misdiagnosed as broken. Corrected wording (TERRAIN-DISPLACEMENT-TRUTH-1) is already staged in the terrain-v2 lane's working tree (`A:/Games/mc2-controlmap-sample-1` `mclib/terrain.cpp` ~:1233-1245, uncommitted) — do not double-fix. | terrain-v2 (TERRAIN-VISUAL-HEIGHT / DISPLACEMENT-TRUTH-1) |
| 4 | `GameOS/gameos/gos_terrain_lod_chunk.cpp:1162` | `u_shaderTime is uploaded unconditionally (cheap scalar)` | Upload at `:1179-1180` sits **inside** `if (shorelineMaskReady)` — it is uploaded **only when a shoreline mask loaded**. Benign today (frag reads it only in the gated shoreline block) but misleads any future consumer of `u_shaderTime` expecting a live clock. | terrain-v2 (TERRAIN-SHORELINE-MASK-1) |
| 5 | `mclib/terrain.cpp:2055` | `[LOW-CAMERA-TERRAIN-CULL-1 / FIX-2] … default OFF -> byte-identical to today.` | Next line + code (`:2056-2057`): `MC2_LOWCAM_TERRAIN_NEAR` is **default ON** (`!(v && v[0]=='0')`). The "default OFF" sentence is a leftover from the recon plan; the adjacent "Default ON for this low-camera build" sentence is the live truth. Also: `MC2_LOWCAM_TERRAIN_NEAR` is **absent from `docs/tier1_env_vars.md`** (registry gap). | low-camera-ground-mode (LOW-CAMERA-GROUND-MODE-RECON-1) |
| 6 | `code/brain_tactic_select.h:5`, `code/mech_brain_runtime.h:116`, `code/warrior.cpp:2341` | `TRACE-ONLY: NOT wired into attackParams.tactic (deferred to TACTIC-WEIGHTS-B)` / `NO behavior write (deferred to TACTIC-WEIGHTS-B)` | **TACTIC-WEIGHTS-B SHIPPED** `743dff9e` (2026-06-25, merged `a62a7811`, on nifty): `brain_special_dispatch.cpp:252` implements `MC2_TACTIC_WEIGHTS_B` gate (default OFF) that writes `runtime->selectedTactic` into `attackParams.tactic` at `:1422`/`:1913`. The "deferred" framing is stale — B exists; selection is trace-only ONLY when the B gate is off. | brain / techscript lane (TACTIC-WEIGHTS) |
| 7 | `code/mech.cpp:1343`, `GameAdapters/MechRenderAdapter.h:43` | `gameObjectId=0 in M2; M2.5 refines when object-ID writes need correlation` (future tense) | **M2.5 SHIPPED** `8f8be648` (2026-05-23, "M2.5 SHIPPED -- mech ObjectID substrate active", `docs/renderworld_arc_status.md:22`). M2.5 landed object-ID correlation via the mech batcher / `registerMech` route (`mclib/mech3d.cpp:2993`), NOT by refining the `syncSpawn` cookie — which deliberately stays 0. Comments should say the cookie is intentionally unused post-M2.5, not that M2.5 is pending. | RenderWorld arc (M2.5) |
| 8 | `docs/water-reflection-plan.md:51`, `docs/water-lighting-plan.md:48-51` (in-tree plan docs read as current state) | S3 reflection block `present, compile-time DEAD (const bool S3_REFLECTION_ENABLED=false)` at `mdi.frag:70/132-173`; `Fresnel stays shelved` | **Reflection SHIPPED and DEFAULT-ON**: `shaders/gos_terrain_water_mdi.frag` no longer contains `S3_REFLECTION_ENABLED` at all — Fresnel-mixed SH-sky + HDRI + terrain-RT reflection is live (WATER-SKY-REFLECTION-1 / WATER-HDRI-REFL-1 / WATER-REFLECTION-SAMPLE-1), default ON strength 1.5 (`gameos_graphics.cpp:2749` WATER-REFL-DEFAULT-ON; `MC2_WATER_REFLECTION=0` kills). The shader's own inline comments are already self-corrected (`mdi.frag:200` STALE NOTE); the plan docs are the remaining misleading layer. | water-v2 / water-reflection lane |

## Sweep method + coverage

- Named candidates from today's reviews (#1-#4 leads) each verified against live code, not just docs.
- Automated pass over nifty `GameOS/gameos`, `mclib`, `code`, `RenderCore`, `RenderWorld`, `GameAdapters`
  (.cpp/.h): comment lines saying `default OFF/ON` within 4 lines of the opposite getenv idiom
  (`!(v && v[0]=='0')` = ON; `v && v[0]=='1'` = OFF). One hit beyond the named set (#5).
- NOT covered: `envFlagDefaultOn()` helper sites, `atoi`-default gates, multi-line comment blocks
  further than 4 lines from the getenv — a follow-up sweep could extend the idiom set.
- Cross-check note: `docs/tier1_env_vars.md` (nifty, verified-current banner 2026-07-01) agrees with
  code on `MC2_SHADOW_CSM=ON` and `MC2_TERRAIN_LOD_CHUNK=ON` — the doc layer is current; the stale
  claims live only in the inline comments listed above.

## 2026-07-16 appends (deferred-board A#2 de-stale sweep)

Entries #6-#8 added from the cross-worktree ledger de-stale sweep
(`mc2-opengl-src/.claude/DEFERRED-BOARD-2026-07-16.md` item 2). Each superseding
commit re-verified against `A:/Games/mc2-main` git (`claude/nifty-mendeleev`
ancestry confirmed) and the stale comment lines re-grepped on both nifty and this
worktree before recording. Same rule as above: report-only; fix each comment when
its owning lane next touches the file.
