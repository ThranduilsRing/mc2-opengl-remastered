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
