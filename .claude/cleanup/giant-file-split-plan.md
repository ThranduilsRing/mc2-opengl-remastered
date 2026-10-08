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
