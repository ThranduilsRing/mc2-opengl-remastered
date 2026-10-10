# Gate deletion audit (pre-G1) — 2026-10-09, read-only adversarial pass at HEAD 2f55cdddc

Read-only. HEAD `2f55cdddc` on `claude/nifty-mendeleev`. The tree is dirty with the seam#2 agent's work: `GameOS/gameos/gos_mech_batcher.h`, `shaders/mech.vert`, `shaders/shadow_mech.vert`, `tools/gpu_abi_gen.py`, `shaders/spv/*`. Any edit batch must stage by path and leave those alone.

**Method.** I ran `git grep -nwI` per gate, plus a whole-repo `grep -rIwF` that also covers untracked files, `.bat`, `.json`, the memory dir and `.claude/`. I read every reader site. The Tier A scan was re-run with `repo_query.py dead-gate-scan --tier A` and matches the plan.

**Scope.** 48 gates: 28 Tier B DELETE, 12 Tier A (DELETE-SAFE plus NEEDS-READER-REMOVAL), 8 terrain. All have default OFF, or are retired no-ops. None is default-ON, so every deletion is "delete the ON path or dead code"; the default path stays unconditional.

## Consumers every batch must handle (the plans do not list the first two)

1. **`docs/flags.auto.yaml` regen.** `check-contracts` runs `flag_registry_drift --strict` (`check-contracts.sh:812`). The generator records reader files, so deleting any getenv in a file needs `py -3 tools/gate_gen.py --write` in the same commit.
2. **`scripts/gl_shader_variants.json`.** The `gl_shader_variants` contract demands it equal a fresh re-extraction. Each of the 231 entries carries `source: file:line` of a `makeProgram` call.
   - Any edit that shifts lines in a source file requires `py -3 scripts/extract_gl_shader_variants.py`. It is not in `terrain-gates-plan.md` or `tierA-gates-plan.md`.
   - Affected files: `gameos_graphics.cpp` (153 entries), `gos_postprocess.cpp` (20), `gos_static_prop_batcher.cpp` (20), `gos_terrain_lod_chunk.cpp` (12), `gos_mech_batcher.cpp`, `gos_terrain_indirect.cpp`, `gos_terrain_lighting.cpp` and others.
   - Deleting the cliff-tess program also deletes the `terrain_lod_chunk_tess` entries (lines 4026, 4050, 4070, …).
3. **`scripts/check-env-registry.sh` allowlist rows.** Stale rows are harmless, since the check is one-way, but delete them. Every Tier B gate has one row (the lines above 1000 are `# trace` rows).
4. **`run_smoke.py` passthrough tuple.** It is vestigial, because the runner copies the parent environment (`check-smoke-env-inheritance.py`). Entries can go any time, but delete the reader before the entry for registered gates (`check-registered-gate-passthrough.py`).
5. **`registered-gate-passthrough-debt.txt`.** It is two-way. Once a reader is removed, the debt line must be deleted in the same commit, or the check fails. This applies to V1, V2 and NFH.
6. **Reader before registry row.** Delete the getenv reader before or with the registry row, or `env_registry` fails on an unregistered reader.

## Verdict table

Shape for every Tier B and terrain row is delete-ON-path unless stated. Default is OFF. "Rd" is the number of env-read lines. Every Tier B row also carries one allowlist row and one yaml entry (the common items above). The Consumers column lists extras only.

| Gate | Plan / verdict | Rd | Extra consumers and notes | Verdict, why |
|---|---|---|---|---|
| ANIMATED_PROP_PROBE | B/DEL | 1 (`bdactor.cpp:1071`) | none | SAFE |
| ANIM_ADVANCE_TRACE | B/DEL | 1 (`mech3d.cpp:5636`) | ANDed with `mc2_diag::tagEnabled("ANIM_ADVANCE")` | SAFE, delete the whole block, not just the env term |
| BRAIN_ENGAGE_TRACE | B/DEL | 1 (`brain_special_dispatch.cpp:2012`) | `check-brain-opord.py` parses `[BRAIN_ENGAGE]`, a different tag | SAFE |
| CLOUD_DIAG | B/DEL | 1 (`gos_postprocess.cpp:3431`) | manifest regen | SAFE-WITH-CONSUMER-FIX |
| DEPLOY_KIND_TRACE | B/DEL | 1 (`mission.cpp:2697`) | none | SAFE |
| EDITOR_DRAG_TRACE | B/DEL | 1 (`EditorInterface.cpp:1745`) | editor target | SAFE |
| EDITOR_MOVER_TRACE | B/DEL | 1 (`EditorObjectMgr.cpp:1387`) | helper `editorMoverTraceOn()` has 4 users (1435, 1588, …) | SAFE, remove helper and all 4 |
| EDITOR_PROJECT_TRACE | B/DEL | 1 (`FoliageRender.cpp:239`) | `s_trace` at 331, 350 | SAFE |
| FL_TRACE | B/DEL | 1 (`missiongui.cpp:3225`) | observe-only | SAFE |
| FRAME_JOBS_PATHB_DIAG | B/DEL | 2 (`objmgr.cpp:2768,2772`) | `run_smoke.py:2472`; three code regions (preamble 2765-2795, loop 2945-2962, post-loop 3261-3305) | SAFE, but it is a hot loop. `smoke --lite` and tier1 are required |
| GPU_SCENE_SCALE_PROBE | B/DEL | 1 (`gos_static_prop_registry.cpp:1742`) | none | SAFE |
| HDRI_SKY_STATE_PROBE | B/DEL | 3 (`gos_postprocess.cpp:5334,5627,5813`) | manifest regen; sky has a VK port, but this is print-only | SAFE-WITH-CONSUMER-FIX |
| LIGHTBRIDGE_COMMIT_TRACE | B/DEL | 1 (`objmgr.cpp:2747`) | `run_smoke.py:2475`; also delete `g_baked*` atomics at `txmmgr.cpp:1875-1877`; `g_bakedProbeCalls.fetch_add` runs on a hot path, the other two are never incremented | SAFE, plan under-scoped |
| LIGHT_DEDUP_TRACE | B/DEL | 1 (`txmmgr.cpp:2059`) | its own comment says "demote-not-delete" | SAFE, see policy note |
| LODBUG_TRACE | B/DEL | 1 (`bdactor.cpp:2479`) | `tests/shader_variants/static_prop.frag.splice-regression` mentions a "LODBUG probe", which is a shader debug mode, not this env var. Leave it | SAFE |
| MATERIALLIB_TRACE | B/DEL | 1 (`gos_terrain_lod_chunk.cpp:2852`) | manifest regen | SAFE-WITH-CONSUMER-FIX |
| MVP_EARLY_TRACE | B/DEL | 1 (`gos_terrain_indirect.cpp:4743`) | manifest regen | SAFE-WITH-CONSUMER-FIX |
| OBJECT_ID_BRIDGE_TRACE | B/DEL | 2 (`RenderWorld.cpp:619,938`) | pick diagnostic, the pick-parity surface | SAFE, weak-keep |
| PATROL_TRACE | B/DEL | 2 (`EditorObjects.cpp:700,855`) | `run-editor.bat:11-13` sets it (consumer). The reads are ORed with `MC2_EDITOR_TRACE` (kept; `EditorMFC.cpp:177` forces it ON) | SAFE-WITH-CONSUMER-FIX; shape is drop the OR term, `[PATROL]` prints survive |
| SHADOW_CASTER_DIAG | B/DEL | 1 (`gos_mech_batcher.cpp:1268`) | manifest regen; the file's `.h` is dirty (seam#2) | SAFE in principle. Defer until seam#2 lands |
| SHADOW_FOCUS_DIAG | B/DEL | 1 (`gos_postprocess.cpp:6926`) | ORed with registered `MC2_SHADOW_FRUSTUM_DIAG`; the block stays | SAFE-WITH-CONSUMER-FIX; shape is drop one OR term (about 1 line, not 5) |
| STABLE_LIGHT_SKIP_DIAG | B/DEL | 1 (`bdactor.cpp:2866`) | `run_smoke.py:2483`; no yaml row (`ParseEnvBool` idiom is invisible to the generator); about 110 lines of struct and flush | HOLD, see disagreement 3 |
| TERRAIN_CAM_DIAG | B/DEL | 1 (`terrain.cpp:2938`) | none | SAFE |
| TERRAIN_LIGHTING_GPU_TRACE | B/DEL | 1 (`gos_terrain_lighting.cpp:55`) | `run_smoke.py:2081`; macro `TL_TRACE` has 5 uses; `telemetry_lift.py:260` expects 8 `TERRAIN_LIGHTING_GPU` records, but those come from ungated `MC2_DIAGF` lines | SAFE-WITH-CONSUMER-FIX (manifest regen) |
| TERRAIN_MASK_DISPATCH_TRACE | B/DEL | 1 (`gos_terrain_mask_dispatch.cpp:94`) | `s_traceOn`, macro `MASK_TRACE` (4+ uses) and blocks at 288, 317; hit in untracked `docs/superpowers/...b4-mask-dispatch-design.md` | SAFE; plan's "no doc hit" is wrong for that file |
| TERRAIN_SHORELINE_PROBE | B/DEL | 1 (`gos_terrain_lod_chunk.cpp:1538`) | `run_smoke.py:1795`; the function spans 1535-1629 (95 lines, not 25) plus call at 2711 | SAFE-WITH-CONSUMER-FIX; also removes `s_coarseHeightCpu` (420, 1542, 1551, 3621), a heightmap copy kept only for this probe |
| TEX_HANDOFF_TRACE | B/DEL | 1 (`gos_static_prop_batcher.cpp:119`) | memory hit in `archive/black_tree_bug_investigation_state.md`, which violates the plan's own rule; macro `TEX_HANDOFF` has 2 use sites (3408, 8924-8941) | HOLD, should be MERGE by the plan's rule |
| VK_PROP_LIGHT_PROBE | B/DEL | 1 (`vulkan_color_prop_island.cpp:1604`) | documented as the "washout engagement probe" in `VK-ARC-STATE.md:2888,2929` | HOLD until VK prop-lighting parity closes |
| CEMENT_EDGE_CURVE | A/DS | 0 | only comments and docs: `gos_terrain_lod_chunk.cpp:4358,4377`, `.h:283`, `terrain_lod_chunk.frag:307`, registry comment 2733, `tier1_env_vars.md:868` | SAFE, no code; tombstone comments only |
| ROAD_PBR | A/DS | 0 | docs only: `tier1_env_vars.md:542,1323`, `active_campaigns.md:9` | SAFE |
| DRAW_PACKET_STATIC_PROP_V6 | A/NRR | 0 | enum 14 (`RendererFeatureRegistry.h:107`), row 445, `tests/unit/test_rendercore.cpp:405`, `engine-closure-audit.md:154`; memory `dead-gate-scan-classifier-precision` says DO_NOT_DELETE | UNSAFE standalone: positional enum. Fold into the renumber pass or keep the tombstone |
| VK_CSM_BISECT | A/NRR | 1 (`vulkan_terrain_island.cpp:168`, warn only) | allowlist row `check-env-registry.sh:191` | SAFE-WITH-CONSUMER-FIX |
| VK_CSM_COPYMODE | A/NRR | 1 (`vulkan_terrain_island.cpp:164`, warn only) | row `:192` | SAFE-WITH-CONSUMER-FIX |
| VK_TERRAIN_NATIVE_PREFIX_CLEAR | A/NRR | 1 (`vulkan_terrain_island.cpp:223`, warn) | strict runner `:41,88` sets it to 1; `run_smoke.py:2673`; registry row 1972 (kind Feature, text says no-op); `tier1_env_vars.md:126` | SAFE-WITH-CONSUMER-FIX |
| VK_SHADOW_DIRECT_CHAIN | A/NRR | 1 (`vulkan_shadow_island_live.cpp:164`, warn) | `check_vk_nointerop_log.py:340-349` `explicit_dc` branch (the umbrella branch already reconciles it); strict runner `:18-22,54,97`; `run_smoke.py:2591,2858`; registry row 1783 (Trace kind, stale text) | SAFE-WITH-CONSUMER-FIX; rework the checker first |
| VK_MECH_DIRECT_CHAIN | A/NRR | 3 (`engine_present_vk.cpp:299`, `vk_shared_gbuffer_gate.h:157`, `vulkan_mech_island.cpp:141`) | strict runner `:47,90`; `run_smoke.py:2642`; registry row 2135 (text 2151 stale). Delete-OFF-path: only `=0` changes behavior | SAFE-WITH-CONSUMER-FIX + OWNER OK. Header edit forces a wide rebuild |
| VK_POST_FOG / SSAO / SCREENFX / HZB_DIRECT_CHAIN | A/NRR | 2 each (`gos_postprocess.cpp:150-153` gate term + `vulkan_post_*_native.cpp` warn) | strict runner `:59-68,92-95`; `run_smoke.py:2850-2853`; allowlist rows 1051/1054/1057/1061; manifest regen | SAFE-WITH-CONSUMER-FIX + OWNER OK; same `=0` override loss |
| TERRAIN_LIGHTING_V1 | T/DEL | 3 (`gameos_graphics.cpp:7175`, `gos_terrain_lod_chunk.cpp:2872`, `GraphicsOptionsWindow.cpp:613`) | `capture_baseline.py:83`; debt `:65`; `terrain.json:17`; `data/visual_tuning.json` (8 keys, incl. the V2 key); `docs/modding/examples/imgui-editing/mod/data/visual_tuning.json:8`; `tools/shader_reflect` goldens (`gos_terrain.frag`, `terrain_overlay.frag`); `check-lighting-divergence.py` rows; manifest regen | SAFE-WITH-CONSUMER-FIX; shipped copies in `mc2-win64-v0.4` and `v0.4c` carry the JSON keys, so keep the retired-key branch in `visual_tuning_profile.cpp` |
| TERRAIN_LIGHTING_V2 | T/DEL | 3 (`gameos_graphics.cpp:7188`, `gos_terrain_lod_chunk.cpp:2873`, `GraphicsOptionsWindow.cpp:643`) | `capture_baseline.py:85`; debt `:66`; `terrain.json:18`; same files as V1 | SAFE-WITH-CONSUMER-FIX |
| TERRAIN_NORMALS_FROM_HEIGHT | T/DEL | 2 (`gameos_graphics.cpp:7160`, `GraphicsOptionsWindow.cpp:561`) | `capture_baseline.py:79`; debt `:68`; `terrain.json:16,25`; goldens. Effect is legacy `MC2_TERRAIN_LOD_CHUNK=0` path only | SAFE-WITH-CONSUMER-FIX |
| TERRAIN_CLIFF_DISPLACE (+MAXTESS) | T/DEL | 2 (`gos_terrain_lod_chunk.cpp:184,188`) | `run_smoke.py:2926-2927`; registry aux rows 2789, 2796; **`gl_shader_variants.json` tess entries (missed by plan)**; delete `terrain_lod_chunk.tesc` and `.tese` | SAFE-WITH-CONSUMER-FIX; confirm with owner that tess is dead (memory: 3b abandoned) |
| TERRAIN_AMBIENT_FLOOR_NIGHT | T/DEL | 1 (`gos_terrain_lod_chunk.cpp:4277`) | `run_smoke.py:1772`; registry aux row 940; `tier1_env_vars.md:790` | UNSAFE pending owner, see disagreement 1 |
| TERRAIN_CONCRETE_DETAIL_NORMAL | T/DEL | 1 (`gos_terrain_lod_chunk.cpp:927`) | `run_smoke.py:1813`; registry aux row 2815; `tier1_env_vars.md:867` | SAFE-WITH-CONSUMER-FIX; user ruling 2026-07-26 backs it |
| PROP_LOD_SHADOW_CAST | T/DEL | 1 (`mclib/bdactor.cpp:36`) | `run_smoke.py:1872`; enum 47 (`:235`) and row 708 force renumber; `.claude/SHADOW-DECISIONS-1-STATUS.md` says "benefit undemonstrated" | SAFE-WITH-CONSUMER-FIX; making the default `setRecipeNoShadow(true)` unconditional leaves shadow behavior unchanged |

## Verified plan claims
- `terrain-gates-plan.md` line numbers still match HEAD: enum rows 137, 140, 143 and 235; `gos_terrain_lod_chunk.cpp` 184, 927, 2872 and 4277; `gameos_graphics.cpp` 7160, 7175 and 7188. Anchor edits by text anyway: commit `c47bc6892` touched comments in `gos_terrain_lod_chunk.cpp`.
- The V1 corner case holds. The chunk reader is `getenv != nullptr`, so `=0` meant ON. The legacy reader tests `e[0] != '0'`.
- `gos_TerrainAmbientDayGate` is clamped to 0..1 (`gameos_graphics.cpp:10589`). Making `max(raw, 0)` into `raw` is byte-identical for the VK feed. This applies if NIGHT is ever deleted.
- The enum renumber (61 to 57) is safe in-repo. Only `RendererFeature::` names are used (`EditorInspector.cpp:356` loops by count), and no script parses the enum.
- There is no data, launcher, `.bat` or capture-profile consumer, apart from `run-editor.bat` (PATROL), `terrain.json`, and `capture_baseline.py`.
- `tests/visual/baselines/*.json` hold historical env records. `check-capture-env-declared.py` only checks the scrub record of new manifests, so leave them.

## Plan verdicts I disagree with
1. **`MC2_TERRAIN_AMBIENT_FLOOR_NIGHT`: do not delete yet.** Its registry text says it "must stay OFF until the user grades it". It is the only lever that reaches mc2_05, mc2_10, mc2_19, mc2_21 and mc2_23, where the shipped ambient floor is zero. `vk-default-flip-checklist.md` item 8 is an open night-gap ship-or-accept decision. Deleting it pre-empts that call. Ask the owner.
2. **`MC2_TEX_HANDOFF_TRACE` (and `MC2_TERRAIN_MASK_DISPATCH_TRACE`) break tierB's own rule.** The rule says DELETE requires no memory or `.md` hit. TEX_HANDOFF has a memory hit; MASK_DISPATCH has a hit in an untracked spec. Treat them as MERGE or get explicit sign-off.
3. **`MC2_STABLE_LIGHT_SKIP_DIAG`: bundle, don't delete alone.** It is the only observability for the live default-off family `MC2_STABLE_LIGHT_SKIP`, `_TOUCH` and `MC2_LIGHTBRIDGE_STABLE_SKIP`. Decide the family first (gate amnesty).
4. **`MC2_VK_PROP_LIGHT_PROBE`: defer.** Active VK lit-prop parity probe, and the VK flip is not done.
5. **Policy tension.** `memory/debug_instrumentation_rule.md` says to demote diagnostics, never delete them (`txmmgr.cpp:2057` repeats it). The 28-gate delete contradicts it unless the owner has superseded the rule. Get that acknowledged in one line before B1/B2.
6. **`DRAW_PACKET_STATIC_PROP_V6` is not a clean delete.** It is a positional enum tombstone; memory says DO_NOT_DELETE. Do it only inside the renumber pass.
7. **Under-scoped:** LIGHTBRIDGE_COMMIT_TRACE (the `txmmgr.cpp` atomics), TERRAIN_SHORELINE_PROBE (the heightmap copy, 95 lines), PATROL_TRACE and SHADOW_FOCUS_DIAG (OR terms, not blocks). Tier B's "39 sites" is mentions; the actual count is 33 env-read lines.

## VK/GL parity surfaces
- **VK-retired gates (MECH plus the four POST).** `=0` is a live debug override today that disarms `postDirectChainUpstreamOn()` and the composite gate. Other OFF entry points remain: `MC2_VK_PROP/WATER/POST_COMPOSITE_DIRECT_CHAIN=0` and the umbrella being off. Default and umbrella configs are unchanged, and the strict runner always sets the umbrella. This is still a VK-flip-checklist surface (items 3, 4, 10), so require VK A/B day and night viewed by a human.
- **Terrain shader edits are GL-only; VK parity is unchanged.** The VK terrain shaders never had V1 or NFH-gate (see `terrain_overlay_tier1.frag:13`). Keep the chunk-path NFH strength and its VK feed (`nfhStrength`). `terrain_lod_chunk.vert` lines 387-391 sit right next to `//PORT-BEGIN:skirtVertMath` (376-378); keep the markers byte-identical and run `glsl_port_diff.py --all`.
- **The two pinned gaps in `tools/glsl_port_diff.allowlist`** (mech `MC2_SUN_AXIS_FIX`, water shore meander) are untouched by any gate here.
- **`OBJECT_ID_BRIDGE_TRACE`, `HDRI_SKY_STATE_PROBE` and `SHADOW_CASTER_DIAG`** are print-only but sit in parity-relevant code.

## Ordered batches
Edit order inside a batch: reader, then registry row, then consumers. Every batch: delete its allowlist rows, regen yaml, and run `check-contracts`.

| Batch | Gates | Env reads removed | Files | Consumer edits | Verify |
|---|---|---|---|---|---|
| **B0** (no build) | CEMENT_EDGE_CURVE, ROAD_PBR comment/doc scrub | 0 | `tier1_env_vars.md:542,1323,868`, the comments | none | `check-env-registry` |
| **B1** engine core (mclib/code, no GameOS) | ANIMATED_PROP, ANIM_ADVANCE, LODBUG, LIGHT_DEDUP, TERRAIN_CAM, BRAIN_ENGAGE, DEPLOY_KIND, FL, FRAME_JOBS_PATHB, LIGHTBRIDGE_COMMIT (10) | 11 | `bdactor.cpp`, `mech3d.cpp`, `txmmgr.cpp` (plus atomics), `terrain.cpp`, `brain_special_dispatch.cpp`, `mission.cpp`, `missiongui.cpp`, `objmgr.cpp` | 10 allowlist rows; `run_smoke.py:2472,2475`; yaml. No manifest regen | build mc2 (force the mclib recompile), `smoke --lite` (hot loop), tier1 5/5 |
| **B2** GameOS diag strip | CLOUD, HDRI_SKY_STATE_PROBE, SHADOW_FOCUS (OR term), GPU_SCENE_SCALE, MVP_EARLY, TERRAIN_LIGHTING_GPU, MASK_DISPATCH, MATERIALLIB, SHORELINE_PROBE (plus `s_coarseHeightCpu`), OBJECT_ID_BRIDGE (10) | 13 | `gos_postprocess.cpp`, `gos_static_prop_registry.cpp`, `gos_terrain_indirect.cpp`, `gos_terrain_lighting.cpp`, `gos_terrain_mask_dispatch.cpp`, `gos_terrain_lod_chunk.cpp`, `RenderWorld.cpp` | 10 allowlist rows; `run_smoke.py:2081,1795`; yaml; **manifest regen** | GL and VK day+night `ab.py`, looked at by a human; `shader_variants` |
| **B3** editor target | EDITOR_DRAG, MOVER, PROJECT, PATROL (4) | 5 | `EditorInterface.cpp`, `EditorObjectMgr.cpp`, `FoliageRender.cpp`, `EditorObjects.cpp` | `run-editor.bat:11-13`; 4 allowlist rows | editor build, `run_editor_playtest_smoke.py` |
| **B4** VK warn-only | CSM_BISECT, CSM_COPYMODE, NATIVE_PREFIX_CLEAR, SHADOW_DIRECT_CHAIN (4) | 4 | `vulkan_terrain_island.cpp`, `vulkan_shadow_island_live.cpp`, registry rows 1783 and 1972 | rework `check_vk_nointerop_log.py:340-349`; strict runner `18-22,41,54,88,97`; `run_smoke.py:2591,2673,2858`; allowlist rows 191-192; `tier1_env_vars.md:126` | strict-runner dry run, checker on a saved good log, VK smoke |
| **B5** (owner OK) VK `=0` overrides | MECH + FOG, SSAO, SCREENFX, HZB (5) | 11 | `engine_present_vk.cpp`, `vk_shared_gbuffer_gate.h` (header, wide rebuild), `gos_postprocess.cpp`, `vulkan_mech_island.cpp`, 4 `vulkan_post_*_native.cpp`, registry row 2135 | strict runner `47,59-68,90-95`; `run_smoke.py:2642,2850-2853`; allowlist rows 1051-1061; manifest regen | VK A/B per flip checklist, strict `--minimal-promotion` |
| **B6** terrain plan, 2 or 3 commits | V1, V2, NFH, CLIFF_DISPLACE (+MAXTESS), CONCRETE, PROP_LOD (7 gates) | 12 | the plan's file list; one registry pass (enum 61 to 57); `gl_shader_variants.json` regen; shader_reflect `--update` | debt lines 65, 66, 68; `capture_baseline.py:79-85`; `terrain.json` (drop `terrain_nfh_v1_v2`, edit `terrain_debug_mode_10`); `data/visual_tuning.json` (8) and the modding example (1) | plan step 14, plus `glsl_port_diff --all` and a debug-mode-10 smoke |
| **Deferred** | SHADOW_CASTER_DIAG (seam#2), TEX_HANDOFF, VK_PROP_LIGHT_PROBE, STABLE_LIGHT_SKIP_DIAG, AMBIENT_FLOOR_NIGHT, DRAW_PACKET_V6 | 5 reads | n/a | n/a | owner decisions |

Total: 56 env reads removable now (11 + 13 + 5 + 4 + 11 + 12) out of 1504 in the tree. 5 more are deferred, for 61.

**Questions for the owner**
1. Is the "demote, never delete" instrumentation rule superseded for these 28 gates?
2. Is NIGHT floor to be deleted or left for night-parity grading?
3. Are the `=0` overrides on the MECH and four POST direct-chain gates expendable (B5)?
4. Is HW tess for cliffs permanently dead (B6)?
5. Can the renumber of `RendererFeature` (61 to 57) proceed, or should the rows become `MC2_FEATURE_REMOVED_<n>` tombstones?

## Owner rulings (2026-10-09)

1. "Demote, never delete" instrumentation rule is retired. Tier B trace gates with DELETE verdicts are deleted → B1, B2, B3 GO.
2. `MC2_TERRAIN_AMBIENT_FLOOR_NIGHT`: KEEP (night-parity grading still open). Stays deferred.
3. VK direct-chain `=0` debug overrides (MECH + FOG/SSAO/SCREENFX/HZB) are expendable → B5 GO.
4. Cliff hardware tessellation (`MC2_TERRAIN_CLIFF_DISPLACE`, `_MAXTESS`, `terrain_lod_chunk.tesc/.tese`): KEEP for possible later use → removed from B6. B6 = V1, V2, NFH, CONCRETE_DETAIL_NORMAL, PROP_LOD_SHADOW_CAST.
5. `RendererFeature` enum: RENUMBER (no tombstone rows); `DRAW_PACKET_STATIC_PROP_V6` tombstone goes in the same renumber pass.

Execution order (orchestrator): B0 → B1 → B2 → B3 → B4 → B5 → B6, one builder agent, one commit per batch, check-contracts + smoke-lite per batch, GL+VK day/night A/B viewed by orchestrator for B2/B5/B6. Still deferred: TEX_HANDOFF_TRACE and TERRAIN_MASK_DISPATCH_TRACE (now DELETE under ruling 1 — fold into B2), VK_PROP_LIGHT_PROBE (until VK prop-lighting parity closes), STABLE_LIGHT_SKIP_DIAG (decide the STABLE_LIGHT_SKIP family first).

Update 2026-10-09: SHADOW_CASTER_DIAG was deleted in B2 (the seam#2 header was clean), not deferred.
