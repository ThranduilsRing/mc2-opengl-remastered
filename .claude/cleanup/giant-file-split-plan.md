# Giant-file split plan (read-only survey; nothing built or edited)

Method: top-level function/banner outline via grep; line counts are approximate (`~`) where a region was sized from boundaries, not read line by line. Re-verify boundaries before cutting. Line numbers are current as of 2026-10-08.

Precedent already in tree: `gameos_graphics_internal.h` (473 lines) plus `gameos_graphics_{font,texture,light_ssbo,params,debug}.cpp`. They reach `gosRenderer` only through shim functions. The "GAMEOS-GRAPHICS-SPLIT-1" rules apply to everything below: class bodies move VERBATIM to a header, because TU-internal classes mean any drift is an ODR bug.

Priority by agent-cost saving per unit of risk:
1. `vulkan_backend_skeleton.cpp` (whole file is probes; mechanical)
2. `gos_static_prop_batcher.cpp` (`flush()` alone is 2.4k lines)
3. `gos_postprocess.cpp`
4. `gameos_graphics.cpp` (needs the `gosRenderer` class moved to a header first)
5. `txmmgr.cpp`

---

## Split order proposal (added 2026-10-09; ranks the files in sections 6-17, sections 1-5 keep their priority above)

Ranking rule: payoff = lines taken out of agent context times how often agents open the file; risk = shared-state coupling plus name-keyed scripts plus churn from lane 1 (gate deletion B0-B6, then G0/G1a typed-gate rewrite of every `getenv`) and lane 2 (C4267 narrowing in `code/`, `mclib/`). A split goes AFTER the gate batch that touches its file, because a gate deletion or G1 reader rewrite inside a freshly moved range is a pure conflict. Gate-free files go first.

| # | File | Why here | Blocked by |
|---|---|---|---|
| 1 | `code/ablmc2.cpp` (8497) | 284 stateless `exec*` functions, 8 getenv, 0 Tracy, 0 file statics shared; 1.4k lines of rule/blank comment scaffolding to drop on the way. Only hazard is registration order, solved by leaving the table in place. | nothing |
| 2 | `code/mover.cpp` (7817) | 3 getenv, no file statics; one 1.0-1.3k net-chunk block and about 720 lines of debug code leave cleanly; one script pin (isOnGui write count). | lane 2, only if it reaches `code/` |
| 3 | `code/mech.cpp` (9796) | Class-method file over extern tables, 5 getenv; 170 dead `#if 0` lines first; cost is updating 4 contract scripts that name the file. | lane 2, only if it reaches `code/` |
| 4 | `mclib/move.cpp` (7050) | 0 getenv, 0 stale refs; pathfinding is hot for AI work; shared tables are 3 small arrays. Needs a path-parity check. | lane 2, only if it reaches `mclib/` |
| 5 | `GameOS/gameos/vulkan_backend_skeleton.cpp` (7640) | Delete (D7) beats split; either way carve `vulkan_spv_loader.cpp` out first because `mc2_vulkan_load_spv` is production API. | owner decision on D7 |
| 6 | `editor/EditorInterface.cpp` (7370) | Self-contained ImGui block and little shared state, but editor-only payoff and a separate target to build. | B3 (`EDITOR_DRAG_TRACE`) |
| 7 | `code/missiongui.cpp` (9979) | The 1.8k-line UI-command channel is gate-free and carves out first; the `update*` family waits. | B1 (`FL_TRACE`) for the rest; G1 (25 getenv) |
| 8 | `code/warrior.cpp` (9578) | Orders/core/move/draw bulk (about 3.5k lines) is gate-free and goes early; the brain-gate cluster (22 gates) waits. | G1 for `warrior_brain.cpp` |
| 9 | `mclib/mech3d.cpp` (6877) | `updateGeometry` carves cleanly, but the shared accessors and 21 getenv are G1 territory and the file is hot. | B1, G1 |
| 10 | `mclib/bdactor.cpp` (8317) | Tree block is the biggest cohesive piece, but it needs `bdactor_internal.h` and the diag layering first; 51 getenv, `STABLE_LIGHT_SKIP_DIAG` on hold. | B1, B6, G1 |
| 11 | `GameOS/gameos/gos_postprocess.cpp` (7724) | Highest payoff (hot render file) but 105 getenv and 32 VK-island blocks; the gate wall cut from section 3 is what G1 replaces. Shadow family can go once B2 lands. | B2, B4/B5, G1 |
| 12 | `GameOS/gameos/gos_terrain_indirect.cpp` (6330) | 85 statics in anonymous namespaces need an `IndirectState` design before anything but mine/decal can move. | B2, G1, state-header design |

Of sections 1-5: `gos_static_prop_batcher.cpp` and `gameos_graphics.cpp` are the most gate-dense of the five, so they sit with 9-12 behind G1 despite their size; `txmmgr.cpp` is touched by B1 (atomics), so split it right after B1.

Every split commit needs the two regenerations the gate-deletion audit lists as common steps: `scripts/flag-registry-auto-gen.py` (`docs/flags.auto.yaml` records the reader FILE of each gate, so moving a `getenv` changes it and `flag_registry_drift --strict` fails) and `scripts/extract_gl_shader_variants.py` (each `gl_shader_variants.json` entry stores `file:line` of a `makeProgram` call: 20 entries in `gos_postprocess.cpp`, 1 in `gos_terrain_indirect.cpp`, 153 in `gameos_graphics.cpp`). Run `tools/file_size_ratchet.py --check --tighten` after each split, and re-run `check-contracts` (the `dup_lines_ratchet` baseline is count-based and does not move when code is only relocated).

Files where a clean split is NOT possible without design work:
- `gos_terrain_indirect.cpp`: about 85 file statics in three anonymous namespaces mirror GPU state and are read by every phase; needs a shared state struct (design), and `ComputeDispatch` (963 lines) stays whole. Only the mine/decal block is a pure move.
- `missiongui.cpp`: `processUiCommandChannel` is one 1.8k-line function with a shared `ack` loop; it cannot move as a file without breaking the 1500-line ratchet, so it needs the `UiCmdCtx` handler-group refactor (small, but not a cut-and-paste).
- `bdactor.cpp`: Bldg and Tree share about 25 diag counters and four anonymous-namespace diag structs; needs `bdactor_internal.h` plus an owner decision on the held `STABLE_LIGHT_SKIP_DIAG` block before the diag layer can move.
- `gos_postprocess.cpp`: even after the section 3 cuts the core stays near 2900 lines (`init` 465, `endScene` 486, FBO lifetime); it needs the extra `_fbo`/`_effects` cuts in section 12, and `init` itself is a 32-gate reader that G1 rewrites.
All other files in sections 6-17 are mechanical once their `*_internal.h` exists.

---

## 1. GameOS/gameos/vulkan_backend_skeleton.cpp (7640 lines)

The whole file is an env-gated (`MC2_VULKAN_PROBE`) one-shot self-test suite. It is not on the render hot path. Callers of `mc2_vulkan_probe*`: `gos_render.cpp`, `vulkan_present_object.cpp`, `vulkan_swapchain_{present,probe}.cpp`, `tools/vulkan_shader_probe_main.cpp`.

### Outline
| Lines | Responsibility |
|---|---|
| 1-135 | Helpers in an anon namespace: `log`, `ensure_volk_initialized`, validation preset |
| 136-274 | SPV loader: `mc2_vulkan_load_spv`, `free_shader`, `probe_shaders` |
| 275-841 | `find_mem_type`, pipeline cache file, VMA create, `probe_triangle` (~480) |
| 842-1167 | `probe_descriptors` (~325) |
| 1168-1596 | `probe_sampled_image` (~430) |
| 1597-1949 | CPU reference functions: `edgefog_cpu_ref`, `fogoob_cpu_ref`, `ssao_cpu_ref`, half/smoothstep helpers |
| 1950-2650 | `probe_edgefog_fixture` (~700) |
| 2651-3385 | `probe_oobfog_fixture` (~735) |
| 3386-4124 | `probe_ssao_fixture` (~740) |
| 4125-4862 | `probe_hzb_fixture` (~740) |
| 4863-5497 | `probe_comparesampler_fixture` (~635) |
| 5498-6156 | `probe_csmarray_fixture` (~660) |
| 6157-6761 | `probe_shadowdepth_fixture` (~605) |
| 6762-7470 | `probe_watermvp_fixture` (~710) |
| 7471-7640 | `mc2_vulkan_probe` and `mc2_vulkan_probe_if_env` drivers (~170) |

### Dead / diagnostic regions
Essentially 100% diagnostic (about 7500 lines). Nothing here executes unless `MC2_VULKAN_PROBE` is set. This is also not "dead" in the `#if 0` sense, since `tools/vulkan_shader_probe_main` uses it.

### Split proposal (5 files)
1. `vulkan_probe_common.{h,cpp}` (~450 lines): `log`, volk init, validation preset, `find_mem_type`, pipeline-cache I/O, `create_vma_allocator`, spv loader, `device_type_str`. These are anon-namespace statics today, so they become `namespace mc2vk_probe` with declarations in the header. They are about 12 functions and the only real coupling.
2. `vulkan_probe_basic.cpp` (~1250 lines): shaders, triangle, descriptors, sampled_image.
3. `vulkan_probe_fog_ssao.cpp` (~2900 lines): CPU refs plus edgefog, oobfog, ssao fixtures. The CPU-ref helpers stay file-local here.
4. `vulkan_probe_shadow_hzb.cpp` (~2650 lines): hzb, comparesampler, csmarray, shadowdepth, watermvp. Further split into hzb and shadow if wanted.
5. `vulkan_backend_skeleton.cpp` (~200 lines): the `mc2_vulkan_probe*` drivers only.

Churn: pure cut and paste. The only edits are namespace qualification of about 12 helpers, plus CMake source-list changes in the 2 CMake files (`CMakeLists.txt:146`, `GameOS/gameos/CMakeLists.txt:104`). Risk is very low and the public header is unchanged. Do NOT name these `_diag.cpp`; the whole thing is already diag, so just keep it out of the agent's way. The agent-cost win is large: an agent touching the real Vulkan backend never needs to open 7k lines.

---

## 2. GameOS/gameos/gos_static_prop_batcher.cpp (11207 lines)

### Outline
| Lines | Responsibility |
|---|---|
| 1-140 | Includes, Fix-B MVP accessor |
| 141-420 | SPFLUSH cost accumulators, legacy-pool gate, `pipelineId_to_group`, `[GPUPROPS v1]` and `[TREE_DIAG]` macros and setup attribution |
| 420-795 | Misc gates and file-scope state, `gos_lightBillboardFacingMode`, setup instrumentation |
| 795-1700 | Coalesce file-scope state: SSBO handles, MaterialGpu gates, emissive fixture, IBL SH constants and CPU SH eval (1154-1400), geometry/material statics |
| 1700-1980 | Global instance pool, split-armed, permutation SSBO, KTX/ORM path derivation, release/rollback helpers |
| 1979-2385 | `loadProgramsIfNeeded` (shader program build, ~410) |
| 2385-2570 | `IsCoalesceEnabled`, `ensureRingCapacity`, light-freshness gate |
| 2569-2918 | Telemetry: `SPOTLIGHT_REAL_TRACE`, `SUBMIT_TYPEHIST`, `TREE_DIAG` pop names, `emitFinalSummaryAtExit`, `accumulateMonotonicAndMaybeEmit`, late-register allowlist |
| 2918-3200 | Debug-view mode mapping, `instance()`, `onMapLoad`/`setMissionForIbl`/`onMapUnload` |
| 3198-3418 | `registerType`, `registerMultiShape` |
| 3418-5470 | `finalizeGeometry` (~2050 lines: VBO/IBO build, atlas/texture-array build, KTX, ORM, packet and material tables) |
| 5470-6234 | `submit` (~300), `submitMultiShape` (~450) |
| 6234-6741 | `uploadAllBucketsIfNeeded`, `buildLiveV6Arrays`, depth prepass V6, poly-offset gates, executor state |
| 6741-9120 | `flush()` (~2380 lines: coalesce write loop, indirect/MDI, draws, INSTDIAG, SEAMPROBE) |
| 9120-9470 | Shadow-prop alpha helpers, `flushShadow` |
| 9470-9830 | `drawStaticBuildingShadows`, `drawDynamicPropShadows` |
| 9829-10092 | Vulkan island geometry feed `mc2_shadow_island_get_geometry_feed` (~265) |
| 10092-10390 | Late-reg, probes, `submitCachedInstance*`, persistent static |
| 10391-10990 | `batcher_get*` RenderCore seam accessors (~600, pure read-only, near-zero coupling) |
| 10994-11207 | `batcher_buildCandidateLog`, base-instance binding, `prepareBaseInstanceTable`, `mc2PeekStaticPropTypeInfo` |

### Diagnostic-only regions (candidates for `gos_static_prop_batcher_diag.cpp`)
- 2569-2918: SPOTLIGHT_REAL_TRACE, SUBMIT_TYPEHIST, TREE_DIAG pop tables, exit summaries (~350)
- 141-175 and 744-795: SPFLUSH cost and GPUPROPS setup instrumentation (~90)
- 10840-10950: `batcher_compareSnapshotPackets`, snap-cull and snapshot stats (~110)
- 10994-11052: `batcher_buildCandidateLog` (~60)
- Inline blocks inside hot functions, each env-gated:
  - `[SEAMPROBE]` stages 4/8/10: lines 3357, 4368-4410, 8755-8790 (~100 total)
  - `[STATICPROP_ORM_PROBE]`: ~4203 (~20)
  - `[PARITY_DIAG v1]`: 5713-5740 (~30)
  - `[INSTDIAG]`: 7203-7220 and 8793-8830 (~60)
  - `[MVP_DIAG]`: 7339-7360 (~20)
  - `[SHADOW_DIAG]`: ~9378 and 9436 (~40)
  - `[GPUPROPS_HIST v2]` "temporary, REMOVE once": 10033-10060 (~30)
- Total about 900 lines. Inline blocks can be reduced to a one-line call such as `batcher_diag::seamProbeDrawCensus(...)`.

No `#if 0` found.

### Split proposal (6 files)
1. `gos_static_prop_batcher_state.h` (new, internal): move the file-scope statics (SSBO handles, `s_packets`, `s_sharedVbo`/`Ibo`, coalesce flags, `s_materialGpu*`, pop counters) as `extern` or inline-in-namespace declarations. This is the main cost: about 80-120 statics, one `batcher_internal` namespace. It is needed by every file below, and `GpuStaticPropBatcher` members already touch most of them.
2. `gos_static_prop_batcher.cpp` (~2900): state definitions, programs, map load/unload, register*, IBL SH, pool helpers.
3. `gos_static_prop_batcher_finalize.cpp` (~2100): `finalizeGeometry` only.
4. `gos_static_prop_batcher_submit.cpp` (~800): `submit`, `submitMultiShape`, cached/persistent static (10172-10355).
5. `gos_static_prop_batcher_flush.cpp` (~3000): 6234-9120, the main draw path, minus diag.
6. `gos_static_prop_batcher_shadow.cpp` (~900 lines) and `..._seam.cpp` (~900 lines): 9120-9830, plus the 10092-10990 and 10994-11207 accessors. These accessors can go in a 7th file if wanted.
7. `gos_static_prop_batcher_diag.cpp` (~600 after extraction).

Estimated churn: low-to-medium. Moving code is mechanical, but 100+ `static` to `extern` conversions. The file-scope `static const bool s_*Enabled = []{getenv}()` lambdas must become `inline` or accessor functions to avoid duplicate definitions (use `inline const bool` in the header or keep them in the `.cpp` that owns them and expose a function). `IsCoalesceEnabled` already has a forward decl at line 804 and is used across the file.

Start with: seam accessors (10391-10990) and `flushShadow`/`drawStaticBuildingShadows` (9120-9830). They have the fewest shared statics. Then `finalizeGeometry`. Leave `flush()` unsplit until its 2.4k lines are internally split into helpers. It is a single function, so a file split does not shrink what an agent must read.

---

## 3. GameOS/gameos/gos_postprocess.cpp (7725 lines)

### Outline
| Lines | Responsibility |
|---|---|
| 1-607 | Includes, VK shared-scene gates, shadow knob getters (`mc2Shadow*`, ~45 small getters 235-500), `setSceneDrawBuffers` |
| 607-726 | GL-lost-draws tracker (`lostBegin/End/Emit`, `gos_GlLostDrawsPush/Pop`) (~110) |
| 726-931 | `getGosPostProcess`, setters, `setSkyNumber` |
| 931-1396 | `gosPostProcess::init` (~465) |
| 1396-2300 | `destroy`, `resize`, `createFBOs` (1523-2031, ~500), `copySceneDepthForParticles`/`copySceneColorForVfx`, `destroyFBOs` |
| 2300-2560 | Fullscreen quad, box decals, `beginScene` |
| 2559-3022 | `runHzbReduce` (~90) and `runHzbProbe` (~375) |
| 3022-3560 | `runSSAO`, region GL/Vulkan variants, `runScreenShadow`, `runCloudShadow`, `runShoreline` |
| 3562-4604 | Edge fog, fog OOB, fog / SSAO / HZB / composite-present "region through backend" adapters (4031-4600, repetitive) |
| 4606-5170 | `endScene` (~490), `drawShadowDebugOverlay` |
| 5170-5959 | Skybox: `renderSkybox`, `renderHdriSkybox`, Basis, InvVP (~790) |
| 5959-6350 | Static shadows: `initShadows`, pass begin/end, light basis, `buildStaticLightMatrix` |
| 6350-7207 | Dynamic shadows: init, CSM cascades, `buildDynamicLightMatrix` (~500) |
| 7207-7725 | Executor ownership accessors and wrappers, `executorApply*State`, `extern "C"` counters |

### Diagnostic-only regions
- `runHzbProbe` 2646-3022 (~375, `MC2_HZB_PROBE`, plus the `HZB_PROBE` logging at 1321): the cleanest `_diag` candidate.
- GL-lost-draws tracker 607-715 (~110).
- Screen-shadow GL canary ~715-725 and `RES_DIAG` 2540-2558 (~30).
- `CLOUD_DIAG` 3435-3500 (~25 inline).
- `SHADOW_FRUSTUM_DIAG` 6993-7010 (~30 inline).
- Total about 570 lines (7%).

### Split proposal (5 files)
1. `gos_postprocess_gates.cpp` (~700): lines 57-606, env-knob getters. These are already free functions (non-static), so there is no coupling and no churn beyond moving.
2. `gos_postprocess_shadow.cpp` (~1900): 5959-7207 (`initShadows` to `buildDynamicLightMatrix`) plus `mc2ComputeLightBasis`/`ClampShadowSunAltitude`. These are `gosPostProcess` members using the shadow FBO fields only, so no changes are needed because the class is already in `gos_postprocess.h`. The forward-declared static `mc2ClampShadowSunAltitude` (line 483) must become non-static (or move together with its users).
3. `gos_postprocess_skybox.cpp` (~800): 5170-5959.
4. `gos_postprocess_region.cpp` (~900): 3150-3230 and 3787-4604, the region/backend adapters. The `*_set_fallback_reason` statics may be shared with `endScene`. Check usage.
5. `gos_postprocess_diag.cpp` (~500): `runHzbProbe`, lost-draws, canary.
6. `gos_postprocess.cpp` (~3000): init, FBOs, effects, `endScene`, executor wrappers (the `executorOwn*` file-statics are used by `endScene`, so keep them there).

Churn: low. `gosPostProcess` is already declared in a header, so the member-function split costs almost nothing. The shared VK gate statics (84-175) used across files should go into `gos_postprocess_gates.cpp` as non-static, declared in `gos_postprocess.h` or a small internal header. Already has the sibling `gos_postprocess_blur.cpp` as precedent.

---

## 4. GameOS/gameos/gameos_graphics.cpp (11814 lines)

### Outline
| Lines | Responsibility |
|---|---|
| 1-395 | Includes; env gates, GL-canary query helpers (130-160), shadow/water bias gates (207-360), `getGosRenderer` |
| 395-790 | Class definitions: `gosVertexDeclaration`, `gosMaterialVariation[Helper]`, `gosRenderMaterial` (~400) |
| 790-1200 | HUD ring gates, `gosMesh::draw`/`drawIndexed` (~400) |
| 1200-1320 | Overlay texture fallback and trace (`overlayTexTrace*`) (~110) |
| 1320-2700 | `HudDrawCall`, `class gosRenderer` body (~1365 lines of declaration and state) |
| 2700-3075 | Program-id accessors and the water setter/getter wall (2856-2990, ~35 trivial pairs) |
| 3075-3970 | `renderWaterFastPath` (~900, includes a 280-line MC2_VULKAN_ISLAND block) |
| 3970-5170 | Terrain bridges: surface draw, `drawIndirect` (~400), mask solid/water (~380), mine static, decal static |
| 5173-6290 | `gosRenderer::init`/`destroy`/render-state push/pop/`applyRenderStates` (~330)/`beginFrame`/`endFrame`/`handleEvents` |
| 6290-6575 | `drawQuads`/`Lines`/`Points`/`Tris` |
| 6575-7300 | Shadow-pass trace helpers (6574-6733, ~160) plus pre/dynamic shadow passes, uniform helpers |
| 7300-7850 | Terrain uniform binding (`terrainBindUniformsForPatchStream`, thin), `drawIndexedTris` |
| 7850-8730 | Text: `findTextBreak`, `drawText` (~175), `replayTextQuads`, `flushHUDBatch` (~290, plus VK HUD queue) |
| 8734-9600 | `gos_*` C-API wrappers: textures, fonts, render states, screen mode, text draw |
| 9600-10060 | Buffers, vertex decl, materials, `gos_SetupObjectShadows` (~150), force-apply states |
| 10060-10290 | Camera preview and terrain tess/phong setters |
| 10285-10440 | `MVP_DIAG` counters, `gos_SetWorldToClipGL`/`SetTerrainMVP`, static light matrix |
| 10440-10830 | Shadow, light-dir, night, terrain material/tint/AO/roughness setters and getters (~400 of trivial one-liners) |
| 10830-11720 | Overlay/decal: `pushToOverlayBatch_`, `uploadOverlayUniforms_` (~235), `drawTerrainOverlays`, `drawDecalStaticBatch`, `drawDecals`, executor state |
| 11720-11814 | Overlay C-API, GL-texture-id accessors, shader clock |

### Diagnostic-only regions
- Overlay tex trace 1255-1316 (~75)
- Shadow-pass GL state trace 6574-6733 (`shadowStateTraceEnabled`, `traceShadowPass`, `captureShadowGLState`, ~160)
- MVP_DIAG counters 10285-10360 (~75; keep the `note_*` functions inlined or in the header if the hot path calls them)
- GL canary 130-160 (~30), `mc2AuditDiagSink` 8734-8753 (~20), `gosHudRingResidency` (~30)
- Lighting debug mode 7274-7299 (~25)
- 184 lines contain trace/diag/getenv keywords.
- Only one `#if 0` (line 5996, ~10 lines, already-dead GL fixed-function comment).
- Total about 450 lines is cleanly extractable.

### Split proposal (6 files)
Prerequisite (the real cost): move `class gosRenderer` (1320-2700, ~1365 lines) and the `gosRenderMaterial` family (395-790) VERBATIM into `gameos_graphics_renderer.h`, as the existing internal.h rules require. Every file below includes it, and it is the one genuinely expensive step (plus the 9 file-scope statics used by the class). If this is not done, the member-function split is not possible, and only the trivial C-API wall can be split.

1. `gameos_graphics_terrain_api.cpp` (~1000): terrain/lighting/water/night setters and getters (2856-2990, 10200-10830) plus bridge accessors (2700-2850). All of it is trivially extern, with state held in a few statics (`s_water*`, `s_terrain*`). These statics must be made `extern` or shared via a tiny `gameos_graphics_terrain_state.h`. Almost no logic. Biggest line count per unit of risk.
2. `gameos_graphics_terrain_draw.cpp` (~2300): `renderWaterFastPath`, surface, indirect, mask, mine, decal bridges (3075-5170), terrain uniform binding (7300-7700). Statics to share: `isAllConcreteTerrainBatch`, `terrainNormalArrayEnabled`, `maskUploadStatsEnabled`, bias gates, plus `g_*` water uniform cache.
3. `gameos_graphics_overlay.cpp` (~900): 10830-11720 plus overlay trace/fallback (1200-1316).
4. `gameos_graphics_shadow.cpp` (~900): 6574-7300 plus `gos_SetupObjectShadows` and the shadow wrappers (10440-10520).
5. `gameos_graphics_hud_text.cpp` (~1150): 7850-8730 (text, `flushHUDBatch`, VK HUD queue) plus the text C-API (9355-9580).
6. `gameos_graphics_diag.cpp` (~300): the diagnostic regions above.
7. Main `gameos_graphics.cpp` (~4000): gosMesh, renderer init/frame/draw*, C-API wrappers.

Estimated churn: high relative to the others. It is mostly blocked on the `gosRenderer` header move (one large verbatim cut, ~1400 lines) and on exposing about 20-30 file statics via extern or a state header. Do the terrain_api (pure accessors) and diag splits first, since both can use the existing shim-function pattern and do not need the class header. Each is about a 1000/300 line saving.

---

## 5. mclib/txmmgr.cpp (6064 lines)

### Outline
| Lines | Responsibility |
|---|---|
| 1-295 | Includes, `mc_ResetTerrainShadowPrimed`, light-clamp env flags, `lightRowCensus*` (269-295, diag) |
| 295-630 | Cost accumulators: SPFLUSH, RENDERLISTS_COST, TXMMGR_BOUNDS, TEXTURE_AUDIT leak counters, shadow shape list, `isAllConcreteTerrainBatch` (duplicate of the gameos_graphics one at line 160!), TGA size helper (~330) |
| 631-1212 | Texture manager lifecycle: `freeVertices`/`Shapes`/`start`/`destroy`/`flush`/`removeTexture`/`flushCache` (~580) |
| 1212-1730 | `addRenderShape` (~520, many `_DEBUG` blocks) |
| 1730-2300 | Baked static light slot API (`mc2*StaticLight*`, LIGHTBAKE, LIGHTSSBO, `addLightDataStructure`, `bakeStaticLightSlot`, `peekLightSlot`) (~570) |
| 2300-2565 | `gos2my` matrix helpers, `class ShapeRenderer` |
| 2565-2840 | Pure object light row (`olr::` adapters, `applyPureObjectLightRow`, light policy publish) (~275) |
| 2838-3214 | `GatherLightsParameters` (~335), `ProbeGatherLightsForList` (3186-3214) |
| 3214-5105 | `renderLists` (~1890 lines, one function, ~17 `MC2_VULKAN_ISLAND` blocks) |
| 5105-6064 | `pinNode`/`update`, `textureFromMemory`, `loadTexture` (~285), `refreshTexturesByName`, `saveTexture`, `copyTexture`, `MC_TextureNode` |

### Diagnostic-only regions
- Cost/census/audit instrumentation 269-295 and 295-500 (~230, RDTSC accumulators, bounds counters, texture-audit leak tracking)
- `[LIGHTBAKE-PROOF]`, parity check, slot cardinality accessors 1747-1760, 2003-2010, 2239-2300 (~100)
- `ProbeGatherLightsForList` 3186-3214 (~30)
- The `MC2_SUBSTRATE_COUNT_PARITY` block at 4308 (size not measured)
- `_DEBUG` blocks in `addRenderShape` (1243-1420, ~100)
- Total about 450-500 lines.

### Split proposal (4 files)
1. `txmmgr_lights.cpp` (~1050): 1730-2300 (baked slot API) plus 2565-3214 (olr row, `GatherLightsParameters`). Mostly free functions plus a few `MC_TextureManager` members (class already in `txmmgr.h`). Shared statics: `s_staticLightHighWater`, baked-slot table, dirty flag, light-policy globals, which become extern in `txmmgr_internal.h`. Medium coupling, but the best seam. It also takes 1100 lines out of the way of `renderLists` readers.
2. `txmmgr_texture.cpp` (~1000): 5105-6064, texture load/save/pin. Low coupling. Needs `s_texture*` cost counters only.
3. `txmmgr_diag.cpp` (~450): RDTSC / audit / bounds / census.
4. `txmmgr.cpp` (~3500): lifecycle, `addRenderShape`, `renderLists`.

Splitting `renderLists` itself needs refactoring of the function into phase helpers (there is a local `bSkip` static spanning phases at line 3225). It is not a file move, so defer it. Churn: medium. Also an easy de-dup win: `isAllConcreteTerrainBatch` exists in both `txmmgr.cpp:529` and `gameos_graphics.cpp:160`. Check that they are equivalent and share via a header.

---

## Cross-cutting recommendations
- The `static const bool x = []{ getenv(...) }()` lambda idiom appears at file scope in both the batcher and `gos_postprocess`. When splitting, use `inline` accessor functions (not duplicated statics) so the gates stay consistent and read once.
- Create `*_diag.cpp` files with the gate check kept in the hot path as a single inlined bool, and move only the formatting/printing bodies. That keeps the hot path readable without adding call overhead when the gate is off.
- Expected agent-cost reduction if done in the priority order above:
  - vulkan_backend_skeleton: 7.6k to about 200-1250 lines per task
  - batcher: 11k to 2-3k per task
  - postprocess: 7.7k to 1-3k
  - gameos_graphics: 11.8k to about 4k (after header move)
  - txmmgr: 6k to 3.5k
- Each split needs updates in the `CMakeLists.txt` source lists and `scripts/` checks that scan files by name (for example the object-MVP currency check `scripts/check-object-mvp-currency.py`, which tags `gameos_graphics.cpp`). Grep the scripts for the old file names before moving code.

---

# Survey addendum 2026-10-09 (HEAD 27a319e54; line numbers are HEAD, re-verify before cutting)

Same method as above (brace-aware function outline, ranges sized from function boundaries, so `~`). Sections 6-17 cover the 12 next-largest files. Sections 12 and 13 amend the existing sections 3 and 1 rather than repeat their tables. Facts true of all 12: no `#include` of a `.cpp` anywhere; `MC2_EVENT` is used 0 times (these files log through `MC2_DIAGF`/`MC2_DIAGFE`, `fprintf`, `DEBUGWINS_print`); no file-scope `static` is referenced by another TU, so every shared name below is a new header declaration, not a visibility change. There is no PCH today: each proposed `*_internal.h` doubles as the include bundle for the PCH the owner wants, so keep its includes to what the sibling files really share. "Stale refs" = comment lines containing `file.ext:NNN` (the de-bloat count); "banner" = pure `//----`/`//****` rule lines.

## 6. code/missiongui.cpp (9979 lines)
Counts: 1484 comment lines (68 banner, 19 stale refs + 2 bare `:NNN`); getenv 25 lines / 20 distinct gates; `MC2_DIAGFE` 6; Tracy 8; fprintf 26; 0 `#if 0`.

| Lines | Responsibility | ~Lines |
|---|---|---:|
| 1-150 | includes (about 25 only for diag/UI-command), `CTRL/SHIFT/ALT/WAYPT/ATTILA_*` macros (201-208) | 150 |
| 151-530 | `terrainStr`, `s_oldTargetWatchID`, hotkey `commands[]` table (271-382), `init`, hover target, `startAnimation` | 380 |
| 531-962 | diag: `MC2_IFACE_COST_SPLIT` accumulators/`MifScope` (535-660), GPU ground-pick parity oracle (661-850), `extern "C"` `mc2FreezeMovers*` (860-962) | 430 |
| 964-2768 | `processUiCommandChannel`: one else-if verb chain (120-line verb catalogue comment + 1685-line body) | 1805 |
| 2771-3828 | `update(void)` | 1060 |
| 3831-5269 | `updateVTol`, `updateTarget`, `updateOldStyle`, `updateAOEStyle`, `update(bool...)` | 1440 |
| 5272-6340 | command handlers: attack/jump/guard/aim*, air strikes, camera*, scroll/zoom/rotate/tilt, draw toggles | 1070 |
| 6343-6914 | `init(FitIni)`, `destroy`, `render`, `doMove`, force groups | 570 |
| 6917-7765 | `moveCameraAround`, `canJump*`, `doDrag`, `makeTargetCursor` (238), `makeNoTargetCursor` | 850 |
| 7768-8340 | reinforcement/VTOL (`beginVtol` 219), salvage/repair, pause API, capture-pause `extern "C"` (8319-8334) | 570 |
| 8341-9237 | debug cheats (`damageObject0-9`, `goalPlan`, `teleport`...), hotkey save/load/set/draw | 900 |
| 9239-9979 | cursors, `calcRotation`, `Save`/`Load`, `updateRollovers`, mouse handlers (9751-9979) | 740 |

Split (11 files, all <= ~1450): `missiongui_uicmd.cpp` (964-1782 + ack epilogue 2735-2768, ~860), `missiongui_uicmd_probe.cpp` (1757-2395: end_mission, time_of_day, light_state, light_row, draw_path, shadow_feed, ground, pixel; ~640), `missiongui_uicmd_fixture.cpp` (2396-2735 + freeze-movers 860-962; ~440), `missiongui_diag.cpp` (535-850; ~320), `missiongui_update.cpp` (update + `updateTarget`; ~1330), `missiongui_style.cpp` (`updateVTol/OldStyle/AOEStyle/update(bool)`; ~1150), `missiongui_orders.cpp` (5272-6340 + 6737-6913; ~1250), `missiongui_cursor.cpp` (6917-7765 + 9239-9382; ~1000), `missiongui_support.cpp` (7768-8340; ~570), `missiongui_debugkeys.cpp` (8341-9237; ~900), `missiongui_input.cpp` (9386-9979; ~600), core `missiongui.cpp` (~930).
Shared state / header: `missiongui_internal.h` holds `struct UiCmdCtx { const std::string& line; const char* result; std::string note, extra; }`, the `MifScope` RAII (used in `update`), `extern` for the `s_mf*`/`s_gp*` counters, and the `CTRL/SHIFT/ALT/WAYPT` macros. `missiongui.h` (11 includers, class has `private:` at 361) gains 3 private member declarations `bool uiCmdWorld/Probe/Fixture(UiCmdCtx&)`.
Surface that must not change: `MissionInterfaceManager` (missiongui.h); `extern "C"` `mc2FreezeMoversActive/MissionReset/Set/...` (mission.cpp, mech3d.cpp, gvactor.cpp consume; data_tools and editor stubs define substitutes); `mc2VisualCapture{GetPauseState,SetPaused,RestorePauseState}` (gos_visual_capture.cpp, sim_identity.cpp, run_golden_parity.py); free functions `BringInReinforcement` (multplyr.cpp), `HandlePlayerOrder`; `g_ifaceCgFrameNs` is DEFINED in controlgui.cpp.
Risks: (1) the verb chain is order-sensitive prefix matching (`list buildings` before `list`, `order move`/`order attack` before the generic `order `, `camera ` vs `camera_follow `); the dispatcher must call groups in original order. (2) File-name pins: `scripts/check-mover-registry-coherence.sh` S3/S4 parse missiongui.cpp (select-all `isOnGUI` count >= 3, "#42b guard shape" is the SOLE defense), `smoke_lib/cursor_guard.py` (3), `gate-defaults-allowlist.txt`, `counter-honesty-allowlist.txt`, `water_class_flip_audit.py`, `camera_sweep_smoke.py`, `run_visual_capture.py`. (3) Namespace-scope dynamic initialisers: `s_gpParityOn = RenderWorld::IsGpuGroundPickParityEnabled()` (633) and the `s_gpSampleN`/`s_gpMissionTag` lambdas run at TU init; move them as function-local statics with their users. (4) B1 deletes `FL_TRACE` (3225) in `update`; `MC2_DAMAGE_*` macros (3293-3303) are local to `update`.
First cut (zero behavior change): 1853-2395, the five RENDERER-INPUT-READBACK verbs (~545 lines, no `ack`/`continue`/`return`, only `line/result/note/extra`) into `uiCmdProbe(UiCmdCtx&)`. Do the rest of `update*` after B1 and G1.

## 7. code/mech.cpp (9796 lines)
Counts: 1675 comment lines (483 banner, 2 stale refs); getenv 5 lines / 5 gates; Tracy 0; `MC2_DIAGF` 0; `#if 0` 170 lines in 8 blocks (3457, 3657-3734, 4458-4483, 7810-7836...); 34 `#define`s, about 25 of them dead (`GOALMAP_DIM`, `BLIP_FRAME_RATE`, `numHotSpots`, `DRAW_*`, `MECH_BASE_*`, `NUM_FEET`, `OTHER_POSITIONS`, `MAX_FRAMES`, `NUM_DAMAGE_SPOTS`... are never used after their definition).

| Lines | Responsibility | ~Lines |
|---|---|---:|
| 1-517 | includes, `s_gpuCullLifecycle` (146), 63 `extern`s, data tables (282-478: `tonnageTurnRate`, `MechClassScale`, hit-location/critical/heat tables) | 517 |
| 518-1224 | `loadGameSystem`, `BattleMechType::init/handleCollision` (250)/`handleDestruction`/`createInstance` | 705 |
| 1225-1574 | `BattleMech::init(bool)`, `clear`, `getStatusRating`, `setControl`, `resolveWeaponMountNode` | 350 |
| 1575-3741 | loadout build: `init(variant)` 771, `resetComponents` 649, `init(FitIni)` 653, `write` | 2170 |
| 3742-4152 | heat sinks/CV/leg status/`pilotingCheck`/`updateHeat`/`mineCheck` | 410 |
| 4153-5659 | movement: `updateJump`, `pivotTo`, `updateMoveStateGoal`, `updateMovePath` 361, `updateMovement`, `netUpdateMovePath` 268, `netUpdateMovement` | 1500 |
| 5660-6089 | jump FX, `crashAvoidanceSystem`, `update{AI,Network,Player}Control`, `updateDynamics` | 430 |
| 6090-6946 | `update` 704 (link-up latch at 6167-6209), `render` | 855 |
| 6947-7230 | `renderShadows`, hit location, jump range, `handleEjection` | 285 |
| 7231-8346 | `hitInventoryItem`, critical hits, status/move chunks, `handleWeaponHit` 507 | 1115 |
| 8347-9331 | `fireGateTrace`, `fireWeapon` ~700, `handleWeaponFire` ~265 (`MAX_WEAPON_FX` at 9059) | 985 |
| 9332-9796 | speeds, `getTotalEffectiveness`, `CopyTo`/`Load`/`repairAll` | 465 |

Split (10 files): `mech_tables.cpp` (1-517 data only, ~330 after externs move to the header), `mech_type.cpp` (518-1224), `mech_loadout.cpp` (`resetComponents` + `init(variant)`, 1575-2993, ~1420), `mech_init.cpp` (1225-1574 + `init(FitIni)`/`write`, ~1090), `mech_move.cpp` (4153-5232, ~1080), `mech_move_net.cpp` (5233-5866, ~630), `mech_update.cpp` (5867-7230, ~1365), `mech_damage.cpp` (7231-8346, ~1115), `mech_weapons.cpp` (8347-9331, ~985), core `mech.cpp` (heat/status 3742-4152 + 9332-9796 + ctor bits, ~900).
Shared state / header: `mech_internal.h` = include bundle (the 37 includes at the top) + the extern table block + an inline `mech_gpuCullLifecycle()` accessor. The same `MC2_GPU_CULL_LIFECYCLE` read is duplicated at mech.cpp:146, mech3d.cpp:73 and objmgr.cpp: de-dup it here. Tables are non-static globals already, so no visibility change; `logisticsTable` (2266, used 2376/2800) stays with `mech_loadout.cpp`.
Surface that must not change: all `BattleMech`/`BattleMechType` members (mech.h, 22 includers); tables extern'd from gameobj.cpp, gvehicl.cpp, mover.cpp, warrior.cpp (`MechClassWeights`, `MechHitSectionTable`, `RankVersusChassisCombatModifier`, `tonnageTurnRate`...).
Risks: (1) four contract scripts read this file by path and must learn the new names in the same commit: `check-mover-registry-coherence.sh` (17 refs: S1a `setOnGUI` count, S2 `addMover(this)` within 10 lines above `setOnGUI(true)`, S5 `if (!isOnGUI())` gate, plus revert-simulations that `sed` the file), `check-order-gate-coverage.py` (`MECH` const; `fireWeapon` must keep the `FIREGATE-REGION-END` sentinel at 8502 and every `return` before it needs `fireGateTrace(` within 4 lines, so `fireGateTrace` and `fireWeapon` MUST land in the same file), `checklib.py` (commented-out `return(4)` and the `//****` phantom-block lexer trap), `.claude/maintenance-rules.json`. (2) comments in `tools/mc2_modlint/modlint_variant_csv.h` cite `code/mech.cpp:1718`. (3) The link-up latch lives inside `update`; keep `update` and `setControl` in one file or point S2/S5 at both. (4) Mid-file macros `JUMPJET_SLOT/SENSOR_SLOT/ELECTRONIC_SLOT/WEAPONAMMO_SLOT` (2341-2344) are used only by `resetComponents`; `ARMOR_*` (301-312) by `init(variant)`.
First cut: `mech_weapons.cpp` = 8347-9331 (`fireGateTrace`, `fireWeapon`, `handleWeaponFire`; ~985 lines). Update `check-order-gate-coverage.py` `MECH` to the new path in the same commit. Delete the 170 dead `#if 0` lines first.

## 8. code/warrior.cpp (9578 lines)
Counts: 1468 comment lines (430 banner, 5 stale refs); getenv 24 lines / 22 gates (9 in 1-250, 7 in `runBrain` 2282-2737, 5 in 5501-5970); Tracy 2; `#if 0` 261 lines in 10 blocks (4357-4428, 6307-6356, 6712-6751...); macros mid-file `OLD_ACKNOWLEDGE_MSG` (1367), `DEBUG_RADIO_MESSAGES` (1455), `USE_ABL_LOAD` (9418-9427).

| Lines | Responsibility | ~Lines |
|---|---|---:|
| 1-136 | includes, `mc2_abl_trace` namespace, externs | 136 |
| 137-250 | brain gate statics `s_brainTaskQ*`/`s_brainRuntime*`/`s_brainFixedTick*`, `initBrain*Gate`, `advanceBrainFixedTickAccumulator`, `getBrainTickIndex`, `getBrainTimeMs` | 115 |
| 251-513 | enums, data tables (`FireOddsTable`, `pilotAlarmFunctionName`, skills/rank tables, update frequencies), `MechWarrior::skillTypes/brainsEnabled` | 265 |
| 514-962 | RNG helpers, `TargetPriorityList::calcAction/calcTarget` (333) | 450 |
| 963-1682 | `operator new/delete`, `lobotomy`, `MechWarrior::init` x2, `MoveOrders::init`, radio, `clear` | 720 |
| 1683-2281 | `destroy`, tac-order queue API (1735-1990), getters, `checkSkill`, `injure`, `eject`, `setBrain*` | 600 |
| 2282-3143 | `runBrain` 456, `setBrainRuntimeMode` .. `isJumping` (24 small) | 860 |
| 3144-3958 | move path: `getMovePath`, `calcMovePath` 549, `getNextWayPoint` | 815 |
| 3959-4539 | `calcWeaponsStatus`, `combatDecisionTree` 285 (nofire trace at 4279-4316), `vectorOffset`, withdraw | 580 |
| 4540-5970 | `movementDecisionTree` 487, alarms (5128-5496), `updateActions`, `mainDecisionTree` 301 | 1430 |
| 5971-6887 | debug orders, `core*` orders | 915 |
| 6888-8130 | `order*` (6888-7918), `handleKilledTarget`, unit-mate events, `updateMissionSkills` | 1240 |
| 8131-8593 | `loadBrainParameters`, debug draws (`drawWaypointPath`, `drawOverviewMovePath`, arrow helpers) | 460 |
| 8594-9578 | `getWayPointMarker`, `calcTacOrder` ~506, goal manager, `copyToData/FromData`, `init`/`shutdown` | 985 |

Split (10 files): `warrior_brain.cpp` (137-250 + 2282-3143 + 8131-8266, ~1100), `warrior_target.cpp` (514-962 + 3959-4539, ~1030), `warrior_move.cpp` (3144-3958 + 4540-5026, ~1300), `warrior_decide.cpp` (5027-5970, ~950), `warrior_core.cpp` (5971-6887, ~915), `warrior_orders.cpp` (6888-8130, ~1240), `warrior_draw.cpp` (8267-8593, ~330), `warrior_queue.cpp` (1683-2281, ~600), `warrior_state.cpp` (`calcTacOrder`, goal manager, `copyToData/FromData`, 8594-9578, ~985), core `warrior.cpp` (tables, `init`/`clear`/radio, ~1100).
Shared state / header: `warrior_internal.h` with a `BrainGates` accessor struct replacing the 14 file statics (read by `runBrain` 2298-2376 and `mainDecisionTree` 5756-5834), the data tables as `extern`, and the include bundle. The gate init functions stay lazy (`*GateChecked` flags), so there is no static-init order hazard.
Surface that must not change: `MechWarrior`, `TargetPriorityList`, `MoveOrders` (warrior.h); `getBrainTickIndex()` and `getBrainTimeMs()` (brain_special_dispatch.cpp, mission.cpp, mech_brain_runtime.h; `tools/brain_dispatch_harness/stubs/brain_tick_stub.cpp` stubs both, so names and signatures stay); `FireOddsTable`, `BrainUpdateFrequency`, `WarriorRankScale` and friends are extern'd by mover.cpp and mech.cpp.
Risks: `check-order-gate-coverage.py` requires the `"ev":"nofire"` emitter gated on `MC2_FIRE_DECISION_TRACE` inside `combatDecisionTree`; `gate-defaults-allowlist.txt`, `counter-honesty-allowlist.txt`, `run_smoke.py` (FX-FORCE-SPAWN fixture comment) name the file. The brain-gate cluster (22 gates) is what G1 rewrites, so cut `warrior_brain.cpp` after G1.
First cut: `warrior_core.cpp` + `warrior_orders.cpp` = 5971-8130 (~2150 lines, `core*`/`order*` touch no file statics and read 2 getenv), as two files. Delete the 261 dead `#if 0` lines first.

## 9. code/ablmc2.cpp (8497 lines)
Counts: 2562 comment lines, the highest ratio in the set: 596 banner lines + 783 bare `//` lines (about 1380 lines, 16% of the file, are rule/blank comment scaffolding, 3-5 per function); 0 stale refs; getenv 8 (6 are `MC2_ABL_TRACE` stubs at 7210-7325); Tracy 0; `#if 0` 77 lines in 6 blocks (7229-7249, 7270-7302...). The file is 284 `void execXxx(void)` ABL built-ins plus host callbacks; the exec functions have external linkage, are not `static`, and are referenced only by the registration table in `initABL`.

| Lines | Responsibility | ~Lines |
|---|---|---:|
| 1-126 | includes (about 75 lines of `#ifndef X_H` guards), globals `CurObjectClass`, `CurAlarm`, `TacOrderOrigin`, `CurMultiplayCode/Param` (137-144) | 126 |
| 127-422 | `abl_arg_guard_log`, `getMoversWithinRadius`, `calcAttackPlan`, `calcBestTarget`, id helpers (`isObjectId`..`getMovers`) | 300 |
| 423-1508 | warrior/contact queries and setters, `execGetId` .. `execGetLastTacOrder` (48 fns) | 1090 |
| 1509-2012 | `execGetObjects` 154, `execOrder*` movement/attack orders | 505 |
| 2013-3008 | damage, distance, object queries, timers, objectives, music/speech/video, mission status (43 fns) | 995 |
| 3009-4605 | object state get/set, salvage, capture, strikes, prisoners, gates, repair (48 fns) | 1600 |
| 4606-5435 | pilot state, messaging, `execCore*` orders, movie mode | 830 |
| 5436-6014 | camera get/set pairs (14), animation callout | 580 |
| 6015-6445 | GUI/logistics callouts, help/target/shelter requests (`MAX_SHELTERS` 6244), text messages, `execAblPrint` | 430 |
| 6446-7632 | teleport, roster/pilot ops, campaign globals (`gCampaignGlobalVars`, `gAbl*Enabled` 6281-6286), strike toggles, radius counts, omni/magic stubs, trigger areas, off-map | 1190 |
| 7633-7944 | ABL host callbacks (malloc/free/file CBs), `ablFatalCallback`, `ablEndlessStateCallback`, `logAblFunctionTableCapacity` | 310 |
| 7945-8350 | `initABL`: heap sizes, `ABLi_init(...)`, 284 `ABLi_addFunction` lines (8003-8340) | 405 |
| 8351-8497 | `closeABL`, `runAblArgGuardRepro`/`2A` (`MC2_ABL_ARG_GUARD_REPRO`, diag) | 148 |

Split (8 files): `ablmc2_queries.cpp` (127-1508 + 4418-4520, ~1500), `ablmc2_objects.cpp` (1509-1662 + 2013-3008, ~1150), `ablmc2_orders.cpp` (1663-2012 + 3854-3963 + 5020-5210 + 7180-7312, ~800), `ablmc2_state.cpp` (3009-4605 minus the moved ranges, ~1350), `ablmc2_misc.cpp` (4606-5435 minus 5020-5210, ~640), `ablmc2_camera.cpp` (5436-6445, ~1010), `ablmc2_roster.cpp` (6446-7632 minus 7180-7312, ~1050), `ablmc2_runtime.cpp` (7633-7944 + 8351-8497 diag repro, ~460), core `ablmc2.cpp` (globals + `initABL` + registration table, ~530).
Shared state / header: `ablmc2_internal.h` = 284 one-line `void execXxx(void);` declarations (generate with a script from the `ABLi_addFunction` lines, then diff to prove complete) + `abl_arg_guard_log`, `getObject/getWarrior/getMovers/isObjectId/isUnitId/isTeamId/double2long`. File statics are local to one group: `tutorialMessages`/`currentMessage` (5993) go with the GUI callouts, `gCampaignGlobalVars`/`gAbl*Enabled` with the roster file.
Surface that must not change: `initABL`, `closeABL`, `getMoversWithinRadius`, `runAblArgGuardRepro*` (mission.cpp, missionbegin.cpp, saveload.cpp), the registered ABL names, and `CurObjectClass/CurAlarm/TacOrderOrigin/CurMultiplayCode` (extern'd by warrior.cpp and others).
Risks: (1) REGISTRATION ORDER IS STATE. `enterStandardRoutine` (mclib/ablsymt.cpp:525) assigns each routine key positionally as `NumStandardFunctions++` into a fixed `FunctionInfoTable` (`MAX_STANDARD_FUNCTIONS` 512, raised from 256 after an overflow; on overflow it logs `[ABL_REG] FULL` and skips the rest), and `logAblFunctionTableCapacity` reports the fill at startup. Keep the single ordered `ABLi_addFunction` list in `ablmc2.cpp`; do NOT move each registration next to its function, and diff the `MC2_ABL_REG_TRACE` dump (key, name, params) before and after. (2) 6 `#if 0` blocks hold the omni/magic stubs; delete before moving. (3) `tools/dup_lines_ratchet.baseline.json` and `run_smoke.py` (magicAiLog comment) name the file; CMake lists it explicitly at CMakeLists.txt:461.
First cut: `ablmc2_camera.cpp` = 5436-6445 (14 camera get/set pairs, movie mode, callouts; ~1000 lines; its only helper dependency is `abl_arg_guard_log`).

## 10. mclib/bdactor.cpp (8317 lines)
Counts: 2110 comment lines (123 banner); stale refs 69 + 26 bare `:NNN`, the highest in the set; getenv 51 lines / 41 gates (`ParseEnvBool` helper, so many reads are invisible to a plain grep); `MC2_DIAGF(E)` 5; Tracy 2; fprintf-ish 57; 358 lines mention diag/trace/probe/census; mid-file macros `SPINRATE/BASE_NODE_RECYCLE_TIME/MAX_WEAPON_NODES` (764-766, used only in the type code), `HEIGHT_THRESHOLD` (5684), `#undef DRAW_BOX` (2757). Two class families (`Bldg*`, `Tree*`) share one set of diag counters.

| Lines | Responsibility | ~Lines |
|---|---|---:|
| 1-151 | includes, `s_animTypeIdleNowStatic`/anim-gate counters (89-91), `g_spr_*`, `g_rc_*`, `s_spotDiagBldg*`, `s_fbDiag*` atomics (100-165) | 150 |
| 152-563 | building PBR sidecar loaders: JSON extractors, `loadBuildingImportSourceWithSidecarAxis`, `uploadBuildingPbrKtx2`, `loadBuildingPbrFromSidecar` | 410 |
| 564-842 | footprint shadow (`submitBuildingFootprintShadow`), `bdLogStaticRegInvalid`, `LoadOverrideRenderShapeTextures` | 280 |
| 843-1345 | `BldgAppearanceType::init` 449, `destroy` | 500 |
| 1346-2138 | `BldgAppearance` accessors, `init` 220, `setObjStatus` 135, `setGesture`, `recalcBounds`, `playDestruction` | 790 |
| 2139-2801 | `BldgAppearance::render` 624, `renderShadows` | 660 |
| 2802-3380 | stable-light-skip and lightbridge-skip gates + diag, `touchEntryDiagRollFrame` 313 | 580 |
| 3381-3634 | `mc2CacheOrBakeStaticGpuLight`, `mc2ConeGroundHit`, light-latch diag | 250 |
| 3635-4551 | `BldgAppearance::update` | 917 |
| 4552-5606 | activity/flash, `isStaticEligible`, `registerStatic` 363, `touch*` (`touchSerialCommit` 247), rebake gates | 1055 |
| 5607-6273 | `destroy`, `calcCellsCovered`, `markTerrain` 269, `markMoveMap`, `markLOS`, `calcAdjCell` | 670 |
| 6274-7316 | `TreeAppearanceType::init` 279, `TreeAppearance` init/status/bounds/`render` 346 | 1045 |
| 7317-8317 | Tree `update`, `mark*`, `touch*`, `registerStatic` 327, `prewarmStaticLightBake`, `destroy` | 1000 |

Split (10 files): `bdactor_sidecar.cpp` (152-842, ~710), `bdactor_type.cpp` (843-1345, ~520), `bdactor_render.cpp` (2139-2801, ~680), `bdactor_diag.cpp` (89-165 counters + 2802-3380 + 3615-3634, ~760), `bdactor_update.cpp` (3381-4551, ~1170), `bdactor_static.cpp` (4552-5606, ~1055), `bdactor_terrain.cpp` (5607-6273, ~670), `bdactor_tree.cpp` (6274-7316, ~1045), `bdactor_tree_static.cpp` (7317-8317, ~1000), core `bdactor.cpp` (1-151 + 1346-2138, ~940).
Shared state / header: `bdactor_internal.h` is required before anything moves. The Tree block alone calls about 10 shared helpers (`mc2CacheOrBakeStaticGpuLight`, `mc2LightBakeEnabled`, `mc2IsBakedStaticLightPresent`, `mc2WriteStaticLightSlot`, `mc2StableLightSkipTouchEnabled`, `lbssRecordTree`, `s_touchEntryDiag`, `g_spr_*`, `s_animTypeIdleNowStatic`, `bldg_anim_gate_counters`). The diag structs live in four anonymous namespaces (2827, 2969, 3057, 4712) and `namespace mc2_lightslot_trace` (3093); they must become named (`bdactor_detail`) and declared in the header.
Surface that must not change: `BldgAppearance(Type)` and `TreeAppearance(Type)` (bdactor.h); non-static globals that `tools/asset_viewer/asset_viewer_game_stubs.cpp` re-defines because the viewer does not link bdactor (`g_fbDiagPtSpotConsumed`, `g_fbDiagPtSpotZeroDir`, ... at bdactor.cpp:166-167): keep names and one definition; `mc2_object_recon`/`mc2_cpu_proj_cost` hooks.
Risks: (1) gate churn: B1 deletes `ANIMATED_PROP_PROBE` (1071) and `LODBUG_TRACE` (2479), B6 deletes `PROP_LOD_SHADOW_CAST` (line 36); `STABLE_LIGHT_SKIP_DIAG` (about 110 lines, 2846-2940) is on HOLD; G1 rewrites every `ParseEnvBool` read. Cut after B1/B6. (2) Name-keyed tools: `check-env-registry.sh` allowlists reads "via ParseEnvBool in bdactor.cpp" (lines 997-1007), `check-light-row-freshness.py` has a per-file rule table for it (line 73) and a phantom-comment note ("225 hidden code lines"), `object_burial_audit.py` (4), `albedo_masked_audit.py` (3), `check-gate-default-agreement.py`, `check-no-raw-gl-from-game.allowlist`, `path-open-baseline.txt`, `mclib/CMakeLists.txt:90` (explicit list). (3) The `g_spr_*`/`g_rc_*`/`s_fbDiag*`/`s_spotDiagBldg*` counters are bumped in place from `update` (3760-4305) and `touchSerialCommit` (5339-5487), so they become extern definitions in `bdactor_diag.cpp` declared in the header (keeping the names the asset-viewer stubs use); the namespace-scope `std::mutex g_rc_shapeMu`/`std::map g_rc_shapeNames` (128-129, dynamic init, used only at 5398-5487) move with `bdactor_static.cpp`.
First cut (after B1): the Tree family 6274-8317 (~2040 lines) as `bdactor_tree.cpp` + `bdactor_tree_static.cpp`, with `bdactor_internal.h` and `bdactor_diag.cpp` landing in the same commit.

## 11. code/mover.cpp (7817 lines)
Counts: 1019 comment lines (354 banner, 0 stale refs); getenv 3 (`MC2_TARGETING_GUARD` 3352, `MC2_CALLED_SHOT_FIX` 6645, one include comment); Tracy 1 (`GameLogic.Mover.Update` at 1755); 0 file-scope statics except `weaponFormToTag` (3258); `#if 0` 80 lines in 6 blocks; debug-ifdef blocks 172 lines (`DebugMoveChunk` 865-874/935-944, `DebugStatusChunk` 1300-1372/1446-1518); 22 `#define`s: `MOVECHUNK_*` (482-494, used 698-948 only), `DEBUG_MOVECHUNK` (836), `DEBUG_STATUSCHUNK` (1240), `MAX_MOVE_GOALS` (4333, used to 5022), `MAX_SHORT_RANGE..MAX_LONG_RANGE` (6447-6451).

| Lines | Responsibility | ~Lines |
|---|---|---:|
| 1-135 | includes | 135 |
| 136-472 | `GOALMAP_CELL_DIM`, `goalMap*` arrays (153-160), tuning globals/tables (`WeaponFireModifiers`, `AttackParameters`, `rearAttackTable`, `WeaponRange(s)`, sensor tables), `Mover::` static members, `TargetRolo` | 340 |
| 473-1782 | net chunks: `getMoverFromHandle`, `MoveChunk` build/pack/unpack/equalTo, `StatusChunk` build/pack/unpack/equalTo, `DebugMoveChunk` 98, `DebugStatusChunk` 209, `MoverDynamics::init` x4, `MoverControl::*` | 1310 |
| 1783-2411 | `Mover::loadGameSystem` 402, `set`, `init` 187, `release` | 630 |
| 2412-2669 | `updateDebugWindow` (diag) | 258 |
| 2670-3340 | `setPartId`.., `setPosition`, `rotate`..`destroy`, `relativePosition`, `setSensorRange`..`hasNullSignature` | 670 |
| 3341-4045 | `handleTacticalOrder` 195, weapon helpers, `getLOSPosition`, `printFireWeaponDebugInfo`, `printHandleWeaponHitDebugInfo`, weapon-fire chunks | 705 |
| 4046-4360 | critical-hit and radio chunks, threat rating, `getDamageClass`, `calcOffsetMoveGoal` | 315 |
| 4361-5602 | `calcBestGoalFromTarget`, `calcLineOfSightView`, `calcGlobalPath`, `canMoveHere` 886, adjacent-cell, hustle | 1240 |
| 5603-6858 | `bounceToAdjCell`, contacts, `calcWeaponEffectiveness`, `calcAmmoTotals`, `calcFireRanges`, `getOrderedFireRange`.., `calcAttackChance` 236, attack-chance tables | 1255 |
| 6859-7166 | `refit`, `calcRecoverPrice`, `recover` | 310 |
| 7167-7817 | sensor text, `initOptimalCells` ~180, `handleEjection`, `Save`, `CopyTo`, `Load` | 650 |

Split (8 files): `mover_chunks.cpp` (473-1782 minus the Debug* functions, ~1000), `mover_diag.cpp` (`DebugMoveChunk` 498-595, `DebugStatusChunk` 1006-1214, `updateDebugWindow` 2412-2669, `printFireWeaponDebugInfo`/`printHandleWeaponHitDebugInfo` 3748-3900, ~720), `mover_init.cpp` (1783-2411 + 4046-4160, ~780), `mover_path.cpp` (4269-5602, ~1330), `mover_combat.cpp` (3341-3747 + 3901-4045 + 5603-6454, ~1400), `mover_attack.cpp` (6455-7166: attack chance, refit, recover, ~715), `mover_misc.cpp` (7167-7817: sensor text, `initOptimalCells`, eject, `Save`/`CopyTo`/`Load`, ~650), core `mover.cpp` (1-472 + 2670-3340 + 4157-4268, ~1250).
Shared state / header: `mover_internal.h` with the `goalMap`/`goalMapRowStart`/`goalMapRowCol` externs, `GOALMAP_CELL_DIM`, `MAX_MOVE_GOALS`, `insertMoveGoal`/`inMapBounds` inline (4335-4357), and the include bundle. Tables at 136-472 are non-static globals extern'd from mech.cpp/warrior.cpp/gvehicl.cpp, so they stay where they are.
Surface that must not change: `Mover` members (mover.h), `MoveChunk`/`StatusChunk` (+ `StatusChunkUnpackErr`, `ChunkDebugMsg`, used by gameobj.cpp, gvehicl.cpp, mech.cpp, move.cpp), `Mover::numMovers`, `TargetRolo`.
Risks: `check-mover-registry-coherence.sh` S1b requires exactly 2 direct `isOnGui =` writes in mover.cpp (init at 2386 and load restore): after the split the count must still be 2 across the files, and the script must scan the list. `water_class_flip_audit.py` names the file. Lane 2 narrowing (C4267) may touch the chunk pack/unpack code if it reaches `code/`: check `git -C A:/Games/mc2-lane2 diff --stat` before cutting.
First cut: `mover_chunks.cpp` = 498-1567 (MoveChunk/StatusChunk, ~1070 lines) with `MOVECHUNK_*`/`DEBUG_*CHUNK` macros moving along; no other range uses them.

## 12. GameOS/gameos/gos_postprocess.cpp (7724 lines): amendments to section 3
Section 3's outline matches HEAD to within 5 lines; this adds what it lacks. Counts: 1801 comment lines (16 banner, 6 stale refs + 7 bare `:NNN`); getenv 105 lines / 93 gates (the highest in the set; 20 in 1-500, 32 inside `gosPostProcess::init` 931-1395, 8 in 5501-5970); `MC2_DIAGFE` 24; 32 `MC2_VULKAN_ISLAND` blocks; Tracy 12; fprintf 82; 0 `#if 0`; no `#define`.
Amendments to the section 3 proposal:
1. Its core is still about 2900 lines. Add two more cuts: `gos_postprocess_fbo.cpp` (`destroy`, `resize`, `createFBOs` 508, `copyScene*`, `destroyFBOs` 173: 1396-2298, ~900) and `gos_postprocess_effects.cpp` (`runSSAO`, `runScreenShadow`, `runCloudShadow`, `runEdgeFog`, `runFogOob`: 3022-3795, ~770). The core (init, `beginScene`, `endScene`, executor wrappers) lands at about 1400 with `init` 465 and `endScene` 486 as the two biggest functions.
2. `mc2ClampShadowSunAltitude` (static, forward-declared at 483, defined at 6175) is also called from `runScreenShadow` (3335), not only from the shadow code (6244, 6713). It must be a header-declared non-static function, otherwise the `_effects` and `_shadow` files both need it.
3. The four region-adapter families (3796-4605: `GlFogRegionBackend`, `GlSsaoRegionBackend`, `GlHzbRegionBackend`, `GlCompositePresentRegionBackend`, each with an anonymous-namespace health struct and a resolver) export `extern "C"` `mc2_render_backend_{,ssao_,hzb_,composite_present_}region_health` and the tail exports `mc2_framegraph_executor_{owned_passes,validation_failures,apply_state_*}`. Consumers: debug_state_dump.cpp, mclib/render_contract.cpp, RenderCore/frame_executor.h, and `tools/render_state_contract_harness/gos_counter_stubs.cpp`, which STUBS these exact symbols because the harness does not link this TU. Keep the symbol names; if the executor accessors move, update that stub list and `tests/unit/CMakeLists.txt:205`.
4. File-name pins (all must learn the new files): `scripts/check-apply-pass-bumped.py`, `check-drawbuffer-ownership.py` (2), `check-frame-resource-feedback.py` (3), `check-gate-default-agreement.py` (6), `check-raw-gl-{blendfunc,colormask(3),depthfunc,depthmask,fbobind}.py`, `check-sampler-bindings-selftest.py` (5), `check-test-tier-coverage.py`, `counter-honesty-allowlist.txt` (14 rows), `gl_shader_variants.json` (20 refs), `tools/shader_offline_build/pilots.json` (7), `res-aspect-baseline.json`, `.claude/maintenance-rules.json`, `GameOS/gameos/CMakeLists.txt` (21, 292, 429), `tools/mc2_launcher/mc2_launcher.cpp` (4).
5. Static-init: the file-scope idiom `static const bool x = [] { getenv(...) }()` (cross-cutting note) appears here too; `kQuadVerts`, `s_postProcess`, `s_viewmodeFramework*` (724-736) are plain data. `s_postProcess` is read by `getGosPostProcess()` (23 includer TUs) and must keep one definition.
6. Ordering vs lane 1: B2 deletes `CLOUD_DIAG` (3431), `HDRI_SKY_STATE_PROBE` (5334/5627/5813), `SHADOW_FOCUS_DIAG` OR-term (6926); B4/B5 remove the `VK_POST_*` direct-chain gate term (150-153) and the `=0` overrides; G0/G1 rewrite all 93 readers, including the 49-function getter wall (57-815). So the section 3 `gates.cpp` cut is the WORST first move (it is exactly what G1 replaces); do it after G1a.
First cut: the shadow family 5959-7219 (static + dynamic shadow, ~1260 lines, 13 getenv lines, 1 `MC2_VULKAN_ISLAND` block) as `gos_postprocess_shadow.cpp`, after B2 lands (the `SHADOW_FOCUS_DIAG` term at 6926 sits inside it). Needs `mc2ClampShadowSunAltitude` exported.

## 13. GameOS/gameos/vulkan_backend_skeleton.cpp (7640 lines): amendments to section 1
Section 1's outline matches HEAD to within 5 lines. Counts: 790 comment lines (20 banner, 6 stale refs); getenv 5 (`MC2_VULKAN_VALIDATION` 92, `MC2_VULKAN_CACHE_DIR` 308, `MC2_VULKAN_PROBE` 7594, `MC2_VULKAN_SPV_DIR` 7602, `MC2_VULKAN_SWAPCHAIN_PRESENT` 7617); Tracy 0; logging through the local `log()` wrapper only; 6 anonymous namespaces (30, 286, 854, 1180, 1649, 6179); `kWaterMvpRaw16`/`kWaterDepthFudgeFast` (6809-6827) are private to the watermvp fixture.
Corrections and additions:
1. Section 1 calls the whole file diagnostic. It is not: `mc2_vulkan_load_spv` and `mc2_vulkan_free_shader` (140-186) are production API, used by `vulkan_present_object.cpp` and declared in `vulkan_backend_skeleton.h:33-34`. They must move to an always-built TU (`vulkan_spv_loader.cpp`, about 60 lines), NOT into `vulkan_probe_common`. That file is also the natural home for the D5 de-dup of the 13 hand-copied `spv_dir()`/`load_spv()`/`vlog()` helpers (opus-consistency A8).
2. D7 / opus-consistency A7 proposes retiring the fixtures: `EdgeFogParams` (100 B at :1988) and `FogOobParams` (84 B at :2690) are stale mirrors of the 112 B / 96 B shader blocks, and `vulkan_shadow_island.cpp` (the only other definer, of `mc2_vulkan_build_shadow_depth_pipeline`) is called only from this probe. If the owner approves D7, DELETE 140-7470 minus the loader instead of splitting (about 7.3k lines gone, the stale mirrors with them); the 5-way split below is the fallback if the fixtures are kept. Decide D7 before spending a lane here.
3. `scripts/run_vulkan_probe.py:94` parses the log prefix `[VULKAN_SKELETON] <probe>: VALIDATION: <msg>`: every split file must keep the prefix produced by the shared `log()`. `scripts/deploy_payload.py:844` and `tools/vulkan_shader_probe_main.cpp` also depend on this TU's behavior. CMake lists it twice (CMakeLists.txt:156 explicit, GameOS/gameos/CMakeLists.txt:105 `list(APPEND ...)`), both under `MC2_VULKAN`.
4. External surface: the 14 `mc2_vulkan_probe_*` prototypes and `Mc2VkShadowDepthPipeline` in the header; callers gos_render.cpp (677), vulkan_present_object.cpp, vulkan_swapchain_present.cpp, vulkan_swapchain_probe.cpp, tools/vulkan_shader_probe_main.cpp.
5. Shared state: the 12 anonymous-namespace helpers named in section 1 (`log`, `ensure_volk_initialized`, `resolve_validation_preset`, `find_mem_type`, `pipeline_cache_path`, `read_pipeline_cache_file`, `create_vma_allocator`, `device_type_str`, three `*_debug_cb`): `vulkan_probe_common.h` with `namespace mc2vk_probe`. CPU reference functions stay file-local to the fog/ssao file. No static-init hazards; no macros.
First cut: `vulkan_spv_loader.cpp` (140-186) first (prerequisite for either path), then `vulkan_probe_shadow_water.cpp` = `shadowdepth` + `watermvp` fixtures (6185-7470, ~1285 lines, needs only `log`, `find_mem_type`, `create_vma_allocator`, `mc2_vulkan_load_spv`).

## 14. editor/EditorInterface.cpp (7370 lines)
Counts: 1123 comment lines (31 banner, 5 stale refs + 5 bare `:NNN`); getenv 4 (`MC2_EDITOR_TRACE` 251, `MC2_EDITOR_DRAG_TRACE` 1745, `MC2_TERRAIN_PROBE` 2926, `MC2_EDITOR_GPU_PICK` 4300); Tracy 0; `#define`s `AssetType_Virtual`/`ProviderType_Secondary` (267-271), `MAX_THRESHOLD/NOISE` (3278-3281, used 3295-3296 only); the ImGui include block is `#ifdef MC2_IMGUI` (72-127) but the editor target requires `MC2_IMGUI=ON` (editor/CMakeLists.txt:40-45), so the `#else` stubs at 6303-6308 are dead.

| Lines | Responsibility | ~Lines |
|---|---|---:|
| 1-319 | includes, `EditorRttClientToViewport`, `EditorShutdownTrace`, file-filter strings (311-312), `windowSizeChanged`/`g_newWidth/Height` (314-316) | 320 |
| 320-950 | `Editor::init` 283, `destroy`, `resaveAll` 177, `Editor::render/update` | 630 |
| 951-1648 | `EditorInterface` ctor/`init`, New menu (`addBuildingsToNewMenu`, `handleNewMenuMessage`), Quit/Open | 700 |
| 1649-2455 | `handleLeftButtonDown`, `handleMouseMove` 298, `handleKeyDown` 207, `updateCameraInput` | 800 |
| 2456-2823 | brushes, `SaveAs`, `Save`, `Erase`, `g_objectRecentRing` (2719) | 370 |
| 2824-3282 | `update` 248, `render` 144, `NewHeightMap` | 460 |
| 3283-4217 | terrain tools (`RefractalizeTerrain`, `SelectSlopes/Altitude`, `SaveHeightMap`), `MissionSettings`, Team/Player, scroll sync, texture dialogs, Link/DropZone | 935 |
| 4218-4481 | `OnCreate`/`OnTimer`, click/pick helpers (`EditorInterface_PickObjectAtScreenPoint[_CPU]`, `EditorPickObjectAtScreen`), `VkToImGuiKey`; click-tracking statics (4264-4269) | 265 |
| 4482-5168 | `WindowProc`, `OnLButtonDown/Up` 220, `OnMouseMove/Wheel`, `renderTerrainSelection` | 690 |
| 5169-6308 | ImGui panels: `renderToolbarImGui` 471, companion 228, info 93, place 80, mission tools 219; statics `s_placePanelOpen`, `s_missionToolsOpen`, `s_uiScale*` (5203-5210, used only here) | 1140 |
| 6309-7371 | scroll handlers, `OnSysKeyDown`.., `OnPaint`, view refresh, `applyObjectAlignment`, smoke fixtures (`runInspectorEditSmoke`, `runPlaceOobSmoke`, 6921-7183), foliage/forest tools | 1065 |

Split (7 files): `EditorInterface_imgui.cpp` (5169-6308, ~1140), `EditorInterface_mouse.cpp` (1649-2143 + 4218-4481 + 4593-5105, ~1250), `EditorInterface_wnd.cpp` (4482-4592 + 6309-6680, ~480), `EditorInterface_terrain.cpp` (3283-4217, ~935), `EditorInterface_editor.cpp` (320-950 + 951-1648, ~1330), `EditorInterface_smoke.cpp` (6921-7183, ~260, diag), core `EditorInterface.cpp` (prologue + 1649-2455 keys/camera + 2456-3282 + 6681-6920 + 7184-7371, ~1500: trim by moving `Save*` to `_editor`).
Shared state / header: `EditorInterface_internal.h` with `extern const char szTGAFilter[]/szPAKFilter[]` (used at 516, 1613, 2571, 3218, 3722, 4033-4143), `windowSizeChanged/g_newWidth/g_newHeight` (used 827-838, 4575-4576), `SQRT_2` (3855, used to 6472), click-tracking statics. Class bodies stay in `EditorInterface.h` (34 includers), so member splits cost nothing.
Surface that must not change: `EditorInterface`, `Editor` (EditorInterface.h); `EditorPickObjectAtScreen(int,int)` extern'd by DamageBrush.cpp and LinkBrush.cpp (declared locally with `extern`, no header).
Risks: CMake globs `editor/*.cpp` with `CONFIGURE_DEPENDS` (new files are picked up after a reconfigure, no list edit). Name pins: `scripts/check-raw-gl-fbobind.py`, `deploy-editor.sh`, `build_release.sh`, `counter-honesty-allowlist.txt`. B3 deletes `EDITOR_DRAG_TRACE` (1745) first. The editor target is separate (EditRel) and is NOT covered by tier1 smoke: validate with the editor build and `run_editor_playtest_smoke.py`. 
First cut: `EditorInterface_imgui.cpp` = 5169-6308 (~1140 lines, self-contained statics). Land after B3.

## 15. mclib/move.cpp (7050 lines)
Counts: 1055 comment lines (350 banner, 0 stale refs); getenv 0; Tracy 3; 41 debug-ifdef blocks (280 lines, mostly `MOVELOG`/`_DEBUG` one-liners); `checklib.py` records this file as the second-worst phantom-block-comment case (2470 code lines read as comments), so text-lint contracts effectively cannot see most of it; macros mid-file `NUM_MISSIONMAP_PACKETS` (966), `GLOBALMAP_VERSION_NUMBER` (1479), `MAX_DOORS_PER_SECTOR` (2661), `AREA_ID_BASE` (3688), `MAX_GLOBAL_PATH` (3769, used 3059-4093), `MAX_MAPWIDTH` (5245, used 5337-6708), `MOVELOG` (20).

| Lines | Responsibility | ~Lines |
|---|---|---:|
| 1-59 | includes (`gamelog`, `move_recon`, `path_trace`, `move_solvectx`), `safeReadPacket` | 60 |
| 60-348 | `readDoorLinksMCO` 289; statics `cellShift` (149), `StepAdjDir` (340), `adjTile` (342) | 290 |
| 349-592 | `agsqrt`, `inMapBounds`, `mineLayout`, `TeamRelations`, path globals, `typeString`, `relativePositionToPoint` 144, `buildCellShiftDistanceTable`, `MOVE_init` | 245 |
| 593-1480 | `MOVE_buildData/saveData/readData/cleanup`, `MissionMap` | 890 |
| 1481-2282 | `GlobalMap::init` 547, `write` 177, `calcSpecialAreas` | 800 |
| 2283-3135 | area tables, `calcGlobalDoors`, `calcLinkCost` 154, `calcDoorLinks`, `getPathCost` | 850 |
| 3136-4362 | `setPathFlag`.., `build`, `setStartDoor`.., `propogateCost` 449, `calcPath`.., `print`, `destroy` | 1230 |
| 4363-5326 | `toggleLog`.., `MoveMap::setGoal`, `markGoals`, `markEscapeGoals` 424, `propogateCost` | 965 |
| 5327-6457 | `propogateCostJUMP`, `MoveMap::calcPath` 561, `calcPathJUMP` 489 | 1130 |
| 6458-7050 | `calcEscapePath` 475, `writeDebug`, `destroy` | 595 |

Split (7 files): `move.cpp` core (1-59, 349-1480 minus door-link parse, ~1100), `move_globalmap_build.cpp` (60-348 + 1481-2282, ~1090), `move_globalmap_doors.cpp` (2283-3135, ~850), `move_globalmap_path.cpp` (3136-4362, ~1230), `move_movemap_goals.cpp` (4363-5326, ~965), `move_movemap_path.cpp` (5327-6457, ~1130), `move_movemap_escape.cpp` (6458-7050, ~595).
Shared state / header: `move_internal.h` with `cellShift` (extern const array or C++17 `inline constexpr`; used by `buildCellShiftDistanceTable` and 14 `MoveMap` sites 4476-6748), `StepAdjDir`, `adjTile`, `typeString`, inline `agsqrt`/`inMapBounds`, and the macros above. Keep the small helpers `inline` so the hot `calcPath` loops still inline across the new TUs.
Surface that must not change: `MissionMap`, `GlobalMap`, `MoveMap` (move.h) and `MOVE_*`; globals `PathFindMap[2]`, `GlobalMoveMap`, `TeamRelations`, `mineLayout`, `GoalIsDoor`, `MaxHPrime`, `numNodesVisited` (extern'd by mover.cpp, warrior.cpp, mission.cpp); `gos_terrain_indirect.h` include for `MarkMineDirty`.
Risks: path-finding parity must be proven (the cost-accumulator zones and `MC2_MOVE_RECON`/`path_trace` telemetry exist for this: compare counters on the same mission before/after); `PathFindMap` and `MoveMap::distanceFloat/Int` are static data with constant init, so no init-order hazard. `mclib/CMakeLists.txt:96` lists the file explicitly; `water_class_flip_audit.py` names it. No gate churn: nothing blocks this file but lane 2 narrowing (C4267 in `mclib/`).
First cut: `move_movemap_path.cpp` + `move_movemap_escape.cpp` = 5327-7050 (`calcPath`, `calcPathJUMP`, `calcEscapePath`, ~1725 lines as two files); they use only `cellShift/StepAdjDir/MAX_MAPWIDTH`.

## 16. mclib/mech3d.cpp (6877 lines)
Counts: 1210 comment lines (161 banner); stale refs 36 + 9 bare `:NNN`; getenv 21 lines / 15 gates (9 in 1-500; the rest are mid-function reads, e.g. `MC2_MECH_RESTORE_TRACE` three times at 3045/3458/6751 and `MC2_LOG_PREVIEW` three times); `MC2_DIAGF(E)` 4; Tracy 6; 17 `#define`s (`BODYSTATE_*` 281-285, `EXPAND_FACTOR`, `MAX_ANIMATION_FILES`, `FX_*_ID`, `SPIN_RATE`, `FOOTPRINT_SLOP` 368 used 4653-4811, `MAX_MECHS` 1033, `idleMAX`); the `#if defined(BGR)` blocks at 2291/2308 are dead (the define is commented out in mclib/CMakeLists.txt:147).

| Lines | Responsibility | ~Lines |
|---|---|---:|
| 1-181 | includes; diag counters (`s_mlrMechDrawsThisMission`, `s_lcSkipCount3d`, `s_spotDiagMech*`, `s_mechStaleLightRow*`), `consumeAndReset*` (62-181), FX-draw counter namespace (135-181) | 181 |
| 182-373 | `textureOrKtxSidecarExists` 192, `MechStateByGesture` (291) and the `BODYSTATE_*` block | 192 |
| 374-1018 | `Mech3DAppearanceType::init` 582, `destroy` | 645 |
| 1019-1430 | weapon-node lookup (`getWeaponNodePosition`, `getWeaponNode`, `getLowestWeaponNode`, socket-name resolve) | 410 |
| 1431-2002 | `Mech3DAppearance::init` 483, `initFX` | 570 |
| 2003-2492 | `hitFront`.., `LoadPaintSchemata`, `setPaintScheme`, `resetPaintScheme` 173 | 490 |
| 2493-3016 | `setObjectParameters`, `debugUpdate` 122 (diag), `isMouseOver`, `recalcBounds` 324 | 525 |
| 3017-3984 | `render` 930, `renderShadows` | 970 |
| 3985-5281 | `setObjStatus`, `updateGeometry` 1061 (Tracy `GameLogic.Mech3D.UpdateGeometry`), `leftArmRecalc`/`rightArmRecalc` | 1300 |
| 5282-6016 | `update` 735 | 735 |
| 6017-6878 | `blowLeftArm/RightArm`, smoke, water wake, ejection, `destroy`, `copyTo`/`copyFrom`, hit-node getters | 860 |

Split (7 files): `mech3d_type.cpp` (182-1430 minus `MechStateByGesture`, ~1050), `mech3d_init.cpp` (1431-2492, ~1060), `mech3d_render.cpp` (2693-3984: `recalcBounds`, `render`, `renderShadows`, ~1290), `mech3d_geometry.cpp` (3985-5281, ~1300), `mech3d_update.cpp` (5282-6489, ~1210), `mech3d_diag.cpp` (62-181, `debugUpdate`, FX counter emit, ~300), core `mech3d.cpp` (prologue, `destroy`/`copy*`, accessors, ~700).
Shared state / header: `mech3d_internal.h` for `s_gpuCullLifecycle` (read at 14 sites from 1078 to 6830; use an inline accessor shared with mech.cpp), `s_mechLodTrace`, `s_fxCountLog` + `fxCountTick(int)` (called 11 times inside `render`), `g_mechPreviewRenderDepth` (3036-4622, also extern'd by gos_mech_batcher.cpp, camera.cpp, tgl.cpp, bdactor.cpp, `RenderCore/ResolutionContract.h`, and re-defined in `tools/asset_viewer/asset_viewer_game_stubs.cpp`), `FOOTPRINT_SLOP`/`EXPAND_FACTOR`/`BODYSTATE_*`/`FX_*` macros, `MechStateByGesture` (extern'd by mech.cpp and declared in mech3d.h).
Surface that must not change: `Mech3DAppearance(Type)` (mech3d.h); `consumeAndResetMlrMechDraws`, `consumeAndResetMechLightRowCounters` (gos_mech_batcher.cpp); `MechStateByGesture`; `g_mechPreviewRenderDepth`.
Risks: (1) name-keyed scanners: `check-light-row-freshness.py` has a per-file rule for mech3d.cpp (MECH-LIGHTROW-FRESHNESS-1, line 79), `check-worktree-context.sh` (the stale-root-edit incident), `albedo_masked_audit.py` (8), `tools/unit_converter/emit.py` (2), `counter-honesty-allowlist.txt` (4), `run_smoke.py` (ENV_DROP comment), `.claude/maintenance-rules.json:49`; `mclib/CMakeLists.txt` lists it 3 times. (2) The MC2_GPU_MECH_* kill-switches use `envFlagDefaultOn` mid-function; G1 touches them. B1 deletes `ANIM_ADVANCE_TRACE` (5636, ANDed with `mc2_diag::tagEnabled`). (3) Dynamic init: `s_gpuCullLifecycle`, `s_mechLodTrace`, `s_lcTrace`, `s_spotDiagMechEnabled`, `s_fxCountLog` are namespace-scope `static const bool x = getenv(...)`: after the split keep ONE definition each (header accessor) or the same env var is read by several TUs (it already is: mech.cpp, mech3d.cpp, objmgr.cpp). (4) `updateGeometry` is the per-frame hot path (Tracy `GameLogic.Mech3D.UpdateGeometry` sub-zones): compare zone times before/after. The `s_buildRecipeOnly`/`s_multiShapePositionsOnly` flags are defined in msl.cpp (mech3d.cpp only cites them in comments), so they are unaffected.
First cut: `mech3d_geometry.cpp` = 3985-5281 (`setObjStatus`, `updateGeometry`, arm recalcs, ~1300 lines; needs `FOOTPRINT_SLOP`, `s_spotDiagMech*`, `s_gpuCullLifecycle`, one getenv at 4115). Land after B1 (ANIM_ADVANCE at 5636 is in `update`, not in the cut, but G1 rewrites the shared accessors).

## 17. GameOS/gameos/gos_terrain_indirect.cpp (6330 lines)
Counts: 2006 comment lines (60 banner, 62 empty `//`); stale refs 52 + 7 bare `:NNN`; getenv 43 lines / 37 gates (12 in the 105-446 gate-reader wall, 13 in the cement block, 5 in `ComputeDispatch`); `MC2_DIAGF(E)` 51 (15 in the pack/resources block); Tracy 8; 5 `MC2_VULKAN_ISLAND` blocks; about 85 file-scope statics/globals in 3 anonymous namespaces (564-736, 757-2648, 3112-3895) plus the named `gos_terrain_indirect` namespace; macros `FCMP` (3026, `#undef` 3072), `PROBE_LOG` (4276, used 4358-5075), `MINE_GLPROBE` (5472-5662).

| Lines | Responsibility | ~Lines |
|---|---|---:|
| 1-104 | includes, `s_loadRaceDelayAtlas` (97, getenv at TU init) | 104 |
| 105-446 | about 30 `Is*Enabled()` gate readers (cost split, mine, overlay, parity, light-cost-split), `s_thinCanaryEnabled` (276) | 340 |
| 447-631 | `ParityFrameTick` 160, `traceOn`, `s_indirectTrace*` (602-606) | 185 |
| 632-810 | `buildRecipeSlot` 179; atlas globals `g_atlas*` (759-764) | 180 |
| 811-2017 | cement/concrete heuristics (`RunwayToneScale`, `ClassifyConcreteFamily`, `CementCellComputeStats`, alpha-atlas gates), `PopulateRecipeCementWords` 457, `BuildTransitionMaskArray`; statics 777-807, 930-1004, 1146-1187 | 1210 |
| 2018-2837 | `BuildColormapAtlas` 174, `BuildCementCatalogAtlas` 483, `BuildDenseRecipe`, `ResetDenseRecipe` | 820 |
| 2838-3262 | dense-recipe flush, `ParityCompareRecipeFrame` 342 (diag) | 425 |
| 3263-4149 | `ResourcesReady`, `PackThinRecordsForFrame` 256, `BuildIndirectCommands`, handle LUT, `BuildSolidQuadWindowSSBO` 176, frame-armed predicates; thin ring/MVP-snapshot/uniform-location statics (3118-3257) | 890 |
| 4150-5112 | `ComputePreflight`, `ComputeDispatch` 963 | 960 |
| 5113-5478 | `DrawIndirect`, `RenderWaterReflectionPass`, ring-slot/MVP accessors | 365 |
| 5479-5864 | mine texture array/static VBO/draw/dirty, cement-atlas dirty flag | 385 |
| 5865-6330 | `BuildDecalStaticVBO` 396, rebuild/dirty | 465 |

Split (8 files): `gos_terrain_indirect_cement.cpp` (811-2017, ~1210), `gos_terrain_indirect_atlas.cpp` (632-810 + 2018-2837, ~1000), `gos_terrain_indirect_diag.cpp` (447-631 + 2838-3262, ~610), `gos_terrain_indirect_pack.cpp` (3263-4149, ~890), `gos_terrain_indirect_dispatch.cpp` (4150-5478, ~1330), `gos_terrain_indirect_mine_decal.cpp` (5479-6330, ~850), core `gos_terrain_indirect.cpp` (1-446 gate wall + public API shims, ~500).
Shared state / header: this is the hard part. A `gos_terrain_indirect_internal.h` must declare in a named `detail` namespace (the three anonymous namespaces cannot be shared across TUs) about 40 `extern` globals: GL handles and flags (`g_atlas*`, `g_cementAtlas*`, `g_transitionMaskArrayGL`, `g_thinRecordSSBO`, `g_thinRingSlot`, `g_thinRingFences`, `g_solid*`, `g_loc*`, `g_dispatchMvp*`, `g_thinSlotMVP*`, `g_recipeSSBO`, `g_recipeMapSide`, `s_frameSolid*`) and the constexprs (`kThinRingFrames`, `kMaxThinRecords`, `kCement*Bit`, `kTransitionMask*`). Those globals mirror GPU state; a single `struct IndirectState` in the header (one definition, accessed as `detail::state().x`) is the better end state and is a design step, not a file move. The mine/decal block needs only about 10 of the shared names (`g_dispatchMvpFloats`, `g_thinSlotMVP*`, `g_thinRingSlot`, `g_recipeSSBO`, `g_recipeMapSide`, `g_mvpDiagFrame`, `g_envRingTrace`) plus its own `g_mine*`/`g_decal*`.
Surface that must not change: everything in `gos_terrain_indirect.h` (88 declarations, 29 includers, e.g. `MarkMineDirty` from move.cpp), `gos_terrain_indirect_getRingSlot`, the MVP snapshot getter used by `check-object-mvp-currency.py`, `[TERRAIN_INDIRECT v1]` log lines.
Risks: (1) heavy pins by file name: `check-buffer-lifetime-ownership.py` (ring fence scan list, line 35), `check-cpu-gpu-cogating.py` (`reader_file`, 136), `check-object-mvp-currency.py` (32: "defines the snapshot + getter", so `g_dispatchMvp*` must stay in the named file), `check-raw-gl-fbobind.py`, `predict_cement_adopt.py` (5 refs, cement block), `scripts/delivery-fixes.json` (3), `gl_shader_variants.json`, `counter-honesty-allowlist.txt`, `.claude/maintenance-rules.json:37`. (2) Gate churn: B2 deletes `MVP_EARLY_TRACE` (4731-4743, inside `ComputeDispatch`); G1 rewrites the 105-446 wall and the 13 cement-block reads. Cut after B2; cut the gate wall and cement block only after G1. (3) Namespace-scope `getenv` initialisers (97, 3231) and `GLsync g_thinRingFences[3] = {0,0,0}` are constant or lazily read; no cross-TU order hazard, but each must have exactly one definition. (4) `PROBE_LOG` and `FCMP` are local-scope macros: copy their definitions with the code that uses them, do not put them in the header. (5) The 963-line `ComputeDispatch` stays one function (a ~1330-line file); splitting it is a refactor, not part of this plan.
First cut: `gos_terrain_indirect_mine_decal.cpp` = 5479-6330 (~850 lines, ~5 getenv, its own `g_mine*`/`g_decal*` statics, ~10 shared names). Do after B2; leave the cement/dispatch cuts until G1 and the `IndirectState` header exist.
