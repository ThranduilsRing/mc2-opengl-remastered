<!-- doc-status: current | reviewed:2026-10-15 -->
# GL retirement inventory (2026-10-15)

WHAT: every GL-only code path that goes when GL is deleted after VK parity (owner ruling: GL is deleted once the Vulkan port is complete),
grouped by subsystem, with the VK replacement that must exist first, the tooling and tests that still lean on the GL path, and a deletion
order. WHY: the deletion is the last step of the VK arc; this list tells each VK slice what it has to leave behind so that the GL file can go
whole, and tells the test owners which oracles must move to VK first.

Method (lane 5, read-only, tree at nifty f418ee189 + lane-5 docs commit): `git grep` for `gl[A-Z]*(` calls per file (comments excluded only
where noted), the VK islands' `extern` feed calls, file headers, `.claude/VK-PARITY-GAPS.md` (rows cited as "row N"),
`.claude/cleanup/vk-default-flip-checklist.md`, `gl-vk-duplication.md`, the lane 2 shadow design (`lane2-state.md` session 19). Nothing was built
or run. Line counts are whole-file `wc -l`.

## 0. Totals and the one structural fact

| | files | lines |
|---|---|---|
| GameOS C++ files that contain GL calls (sections 1-11) | 97 | 65,162 |
| GL-only shaders and includes (`shaders/*` outside `shaders/vulkan`, `shaders/spv`) | 92 | 16,535 |
| **GL footprint in the engine** | **189** | **81,697** |
| outside GameOS: editor GL viewport + thumbnails, `tools/asset_viewer` (13 files, 6,417 lines), `tools/mc2fx_preview`, `tools/mc2weapon_gui`, `ui_editor` (whole files; most hold a few GL calls) | 26 | 19,037 |
| GL-only tooling (section 12) | 11 | 7,724 |

The footprint is an upper bound for deletion: about a third of those lines are CPU code that the VK islands consume today. **VK is not a
standalone backend yet.** Under the umbrella (`MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1`) the GL context still exists and the GL
side is the asset loader and the per-frame producer:
- Feeds the islands pull every frame are defined in GL files: `mc2_color_prop_island_get_{frame,material,light}_feed` and
  `mc2_shadow_island_get_geometry_feed` (`gos_static_prop_batcher_shadow.cpp:692-913`), `mc2_mech_island_get_{geometry,shadow,frame}_feed`
  (`gos_mech_batcher_api.cpp:19-62`), `mc2_terrain_tier1_get_frame_feed` (`gos_terrain_lod_chunk_feed.cpp:39`).
- Those feeds carry GL object names (`glHeightSsbo`, `glColormapTex`, `glCementAtlasTex`, `glCementSsbo`, `glVisualHeightSsbo`,
  `glStaticShadowTex`, `gos_GetGLTextureId`) and the islands copy them into VK memory with `glGetBufferSubData` / `glGetTexImage` /
  `glGetCompressedTexImage` (about 120 call sites in `vulkan_*.cpp`; most are map-load snapshots, the mech light SSBO copy is per frame,
  `vulkan_mech_island.cpp:1110`).
- Every texture is created by the GL texture manager (`gameos_graphics_tparams.cpp`) and reaches VK by readback.
- The fail-soft path `forceGLFallback` (`render_backend_select.cpp`) puts a session back on GL when VK init fails.

So every group below has two halves: the GL DRAW half (deleted when the VK twin is at parity) and the GL-OWNED RESOURCE / FEED half (moved to
CPU or VK ownership first, then deleted). A file marked MIXED needs the split before it can go.

## 1. Shadows (18 files, 4,346 lines)

| file | lines | gl calls | kind |
|---|---|---|---|
| `GameOS/gameos/gameos_graphics_shadow.cpp` | 786 | 167 | GL draw + static light matrix (`gos_ResetStaticLightMatrix`) |
| `GameOS/gameos/gos_postprocess_shadow.cpp` | 1,273 | 214 | GL cascade passes, screen-shadow program, static-map pass |
| `GameOS/gameos/gos_static_prop_batcher_shadow.cpp` | 946 | 65 | MIXED: GL caster draws + the four VK feed producers |
| `shaders/shadow_*` (13 files), `shaders/include/shadow.hglsl` | 1,341 | - | GL only (`shadow_fade_model.hglsl` is shared and stays) |

VK replacement first: `vulkan_shadow_island_live.cpp` (cascades, full-map, dynamic map), `vulkan_shadow_mech_casters.cpp`,
`vulkan_post_screenfx_native.cpp` (object-pixel receiver). Parity rows: **4** (dist fade / sun clamp / terrain guard: MISSING in
`lighting_shadow_vk.glsl`), **17** (building static shadow: MOSTLY FIXED; the static building map is still GL-owned and imported into the
terrain island as `glStaticShadowTex`, a VK producer is required before deletion, lane 2 open item 2), **20** (`ShadowCSM` /
`PropShadowReceive`: PARTIAL). Lane 2 open item 1: VK `ShadowCSM=FALSE` renders broken props, suspected GL state left by the legacy dynamic
pass; it must be understood before the GL dynamic pass goes.
Dependents: goldens `mech_drop_shadows`, `tree_shadows_palm`, `mech_night_shadow_fan`, `snow_shadow_crossings`, `tree_shadows_directional`
(GL-blessed, section 12); `shadow_debug.frag` debug view; `scripts/check-lighting-divergence.py`; `tools/tracy_capture_compare.py` forces
`MC2_VK_SHADOW_INTEROP=0` in its VK arm (measures the no-shadow frame).

## 2. Post-process and sky (26 files, 7,535 lines)

| file | lines | gl calls |
|---|---|---|
| `gos_postprocess.cpp` | 1,185 | 54 |
| `gos_postprocess_effects.cpp` | 871 | 208 |
| `gos_postprocess_fbo.cpp` | 906 | 143 |
| `gos_postprocess_skybox.cpp` | 614 | 140 |
| `gos_postprocess_region.cpp` | 705 | 3 |
| `gos_postprocess_executor.cpp` | 482 | 18 |
| `gos_postprocess_blur.cpp` | 429 | 54 |
| `gos_cluster_depth_pyramid.cpp` (HZB) | 311 | 44 |
| `gos_hdri.cpp` | 302 | 26 |
| `gos_cardcloud_sim.cpp` | 259 | 38 |
| shaders: `postprocess*`, `ssao*`, `hzb_reduce`, `fog_oob`, `edge_fog`, `cloud`, `cardcloud_sim.comp`, `hdri_skybox*`, `skybox`, `cluster_depth_pyramid.comp` | 1,471 | - |

`gos_postprocess_gates.cpp` (348 lines) holds the option readers (`mc2ShadowCsmEnabled`, `mc2PropShadowReceive`) that both backends read:
keep it, move it out of the GL file family. `gos_postprocess_region.cpp` is mostly CPU region math (3 GL calls): split, keep the math.
VK replacement first: `vulkan_post_{fog,ssao,hzb,screenfx}_native.cpp`, the native composite `postprocess_visual.frag`, `vulkan_sky_island.cpp`.
Parity rows: **8** (fog, FIXED), **12** (SSAO / cloud / screen-shadow / HZB opt-in OFF in VK; SSAO-BLUR missing), **19** (CAS / tonemap,
FIXED). Checklist section 1: FORCE-43 / OOB letterbox clear is GL-only (`gos_postprocess.cpp` composite, `gos_Compute43Box`), no VK
equivalent found; VK fog declines when `gos_SkyboxFogExcludeEnabled()` (stencil sky exclusion has no VK path).
Dependents: golden `distant_terrain_horizon_band`, `horizon_waterline`; `shaders/spv/*.json` reflection for `postprocess`, `ssao`,
`fog_oob`, `edge_fog`, `hzb_reduce`, `cloud` (`spirv_reflection` board row).

## 3. Terrain and water (49 files, 24,280 lines; the largest group)

| file | lines | gl calls | kind |
|---|---|---|---|
| `gos_terrain_water_stream.cpp` | 2,332 | 145 | GL water draw |
| `gos_terrain_lighting.cpp` | 1,696 | 142 | GL lighting compute + draw |
| `gos_terrain_lod_chunk_draw.cpp` | 1,362 | 198 | GL draw |
| `gameos_graphics_water.cpp` | 1,095 | 140 | GL draw |
| `gos_terrain_indirect_cement.cpp` | 1,006 | 9 | MIXED: cement atlas producer (fed to VK) |
| `gameos_graphics_overlay.cpp` | 970 | 158 | GL overlay / decal draw |
| `gos_terrain_indirect_atlas.cpp` | 958 | 34 | MIXED: colormap atlas producer (fed to VK) |
| `gos_terrain_patch_stream.cpp` | 887 | 52 | GL |
| `gos_terrain_indirect_mine_decal.cpp` | 862 | 28 | GL |
| `gos_terrain_lod_chunk.cpp` | 797 | 162 | MIXED: shore height / mask textures fed to the VK water island |
| `gos_terrain_indirect_compute.cpp`, `_dispatch.cpp`, `_pack.cpp`, `gos_terrain_indirect.cpp` | 2,565 | 112 | GL GPU-driven path |
| `gos_terrain_lod_chunk_upload.cpp` | 701 | 52 | MIXED: height / visual-height SSBOs (VK snapshot source) |
| `gos_terrain_mask_dispatch.cpp`, `gos_terrain_height_tex.cpp`, `gos_terrain_lod_chunk_matalbedo.cpp`, `_gates.cpp` | 815 | 42 | GL |
| shaders: `terrain_lod_chunk.*`, `terrain_overlay.*`, `gos_terrain*`, `gpu_driven_*`, `shoreline`, `decal`, `box_decal`, four terrain includes | 8,839 | - | GL only |

Not in the table and NOT GL: `gos_terrain_lod_chunk_feed.cpp` (the VK feed; rewrite to hand VK-owned buffers instead of GL names).
VK replacement first: `vulkan_terrain_island*.cpp` (tier1 + skirts + overlay + decal shaders), `vulkan_water_island*.cpp`. The island
header still says LOD0 main draw only with coarser bands / stitch / visual-displace deferred (`vulkan_terrain_island.cpp:30-31`) and the
CPU LOD0 chunk selection stays on the GL side: confirm the current scope before planning the deletion.
Parity rows: **1** (night light pools / grid / cone, MISSING), **3** (ambient floor), **5** (cement night, PARTIAL), **6** (overlay sidecar /
decal pass, MISSING), **7** (water shore edge), **9** (macro variation / control map / geomorph, PARTIAL), **13** (POM, FIXED), **14**
(cliff material, PARTIAL, MIXED oracle).
Dependents: about 15 of the 25 golden regions are terrain / water / shore (`shore_*`, `map_edge_skirt`, `snow_terrain_seams`, `pad_edge`,
`cement_pads_dark`, `light_pools_road`, `night_base_lighting`, ...); `mc2-terrain-indirect` parity counters (`gos_push_overlay_calls`,
`indirect_overlay_packed_quads`, `legacy_solid_setup_quads`) and the `tessDebug` / `MC2_TERRAIN_DEBUG_MODE` debug views live in GL code;
`scripts/check-terrain-daylight-constants.py` reads GL shaders; `check-editor-game-render-context-parity.py` (the editor renders terrain
through the GL path).

## 4. Static props, culling, light grid, vegetation (32 files, 17,189 lines)

| file | lines | gl calls | kind |
|---|---|---|---|
| `gpu_cull_compute.cpp` / `gpu_cull_readback.cpp` / `gpu_cull_substrate.cpp` / `gpu_driven_common.cpp` | 3,077 | 247 | GL compute cull |
| `gos_static_prop_batcher_submit.cpp` | 1,201 | 17 | MIXED: CPU bucket build |
| `gos_static_prop_batcher_finalize.cpp` | 1,054 | 62 | MIXED: map-load geometry (VK reads its VBO / IBO back) |
| `gos_static_prop_batcher_seam.cpp` | 1,040 | 15 | MIXED |
| `gos_static_prop_batcher_finalize_textures.cpp` | 930 | 86 | MIXED: material arrays (VK reads them back) |
| `gos_static_prop_batcher_state.cpp` | 887 | 7 | MIXED: option readers both backends use |
| `gos_static_prop_batcher_flush_dispatch.cpp` / `_flush.cpp` / `_flush_prep.cpp` | 2,185 | 172 | GL draw |
| `gos_static_prop_batcher_program.cpp` / `_mapload.cpp` / `gos_static_prop_batcher.cpp` | 1,522 | 117 | GL |
| `gos_lightgrid_build.cpp` | 585 | 98 | GL compute (terrain light grid, row 1 input) |
| `gos_materials.cpp` | 536 | 34 | MIXED |
| `gos_vegetation.cpp` | 515 | 87 | GL draw |
| shaders: `static_prop*`, `building_pbr*`, `gpu_cull*`, `lightgrid_build.comp`, `gos_vegetation_card*`, `lighting.hglsl`, `pbr_common.hglsl` | 3,497 | - | GL only |

VK replacement first: `vulkan_color_prop_island.cpp`, `vk_shared_light_feed.cpp`; a VK cull (none exists: whether the colour feed's visible
set depends on the GL compute cull was not established here) and a VK light-grid build (needed by row 1 anyway).
Parity rows: **11** (vertex drift, residual unverified), **15** / **16** (FIXED), **18** (decal fill: NOT PORTED, no stock content shows it),
**22** (PBR / ambient, FIXED). `gl-vk-duplication.md` section 2: `GpuStaticPropInstance` is now generated (`gpu_abi_static_prop_instance.gen.hglsl`,
shared, stays).
Dependents: golden `compound_light_wedges`, `searchlight_cones_compound`, `ghost_*`; `tests/shader_variants/static_prop.frag.splice-regression`;
`scripts/check-light-abi-lockstep.py`, `check-material-gpu-mirror.sh`, `check-split-path-contracts.py` (GL halves of the mirrors).

## 5. Mechs (6 files, 4,034 lines)

| file | lines | gl calls | kind |
|---|---|---|---|
| `gos_mech_batcher_flush.cpp` | 1,275 | 161 | MIXED: GL draw + the shadow snapshot VK casters use |
| `gos_mech_batcher.cpp` | 1,038 | 45 | MIXED: skinning / instance build |
| `gos_mech_batcher_geometry.cpp` | 592 | 33 | MIXED: geometry the VK island reads back |
| `gos_mech_batcher_api.cpp` | 416 | 6 | the three VK feeds (keep, re-home) |
| `shaders/mech.vert`, `mech.frag` | 713 | - | GL only (`mech_shading.hglsl` is shared) |

VK replacement first: `vulkan_mech_island.cpp`, `vulkan_shadow_mech_casters.cpp`. Parity rows: **2** (StandardLit / ORM / AO, MISSING; partly
4f076ce91), **21** (`SunAxisFix`, FIXED). The VK mech island still copies the light SSBO from GL every frame (`vulkan_mech_island.cpp:1110`)
and has a VBO / IBO readback fallback (`:913-918`).
Dependents: `scripts/mech_bone_parity.py`; `shaders/spv/mech.*.json` reflection (8 files); goldens `mech_drop_shadows`,
`mech_night_shadow_fan`.

## 6. VFX (10 files, 2,308 lines)

| file | lines | gl calls |
|---|---|---|
| `gos_particle_bridge.cpp` | 848 | 109 |
| `gos_particle_bridge_tube.cpp` | 534 | 68 |
| `gos_vfx_mesh_bridge.cpp` | 337 | 75 |
| shaders `particle_billboard*`, `tube_ribbon*`, `vfx_mesh*`, `particles.hglsl` | 589 | - |

VK replacement first: `vulkan_vfx_island.cpp` (RAII island, lane 2). No parity row in VK-PARITY-GAPS; the queue's "G VFX parity oracle"
and item 13 (GPU particles parity, then MLR retirement) are the gate. The MLR legacy loops are drawn under VK by
`vulkan_legacy_shape_island.cpp` (row 10, FIXED), so MLR retirement is independent of the GL deletion.
Dependents: `tools/vfx_parity_oracle.py` refuses anything but `{"requested": "gl", "drawn": "gl", "readback": "gl_fbo"}` (line 98);
`scripts/capture_vfx_oracle.py`, `tools/test_capture_vfx_oracle.py`, `scripts/check-oracle-registry.py`. `tools/vfx_oracle/` is out of
lane 5's scope and was not read.

## 7. Present, HUD and the GL renderer core (34 files, 16,142 lines)

| file | lines | gl calls |
|---|---|---|
| `gameos_graphics_bridge.cpp` | 1,255 | 376 |
| `gameos_graphics_renderer_internal.h` | 1,276 | 152 |
| `gameosmain.cpp` / `gameosmain_frame.cpp` (SDL GL context, frame loop) | 2,288 | 56 |
| `utils/shader_builder.cpp` | 1,550 | 66 |
| `gos_render.cpp` | 1,164 | 11 |
| `gameos_graphics_draw.cpp` (immediate draws, HUD batch) | 1,154 | 47 |
| `gameos_graphics_hud.cpp` | 1,060 | 2 |
| `gameos_graphics.cpp` | 933 | 52 |
| `gameos_graphics_capi.cpp` | 929 | 76 |
| `gameos_graphics_material_internal.h` | 851 | 21 |
| `utils/gl_utils.{cpp,h}` | 825 | 189 |
| `gameos_graphics_light_ssbo.cpp` | 487 | 48 |
| `gameos_graphics_internal.h` | 455 | 45 |
| `gameos_graphics_font.cpp` | 258 | 6 |
| `utils/gpu_ring_buffer.h`, `gos_gpu_sync.*`, `pipeline_binder.*`, `view_uniforms_gl.cpp`, `gpu_buffer_gl.cpp`, `gos_render_context.*`, `GLBackend.*` | 1,077 | 98 |
| shaders `gos_vertex*`, `gos_tex_vertex*` | 530 | - |

Most of these are the `gos_*` API implementation the game calls (`gos_DrawQuads`, `gos_SetRenderState`, text, HUD); the API stays, its GL
body goes. VK replacement first: `engine_present_vk.cpp` (swapchain, HUD tail `queueHudVulkanFrame`, ImGui on `imgui_impl_vulkan`),
`vulkan_legacy_shape_island.cpp` (legacy `gos_VERTEX` loops), a VK window / context that does not borrow an SDL GL window.
Open per checklist section 1 and 4: in-scene letterbox / fade quads are not `IsHUD` and land in the discarded GL FBO under VK; FORCE_43
pillarbox; HUD / menus / cursor / briefing / video (`MC2_VK_HUD_DRAW_DUMP`, no `[VK_HUD_SKIP_DECLINE]`); fail-soft to GL must be replaced
by a VK-only failure policy (owner decision, section 13).
Dependents: tier1 smoke runs GL by default (no backend in the run env); `scripts/check-no-raw-gl-from-game.sh` and its allowlist,
`RenderCore/IRenderBackend.h`, `GpuBuffer.h`, `frame_executor.h` (GL tokens in the abstraction headers, 14 + 4 + 4 calls or names).

## 8. Textures (1 file, 920 lines, plus the readbacks)

`gameos_graphics_tparams.cpp` (920 lines, 123 GL calls) creates every engine texture and the static light matrix (`gos_StaticLightMatrixBuilt`).
VK has no texture loader: the islands read GL textures back (`glGetTexImage` / `glGetCompressedTexImage` in `vulkan_color_prop_island.cpp`,
`vulkan_terrain_island_images.cpp`, `vulkan_water_island_images.cpp`, `vulkan_sky_island.cpp`, `vulkan_legacy_shape_island.cpp`, via
`gos_GetGLTextureId`). Also GL-owned: `gos_static_prop_batcher_finalize_textures.cpp` (section 4) and `gos_terrain_lod_chunk_matalbedo.cpp`
(section 3).
VK replacement first: a VK texture manager behind the `mcTextureManager` handles (upload from the decoded image, BC7 / KTX2 kept, VMA
owned). No parity row covers it; it is a prerequisite of sections 3, 4, 5, 7.
Dependents: `editor/AssetThumbnailCache.cpp`, `ui_editor/UiEditorImageCache.cpp` (GL texture ids in the editor).

## 9. GL state guard (1 file, 427 lines + tests)

`GameOS/gameos/gl_state_guard.h` (427), `tests/unit/test_glstate_guard_harness.cpp` (418, 133 GL calls), `scripts/raw-gl-state.allowlist.json`
(127), `scripts/tests/test_raw_gl_firewall.py`, the `no_raw_gl_from_game` board row. VK replacement: none needed (VK state is in pipeline
objects). Goes with section 7.

## 10. Debug views and diagnostics (10 files, 3,519 lines)

| file | lines | gl calls |
|---|---|---|
| `gos_object_parity.cpp` (+ `.h` 350) | 682 | 7 |
| `gos_dev_shell.cpp` | 552 | 4 |
| `gos_postprocess_diag.cpp` | 464 | 9 |
| `debug_renderer.cpp` (+ `debug_prim.*` shaders) | 415 | 65 |
| `gos_render_pass_timer.cpp` (GL timer queries) | 285 | 12 |
| `mc2_hitch_trace.h` | 273 | 12 |
| `gos_validate.cpp` | 243 | 4 |
| `gpu_memory_report.cpp` (GL_EXT_memory_object LUID) | 231 | 10 |
| `gos_renderer_input_probe.cpp` | 188 | 8 |
| `gos_frame_pass_stats.cpp` | 186 | 6 |

Most are diagnostics with a few GL queries: port the query (VK timestamps exist: `vk_gpu_timestamps.h`) and keep the tool, or delete the
view. `debug_renderer.cpp` (lines, boxes) needs a VK pipeline if the editor and the dev shell keep it. Terrain debug modes are in section 3.
Decision needed per view (section 13).

## 11. Capture (2 files, 997 lines)

`gos_visual_capture.cpp` (947) reads back `gl_fbo` or `vk_present` and writes the sidecar `backend` / `readback_source`; delete the `gl_fbo`
arm only. `gos_screenshot.cpp` (50, GL readback): port to the VK present readback.

## 12. Tooling and tests that depend on a GL path

| item | what it needs from GL | replace with, before deletion |
|---|---|---|
| goldens `tests/visual/goldens/golden-e9e3f19d6-1080p` (25 regions, 10 poses) | blessed from GL captures (`capture_env.backend = {requested: gl, drew: [gl]}`); `reset-50331d5` is GL as well | a VK bless (`bless.py` on VK captures, three runs), then retire the GL sets per `RETIRED.md` |
| the parity oracle of every VK row (lane 2 method: same-exe GL capture, `MC2_SMOKE_FREEZE_ANIM=1`) | a GL arm in the same exe | VK goldens + `verify.py`; the oracle ends when GL ends, so every MISSING / PARTIAL row must close before |
| `tools/visual_verify/capture_ab.py` (GL twice + VK) | two GL arms as the noise floor | VK twice as the noise floor |
| `tools/vfx_parity_oracle.py`, `scripts/capture_vfx_oracle.py` | `gl_fbo` readback only | a VK readback arm |
| `scripts/check-gl-shader-variants.py`, `extract_gl_shader_variants.py`, `gl_shader_variants.json` (5,757 lines) | GL shader sources | delete with the shaders |
| `scripts/check-shader-variant.py`, `tests/shader_variants/*` | GL prefix splice | delete |
| `tools/glsl_port_diff.py`, `tools/gl_vk_feature_coverage.py`, `tools/vk_not_ported.allowlist`, `scripts/tests/test_gl_vk_feature_coverage.py` | GL as the reference | delete after the last row closes |
| `shaders/spv/*.json` + `check-spirv-reflection-contract.py` / `check-spirv-artifacts.py` | reflection of GL shaders | keep for VK SPIR-V only |
| `check-lighting-divergence.py`, `check-split-path-contracts.py`, `check-light-abi-lockstep.py`, `check-terrain-daylight-constants.py`, `check-material-gpu-mirror.sh` | GL halves of the lockstep pairs | reduce to the VK half / the generated ABI |
| tier1 / tier2 smoke | GL default backend | run on the VK default (checklist step 10), then drop the GL leg |
| `tools/tracy_capture_compare.py` | GL arm; its VK arm forces `MC2_VK_SHADOW_INTEROP=0` | VK only, shadows on |
| editor smoke (`tests/smoke/editor/old-editor-2026-10-13`) | the MFC editor's GL viewport | the editor rework's VK viewport (target architecture 2.10) |
| `tools/asset_viewer` (13 files, 6,417 lines, 444 GL calls), `mc2fx_preview`, `mc2weapon_gui` | own GL contexts | port or retire: decision (section 13) |

## 13. Proposed deletion order

Preconditions for any step: the VK default flip is done (checklist section 4, steps 1-15), VK goldens are blessed and `verify.py` passes on
VK, and the owner has ruled on the fail-soft policy. Each step deletes whole files where it can, and is its own batch with its own proof
(build, `build_all_targets`, doctest, board, tier1 + tier2 on VK, VK captures viewed day and night).

1. **Capture and tooling GL arms** (section 11, the GL rows of section 12 that only compare): after the VK goldens exist. Small, removes the
   habit of proving VK against GL.
2. **Debug views and diagnostics** (section 10): port the few kept ones to VK timestamps / pipelines, delete the rest. Independent of parity.
3. **Post-process and sky** (section 2): the VK native post stack is already the umbrella path; close row 12 for the options kept and the
   FORCE-43 / sky-fog-exclude gaps, then delete. Keep `gos_postprocess_gates.cpp` and the region math, re-homed.
4. **VFX** (section 6): after the VFX parity oracle runs on VK.
5. **Shadows** (section 1): after rows 4 and 20 close and the static building map has a VK producer (row 17); move the four prop feeds out of
   `gos_static_prop_batcher_shadow.cpp` first.
6. **Mechs** (section 5): after row 2; move the skinning / instance / shadow snapshot into a backend-neutral producer, delete the GL draw.
7. **Props, cull, light grid, vegetation** (section 4): VK cull and VK light-grid build first (the light grid also unblocks row 1); keep the
   CPU bucket build, delete the GL draw and compute.
8. **Terrain and water** (section 3): last of the scene passes, because it carries most of the open rows (1, 3, 5, 6, 7, 9, 14) and the editor
   still draws terrain through it; move height / visual-height / cement / colormap producers to VK-owned buffers, rewrite the terrain feed.
9. **Textures** (section 8): when no island calls `glGetTexImage` / `glGetBufferSubData` any more (a grep row can enforce zero), the GL texture
   manager goes and the VK texture manager owns every handle.
10. **Present, renderer core, state guard** (sections 7 and 9): the GL context, GLEW, `shader_builder`, `gl_utils`, `gl_state_guard`, the
    `gos_*` GL bodies and the GL shaders that remain; the editor and the in-repo tools must be off GL by then (editor rework VK viewport;
    asset_viewer decision). The second VK best-practices review (queue item 14) is the acceptance review for this step.

## 14. Open questions (not decided here)

1. DECIDED 2026-10-16 (owner): with GL gone, a failed VK init shows a clear message and exits; no fallback. `forceGLFallback` and the smoke's
   `capture_backend` mismatch check exist only because of the old fail-soft and go with GL.
2. `tools/asset_viewer`, `mc2fx_preview`, `mc2weapon_gui`: port to VK or retire?
3. Which debug views survive (`debug_renderer`, terrain `tessDebug` modes, `gos_object_parity`, `gos_dev_shell`)?
4. Does the VK colour-prop feed depend on the GL compute cull's visible set (section 4)? Not established by reading; decides whether step 7
   needs a VK cull before or with the GL deletion.
5. Current scope of the VK terrain island (LOD0 only per its header, or all bands); decides how much of section 3 is a port, not a deletion.
6. Row 18 (decal fill) has no stock content: port blind, or delete the GL feature with GL?

## 15. Step 1 status (lane 2 sessions 29-30, 2026-10-16)

| item | state |
|---|---|
| CURRENT_GOLDEN | switched to `golden-d9e1f3801716-vk-1080p`; the two GL golden sets archived and removed |
| `capture_ab.py` noise floor | two VK arms; GL arm behind `--gl-arm` (kept while a GL exe ships) |
| Tracy compare GL arm | DONE (session 30, f4d0204b4): every subcommand measures VK; `selfcheck` proves sensitivity with 1080p vs 4K (33.0% p50 apart on mc2_10; the old `MC2_LIGHT_POOL_MAX` 64 vs 256 pair moves the VK frame only 1.3%), refuses a run without the VK receipt or with a fail-soft; `game_env` refuses any other arm |
| VFX oracle GL arm | NOT DONE: `capture_vfx_oracle.py` runs `MC2_RENDER_BACKEND=gl` and compares MLR vs GPU particles; it is the proof the MLR retirement waits on, and no VK readback arm exists. Needs the VK arm (not a deletion) |
| GL-reference provers (`glsl_port_diff`, `gl_vk_feature_coverage`, `vk_not_ported.allowlist`) | NOT DONE: rows 3, 5-22 and 24 of `.claude/VK-PARITY-GAPS.md` are still open and these provers are what pins them; delete after the last row closes |
| capture `gl_fbo` arm (`gos_visual_capture.cpp`, `gos_screenshot.cpp`) | STOPPED (session 30): it is the only readback of a GL-backend frame, and the same-exe GL capture is still the oracle of every open parity row (3, 5-22, 24: row 24 used it this session) and of the VFX oracle. Goes with the last GL capture consumer |
| 10 debug views | STOPPED (session 30), each has a live reader or a ruling: lost-draws + `MC2_VK_SCENE_GL_PRODUCER_QUERY` (KEEP by orchestrator ruling 2026-10-12: the proof that no GL draw reaches the VK scene before the GL interop goes; a full deletion was made and reverted); `gos_render_pass_timer` + `gos_frame_pass_stats` (editor Frame Inspector, dev-shell `framegraph`); `gos_dev_shell` (`tools/dev_shell`, `mission_order_ingress_witness.py`); `debug_renderer` (KEEP-LIVE, viewmode matrix + inspector toggle); HZB probe in `gos_postprocess_diag` (KEEP-LIVE, `hzb.json`); `gpu_memory_report` (`smoke_lib/vram.py`); `gos_validate` (`runner.py` `MC2_GL_ERROR_DRAIN_SILENT`); `gos_renderer_input_probe` (UI probe commands); `gos_object_parity` (checks the GPU static-prop producer the VK prop island also consumes); `mc2_hitch_trace` (hitch attribution, no VK equivalent). Each needs a VK port or an owner decision (section 14 question 3) |
| `golden-e350d0b4a5c6-vk-1080p` | DONE (session 30, 91c1e0502): archived (tarball + sha256) and removed, RETIRED.md row |
