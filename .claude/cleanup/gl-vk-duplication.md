<!-- doc-status: current | reviewed:2026-10-11 -->
# GL <-> VK duplication map and consolidation plan
Read-only survey (no builds). Evidence is from file headers and greps; not every claim was re-verified line by line.

## 1. Island -> GL twin
| VK island (GameOS/gameos) | VK shaders (shaders/vulkan) | GL twin shaders | GL twin C++ | Gate (default OFF) |
|---|---|---|---|---|
| vulkan_color_prop_island | color_prop_tier1.vert/.frag | static_prop.vert/.frag, static_prop_depth.frag | gos_static_prop_batcher.cpp/.h | MC2_VK_PROP_COLOR_TIER1 |
| vulkan_shadow_island(_live) | shadow_prop.*, csm_array_shadow, compare_shadow, screen_shadow_csm/static | shadow_static_prop.*, shadow_depth, shadow_screen.frag | gos_static_prop_batcher.cpp, shadow code | not confirmed |
| vulkan_mech_island | mech_tier1.* | mech.*, shadow_mech.vert | tgl.cpp light feed | MC2_VK_MECH_TIER1 (needs SHARED_SCENE_DEPTH) |
| vulkan_terrain_island | terrain_tier1(+_skirt,_overlay,_decal).* | terrain_lod_chunk.*, gos_terrain*.vert, terrain_overlay.* | gos_terrain_lod_chunk.cpp, gos_terrain_indirect.h | MC2_VK_TERRAIN_TIER1 |
| vulkan_legacy_shape_island | legacy_shape_tier1.* | gos_vertex(_lighted), gos_tex_vertex(_lighted) | gameos_graphics*.cpp | not confirmed |
| vulkan_water_island | water_geom/water_mvp.* | gos_terrain_water_*, shoreline.frag | gos_terrain_water_stream.cpp | MC2_VK_WATER_* (perf/debug seen) |
| vulkan_sky_island | hdri_sky.* | hdri_skybox.*, skybox.* | | not confirmed |
| vulkan_vfx_island | vfx_particle.* | particle_billboard.*, vfx_mesh.*, tube_ribbon.* | | MC2_VK_VFX_* |
| vulkan_post_*_native | fullscreen, postprocess(_visual), ssao*, hzb_reduce, fog_oob, edge_fog | postprocess.*, ssao*.frag, hzb_reduce.frag, fog_oob/edge_fog.frag | gos_postprocess.* | |

Backend select: render_backend_select.cpp. MC2_RENDER_BACKEND unset gives GL, so GL is the default backend. Each island also has its own default-OFF tier1 gate.

## 2. Structs that must stay in lockstep
- GpuStaticPropInstance (112 B) exists in 7 places: the C++ definition (gos_static_prop_batcher.h, with static_asserts and offsetof), two hand-copied C++ `GpuStaticPropInstanceMirror` structs (vulkan_color_prop_island.cpp:205, vulkan_shadow_island_live.cpp:205), and GLSL copies in static_prop.vert, shadow_static_prop.vert, vulkan/color_prop_tier1.vert and vulkan/shadow_prop.vert. Only comments say "LOCKSTEP". Nothing compares the GLSL layouts to the C++ one.
- Same pattern elsewhere: ObjLightsMirror (color-prop island) vs TG_HWLightsData; the 40 B static-prop vertex stride (literal `vib.stride = 40`); terrain UBO byte offsets (comments "off 212/244/688/700" in terrain_tier1.frag) mirrored by hand in C++; PerDrawEntry (32 B); MaterialGpu; shadow knobs (shadowDistFadeWu etc.) duplicated from GL #defines.
- Existing checks (scripts/):
  - check-light-abi-lockstep.py: lighting.hglsl, tgl.h, light SSBO stride. Does not cover the VK mirrors.
  - check-material-gpu-mirror.sh: MaterialGpu field order GLSL vs C++.
  - check-split-path-contracts.py: colour swizzle, depth-bias mirrors, sibling families.
  - check-terrain-daylight-constants.py: night constants.
  - check-spirv-reflection-contract.py, check-vulkan-bindings.py: bindings and slots.
  - check-lighting-divergence.py: pins the divergence, does not remove it.
  - check-shader-variant.py: depth/color prefix, GL only.
  - Nothing checks VK-vs-GL math parity, VK feed completeness, or pipeline stride vs feed.

## 3. Hand-duplicated logic (GL vs VK)
- Glass/tree dither: Bayer 4x4 in static_prop.frag:291 is re-typed in color_prop_tier1.frag:137.
- NdotL: GL uses abs(dot(N,sun)) (static_prop.vert:732); VK re-types its own vertex-light path.
- Fog/haze mix: re-typed in mech_tier1.frag, legacy_shape_tier1.frag and the GL equivalents.
- Shadow sampling: GL shadow.hglsl cascade select is "ported VERBATIM" into vulkan/include/lighting_shadow_vk.glsl and terrain_tier1.frag. Only shadow_fade_model.hglsl is shared.
- Already shared via `#include` of shaders/include/*.hglsl: noise, mech_shading, object_light_pool, terrain_light_pool, edge_haze, water_shore_edge, terrain_depth_bias, shadow_fade_model.
- Not shared: dither, abs(NdotL)/vertex-light, haze mix, cascade select, glass alpha, GBuffer1 shadow-class encoding.

## 4. Consolidation proposals, ranked by bug-risk reduced
1. **One shared instance definition.** Add `shaders/include/static_prop_instance.hglsl` (the GLSL block) and `RenderCore/GpuStaticPropInstance.h` (the C++ struct plus static_asserts), and delete the two `*Mirror` structs. Add a check-instance-layout.py that parses the hglsl and compares it with the offsetof table, in the style of check-material-gpu-mirror. Put `kStaticPropVertexStride = 40` in the same header and static_assert the feed against it.
   - Would have prevented: "pipeline stride baked before feed". The literal `vib.stride = 40` and the earlier stride=0 bug (PROPS-INVISIBLE comment) both go away, because the pipeline and the feed read one constant.
2. **Shared `shaders/include/prop_shading.hglsl`** with `bayer4x4Threshold()`, `propNdotL()` (abs) and `hazeMix()`, included by static_prop.* and color_prop_tier1.*. VK shaders already use GL_GOOGLE_include_directive, so no toolchain change is needed.
   - Would have prevented: "abs(NdotL) missing in VK" and "tree dither missing in VK". Each fix would land once and apply to both backends.
3. **Feed-completeness contract.** A table of bucket kinds (static, dynamic, alpha-test, tree) that GL draws, each needing a producer call into `Mc2ColorPropFeed`, plus check-vk-feed-coverage.py.
   - Would have prevented: "dynamic buckets not fed" (a bucket kind with no feeder fails the check). It also catches a missing tree-dither feed flag.
4. **Merge lighting_shadow_vk.glsl into shadow.hglsl.** Put only the resource declarations behind `#ifdef MC2_VULKAN`, so the cascade math becomes single-source. Highest effort, medium risk, because sampler declarations differ.
5. **Extend check-split-path-contracts.** Require each tier1 shader to `#include` prop_shading.hglsl or carry a `// LOCKSTEP-MIRROR:` marker with a pinned value. Cheap guard against future re-typing.

## 5. GL paths deletable once VK is default
Nothing is deletable now: GL is the default backend and every tier1 gate is default-OFF. Delete only after the gate is flipped, a golden run passes, and the GL-fallback (forceGLFallback in render_backend_select.cpp) is decided.
- GL static-prop color/depth (static_prop.*, batcher draw paths). Gate: MC2_VK_PROP_COLOR_TIER1 default-ON plus the shadow island live.
- GL mech (mech.*, shadow_mech.vert). Gate: MC2_VK_MECH_TIER1.
- GL terrain LOD chunk (terrain_lod_chunk.*, gos_terrain*.*). Gate: MC2_VK_TERRAIN_TIER1. Largest block; the editor path may still use it (see check-editor-game-render-context-parity.py).
- GL post, sky, vfx, water last.
- scripts/check-gate-default-agreement.py and check-new-gates.py would need the new defaults registered.

## Caveats
- Gates for the shadow, legacy-shape and sky islands were not confirmed.
- The editor's dependence on GL was not checked.
