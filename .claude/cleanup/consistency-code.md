<!-- doc-status: stale | reviewed:2026-10-11 -->
# Code self-consistency audit (read-only, 2026-10-09)
Method: rg + manual read. No builds. tools/check_ubo_layout.py already guards terrain/overlay/ColorFrameUBO(frag)/WaterFsParams/postfog/ssao/screenfx UBOs; this audit targets what it does NOT cover. Ranked by bug risk. "Right side" = which side I believe is correct.

## HIGH

### H1. lightgrid_build.comp record stride is 16 slots at runtime vs 32 in the real SSBO
- C++: `GameOS/gameos/gos_lightgrid_build.cpp:30` `kMaxLights = 16`, injected at `:118` as `#define LIGHTGRID_MAX_LIGHTS 16`.
- GLSL: `shaders/lightgrid_build.comp:31-33` defaults to 32 (`#ifndef`), and `:63-68` sizes `LG_ObjectLights.light_to_world/light_dir/light_color/light_falloff[LIGHTGRID_MAX_LIGHTS]`. Its own comment (`:28-30`) says the mirror MUST equal the record stride (32) "or the sphere-build reads garbage".
- Real layout: `shaders/include/lighting.hglsl:33,45-51` MAX_LIGHTS_IN_WORLD=32; `mclib/tgl.h:309` MAX_HW_LIGHTS_IN_WORLD=32; `mclib/object_light_row.h:90` kMaxLights=32. The C++ `#define` defeats the shader default, shrinking the std430 record from 3600 B to 1808 B: `numLights` is read at byte 1792 instead of 3584, and `light[i>0]` strides are wrong.
- Stale comments: `gos_lightgrid_build.cpp:27` ("lockstep with lighting.hglsl MAX_LIGHTS_IN_WORLD" - that is 32, not 16); `mclib/tgl.h:184-186` (claims shader value "stays at 16").
- Right side: GLSL default (32) and the ABI. Gate is a default-OFF substrate, so latent.
- Fix: split the meanings. Keep a fixed `LG_RECORD_LIGHTS 32` for the struct and a separate `LIGHTGRID_MAX_LIGHTS` (16) only for local_size/loop caps. Add `static_assert(kMaxLights <= object_light_row::kMaxLights)`; make check-light-abi-lockstep.py parse lightgrid_build.comp and the injected define. Fix both comments.

### H2. VK shadow caster draws alpha-test (tree/fence cards) as opaque slabs; GL discards by alpha
- GL: `shaders/shadow_static_prop.frag:63-83` samples the albedo array when ALPHA_TEST_BIT is set, discards at `a < 0.5`.
- VK: `shaders/vulkan/shadow_prop.frag` is an empty main ("P8a is OPAQUE-ONLY", header line 6). The feed `mc2_shadow_island_get_geometry_feed` (`GameOS/gameos/gos_static_prop_batcher.cpp:9835-9884`) carries only firstIndex/indexCount/baseVertex per packet: no materialFlags, texture layer or uv scale. `vulkan_shadow_island_live.cpp` has no alpha handling.
- Result: every card of every alpha-test packet casts a full rectangle shadow in VK. Same class as "tree dither missing in VK".
- Right side: GL. Fix: add materialFlags/texArrayLayer/uvScaleX/Y to Mc2ShadowPacket and the feed; add an alpha-test shadow pipeline variant (GL `MC2_SHADOW_PROP_ATLAS` branch is the template) with the same discard.

### H3. `extern int mc2CaptureGaitFreezeActive();` has C++ linkage, definition is `extern "C"`
- Declaration: `GameOS/gameos/vulkan_color_prop_island.cpp:1687` (block scope; no prior C-linkage declaration visible in that TU, it does not include vk_feed_dump.h).
- Definition: `GameOS/gameos/gos_visual_capture.cpp:680` `extern "C" int mc2CaptureGaitFreezeActive()`. Other users (`mclib/mech3d_internal.h:22`, `vk_feed_dump.h:36`) correctly use `extern "C"`.
- Under MSVC the C++ declaration mangles to `?mc2CaptureGaitFreezeActive@@YAHXZ`, so this is LNK2019 whenever `MC2_VULKAN_ISLAND` is compiled and linked. Not built to confirm.
- I scanned every block-scope `extern` function declaration in GameOS, RenderCore, RenderWorld, mclib, code, GameAdapters, GuiRuntime and gui against `extern "C"` definitions: this is the only real mismatch. `mc2VisualCaptureGetPauseState` at `gos_visual_capture.cpp:682` is fine (earlier `extern "C" {}` at `:82-85` is visible).
- `gos_GetTerrainLightDir` (your example) is consistent: `GameOS/include/gameos.hpp:2345` (C++), definition `GameOS/gameos/gameos_graphics_tparams.cpp:667` (C++), block-scope redeclarations at `gos_static_prop_batcher.cpp:7445` and `:10043` are outside the anonymous namespace, so OK (redundant, not wrong). The `::` call at `:1411` was already handled.
- Fix: delete the local extern and include vk_feed_dump.h, or declare it `extern "C"`.

### H4. VK color-prop vertex lighting skips instance flags that GL honours
- GL `shaders/static_prop.vert`: lightsOut bit0 (`:558-559`), isWindow bit1 (`:685,697-700`, `lit = base_light` only, no sun/ambient), light billboard bits 4/5 (`:426-428,482`), highlight `lit = clamp(lit + inst.aRGBHighlight.rgb, 0, 1)` (`:1102`); the frag also adds `v_highlight` (`static_prop.frag:468`).
- VK `shaders/vulkan/color_prop_tier1.vert:~160-322` reads only bit 2. Windows are lit like walls; selection/objective highlight, lightsOut and billboards are ignored. Base light is a daytime-only stub (`isNight=false`, comment `:186-190`), so night hot-colour tags give 0.
- Stale header: `gos_static_prop_batcher.h:~32` says "bit 2: isSpotlight"; GL (`static_prop.vert:690`, `kFlagIsAlphaTest = 1<<2`) and `GameOS/gameos/gos_static_prop_batcher_submit.cpp:832` use bit 2 for alpha-test. Define the bits once (C++ enum + .hglsl defines).
- Right side: GL. Fix: port isWindow/highlight/lightsOut branches; document or implement night base-light.

## MEDIUM

### M1. GL shadow-caster alpha cutoff is binary 0.5 while the colour pass uses a Bayer dither (default ON)
`shadow_static_prop.frag:75,80` claims "BYTE-IDENTICAL to static_prop.frag", but `static_prop.frag:291-306` applies a Bayer threshold when `u_glassDither > 0.5`, as does VK colour (`color_prop_tier1.frag:137-149`). Casters and receivers disagree on partial-alpha texels. Right side: unknown. Fix: share one alpha-test helper (M4) or document intent.

### M2. Missing static_asserts / unregistered checks on std140/std430 mirrors
Layouts match when compared by hand, but nothing enforces them:
- No `static_assert(sizeof)`: `ColorFrameUBO` (`vulkan_color_prop_island.cpp:226`, expect 256), `VfxVsParams` (`vulkan_vfx_island.cpp:111`, 96), `WaterVsParams`/`WaterFsParams` (`vulkan_water_island.cpp:218,225`, 80/208), live `ShadowFrameUBO` (`vulkan_shadow_island_live.cpp:217`, 80).
- Not in `tools/check_ubo_layout.py` REGISTRY: MechFrameUBO (docstring mentions it, registry omits it), `LegacyFrameUBO`, `WaterVsParams`, `VfxVsParams`, live `ShadowFrameUBO`, `SkyPC`, `OvlPC`, skirt `.vert/.frag` TerrainFrameUBO copies, color_prop_tier1.vert block, `ScreenFxShadowStaticParams`, skeleton `CompareParams/CsmParams/WaterMvpParams`.
- `TerrainFrameUBO` asserts only total size (1024); offsets live in comments, no per-field offsetof.
- `GpuStaticPropInstance` (112 B) is asserted in C++ only; the two `GpuStaticPropInstanceMirror` structs (`vulkan_color_prop_island.cpp:205`, `vulkan_shadow_island_live.cpp:205`) assert only size; none of the 4 GLSL copies (`static_prop.vert:33`, `shadow_static_prop.vert:19`, `vulkan/color_prop_tier1.vert:100`, `vulkan/shadow_prop.vert:50`) is checked. All 6 agree today.
- Fix: one shared `RenderCore/GpuStaticPropInstance.h`, per-field offsetof asserts, extend check_ubo_layout.py to SSBO structs and the unregistered blocks.

### M3. Vertex stride literals
- `vulkan_color_prop_island.cpp:1175` `vib.stride = 40` (literal; post-upload assert only) vs `vulkan_shadow_island_live.cpp:567` `mc2_static_prop_vertex_stride()` vs `GameOS/gameos/gos_static_prop_batcher_shadow.cpp:785` `kVertexStride = 40`. Use the accessor in the colour island.
- `vulkan_mech_island.cpp:873` `s->vertexStride ? s->vertexStride : 48`: same "stride baked before feed" hazard; the literal fallback masks a 0 stride. Use sizeof(GpuMechVertex) (`gos_mech_batcher.h:24` asserts 48).

### M4. Hand-retyped Bayer / alpha-test blocks
`static_prop.frag:295` and `vulkan/color_prop_tier1.frag:137` (identical, `(i+0.5)/16`). `gos_vegetation_card.frag:44` uses `i/16` with a different row order (third dialect). No VK vegetation/grass shader exists (`gos_vegetation_card.frag`, `gos_grass.*`): a parity gap, not a mismatch. Fix: `shaders/include/prop_shading.hglsl` (proposal 2 in gl-vk-duplication.md).

### M5. MC2_GPU_PARTICLES default read inconsistently
- Default OFF: `mclib/particles/batcher.cpp:80-90` ("REVERTED TO CPU/MLR DEFAULT", absent = disabled), `mclib/gosfx/effect.cpp:45` (`== '1'`), gate table `GuiRuntime/GraphicsOptionsWindow.cpp:211` (false).
- Default ON: `GuiRuntime/GraphicsOptionsWindow.cpp:1903-1907` (`!(penv && penv[0]=='0')`, text "default ON"), comments in `EditorInspector.cpp:1363`, `code/gamecam.cpp:635`, `mclib/gosfx/cardcloud.cpp:512`. The same file contradicts itself (`:211` vs `:1903`).
- Right side: batcher.cpp. Effect: Graphics Options shows "GPU particles: ON" with env unset while GPU FX is off. Fix: export one `is_enabled()` and use it in the UI.

### M6. Hard-coded SH copies
`kWaterSkySh` in `gos_terrain_water_mdi.frag:199` and `vulkan/water_geom.frag:86` equal `RenderCore/IblShCoeffs.h:35` today, but `IblShRegistry.h` supports per-mission sets that props use (`s_currentShSet`) and water ignores. Water sky tint diverges from props on any mission with a custom SH set. Right side: registry. Fix: feed SH via uniform/UBO.

### M7. VK terrain ambient floor uses a binary flat-light share
`vulkan/terrain_tier1.frag:~833` `flatLitShare = pureConcrete ? 1.0 : 0.0` vs GL `terrain_lod_chunk.frag:2174-2183` continuous `cementLightBlend*(1-u_cementLit)`. Differs only on partially blended cement edges. Right side: GL. Fix: feed the real blend.

## LOW / info
- Verified equal across GL, VK and C++: depth biases (`mclib/terrain_depth_bias.h`, `shaders/include/terrain_depth_bias.hglsl`, VK UBO feed); 0.5 alpha cutoffs in all VK frags vs GL; GBuffer1 classes 0.25/0.1875; MC2_SHADOW_CSM_MAX=3 everywhere; shadow PCF/bias/Poisson constants (`lighting_shadow_vk.glsl` vs `shadow.hglsl`, VK shadow port is complete); water wave/reflection constants and SH table; sun-dir feed.
- `mclib/terrain_depth_bias.h:27` comment says TERRAIN_DEPTH_FUDGE = -0.002; value is 0.0 (line 72).
- `vulkan_water_island.cpp:223` p3 comment says "pad, pad, pad"; p3[1] = shoreFlat (set `:1289`), p3[2] = shoreDepthK read by water_geom.vert.
- `isAllConcreteTerrainBatch`: `GameOS/gameos/gameos_graphics.cpp:13` and `mclib/txmmgr_render.cpp:70` are logically identical (`(frgb & 0xFF) == 3` for all vertices; int vs DWORD count). Not diverged; dedupe into a header.
- `MC2_TEXMGR_KTX_PRIMARY`: `mclib/txmmgr_texture.cpp:354` uses `atoi` (modes 0/1/2); `mclib/bdactor_static.cpp:18`, `gvactor.cpp:143`, `mclib/mech3d_render.cpp:50` use `v[0] != '0'`. Differ only on non-numeric values. Harmless.
- `MC2_SP_GLASS_DITHER`, `MC2_CEMENT_ATLAS_SEAM_FIX`, `MC2_OVERLAY_CEMENT_ADOPT`, `MC2_SKYBOX_FOG_EXCLUDE`, `MC2_SHADOW_ROBUST_BASIS` are re-read at several sites, all with the same default-ON idiom today; consolidate into one reader each. My gate scan is heuristic and misses custom wrappers; check-gate-default-agreement.py stays the authority for registered gates.
- `.claude/VK-PARITY-GAPS.md` is STALE: mech FS (row 2), terrain ambient floor (row 3), shadow include sync (row 4) and terrain night pools (row 1) are now ported (UBO fields and shader code present). Still real: VK post visual lacks GL CAS and tonemap-V2 (`postprocess.frag:~180-211` vs `vulkan/postprocess_visual.frag`), gated, default not checked.
