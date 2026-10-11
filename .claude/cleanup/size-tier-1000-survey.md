# Size-tier survey: files of 1,000-1,500 lines and the remaining >1,500 (2026-10-11, tip 684db17f8)

Method: committed tree, the file-size ratchet's file set (1,833 files in 11 source dirs); verdicts come from a per-file
function census (function count, largest function), not from reading every file. Survey only, nothing edited.
Verdicts: CHEAP = verbatim split along clear sections; PHASE = one dominant function, needs a phase-helper pass;
LEFT = cohesive class / table / vendored header, a split would only add include churn.

## Counts
- 1,000-1,500 lines: **111 files** -- code 32, mclib 32, GameOS 27, editor 12, gui 3, shaders 2, GuiRuntime 1, RenderCore 1, RenderWorld 1.
- over 1,500: **61 files** (was 64) -- code 23, mclib 22, GameOS 11, GuiRuntime 1, RenderCore 1, editor 1, gui 1, shaders 1.

## Top 20 of 1,000-1,500
1. code/objmgr_update.cpp 1498 -- LEFT: just phase-split, largest helper is 546 lines.
2. GameOS/gameos/gos_terrain_lod_chunk_draw.cpp 1497 -- LEFT: just phase-split, helpers at most 279 lines.
3. RenderCore/bcdec.h 1489 -- LEFT: vendored single-header BCn decoder.
4. mclib/txmmgr.h 1470 -- LEFT: class declarations (10 types), splitting fans out includes.
5. GameOS/gameos/gos_terrain_indirect_dispatch.cpp 1466 -- PHASE: ComputeDispatch is 924 lines (63%).
6. mclib/elist.h 1463 -- LEFT: three container declarations.
7. code/mission_update.cpp 1458 -- PHASE: Mission::update is 984 lines.
8. GameOS/gameos/gos_particle_bridge.cpp 1439 -- CHEAP: 49 functions, flush (450) is the only big one.
9. editor/EditorData_terrain.cpp 1416 -- PHASE: initTerrainFromPCV 550 of 12 functions.
10. GameOS/gameos/vulkan_water_island.cpp 1414 -- CHEAP: 21 functions, init / run / destroy groups, none over 305.
11. mclib/quad_draw.cpp 1413 -- LEFT: the two legacy branches (417 / 370) are copy-paste twins; dedup, do not split.
12. mclib/mlr/mlr_i_pmesh.cpp 1403 -- LEFT: one MLR class, 23 methods.
13. code/contact.cpp 1398 -- LEFT: 58 small methods of one class.
14. mclib/ablscan.cpp 1395 -- LEFT: the ABL lexer, cohesive.
15. code/mech_loadout.cpp 1390 -- PHASE: 3 functions, init is 630 lines.
16. mclib/soundsys.cpp 1377 -- LEFT: one class, update 267 is the largest.
17. mclib/tgl_transform.cpp 1374 -- PHASE: MultiTransformShape is 1,018 lines (74%).
18. code/objmgr.cpp 1367 -- LEFT: already the remainder of a four-way split.
19. GuiRuntime/GuiRuntime.cpp 1365 -- CHEAP: 53 functions, one panel per group.
20. editor/EditorObjects.cpp 1365 -- CHEAP: 41 functions, grouped by object kind.

## The 61 files over 1,500
- PHASE (a dominant function; ratio = function / file): gameosmain.cpp (main 1,798 / 2,811), gos_static_prop_registry.cpp (flush 1,030 / 2,207),
  code/missionbegin.cpp (update 976 / 2,132), mlr/mlrlightmap.cpp (DrawLightMaps 1,240 / 1,599), code/saveload.cpp (load 1,054 / 1,683),
  GuiRuntime/EditorInspector.cpp (drawImGui 1,294 / 1,652), code/gamecam.cpp (render 816 / 1,564), code/mpparameterscreen.cpp (update 660 / 2,415),
  gos_terrain_water_stream.cpp (ComputeDispatchAndBindThinRecords 643 / 2,455), gpu_cull_compute.cpp (compute_dispatch 567 / 1,557),
  mlr/mlrindexedpolymesh.cpp and mlrpolymesh.cpp (Clip 796 and 766), code/tacordr.cpp (status 557 / 2,072), code/infowindow.cpp (renderImGui 517 / 1,732).
- CHEAP (many functions, section banners): code/brain_special_dispatch.cpp (66 funcs, 43 banners), code/warrior.cpp, mclib/file.cpp, mclib/mapdata.cpp,
  code/objective.cpp, code/gameobj.cpp, code/turret.cpp, code/logisticsdialog.cpp, mclib/gosfx/fcurve.cpp, code/mechicon.cpp, code/mechlabscreen.cpp,
  code/pilotreviewarea.cpp, code/optionsarea.cpp, code/artlry.cpp, code/terrobj.cpp, code/bldng.cpp, code/mechlopedia.cpp, mclib/terrtxm.cpp,
  mclib/assimp_importer.cpp, mclib/render_contract.cpp (85 small funcs), mclib/gosfx/tube.cpp, GameOS/gameos/engine_present_vk.cpp (52 funcs),
  vulkan_color_prop_island.cpp, vulkan_shadow_island_live.cpp, gos_terrain_lighting.cpp, utils/shader_builder.cpp.
- LEFT, cohesive implementation: mclib/heap.cpp (allocator, USE_GOS_HEAP regions), ablrtn.cpp / ablenv.cpp (ABL interpreter), stuff/rotation.cpp (math),
  estring.cpp / echarstring.cpp / ewcharstring.cpp (three near-twin string classes: dedup is the win, Format is ~290 lines in each), gui/alistbox.cpp,
  editor/ObjectiveDlg.cpp (one dialog class).
- LEFT, headers and tables: GameOS/include/gameos.hpp (public API, 3,753), RendererFeatureRegistry.h (2,917, registry table), code/warrior.h, mover.h,
  multplyr.h, mclib/move.h, camera.h, tgl.h (class declarations), GameOS/include/imagehlp.h (vendored SDK header), mlr/mlrtriangleclipping.hpp and
  mlrprimitiveclipping.hpp (macro-expanded clipping bodies).
- shaders/terrain_lod_chunk.frag (2,412): split along its hglsl include sections; needs the shader-reflect and variant-manifest gates, moderate cost.

## Single functions still over 700 lines (the phase-helper list is not empty after all)
gameosmain.cpp main 1,798; mlrlightmap DrawLightMaps 1,240; EditorInspector drawImGui 1,294; saveload load 1,054; gos_static_prop_registry flush 1,030;
tgl_transform MultiTransformShape 1,018; Mission::update 984 (mission_update.cpp) and missionbegin update 976; gos_terrain_indirect_dispatch ComputeDispatch 924;
gamecam render 816; mlr Clip 796 / 766; brain_special_dispatch executeSpecialBody_Apply 758.

## Appendix: whole-tier census (lane 2, 2026-10-10, tip aa9130de3)

Generated by `py -3 tools/split/tier_census.py` (stdlib + `tools/split/cpp_units.py`; rerun it instead of editing this table). It covers every file of 1,000-1,500 lines in the ratchet's file set,
not only the top 20 above. Files already split by lane 2 in this pass (abldbug, genactor, logisticsmissioninfo, logisticsvariant) are below 1,000 now and no longer listed.
Verdict is a heuristic: PIECE = a sibling `<stem>_internal.h` exists (already a split piece; PIECE+PHASE when one function is over 55%), PHASE = one function over 55%,
CHEAP = 12+ functions and none over 35% (worth a look, not proof that a clean cut exists), MID = fewer functions, LEFT = header / shader / no functions.
Where the survey above already gave a verdict it wins: contact, ablscan, soundsys, estring and mlr_i_pmesh read CHEAP here but are LEFT there (one cohesive class / lexer / twin-string dedup).
Remaining real candidates in code/ and mclib/ after the lane-2 splits: cevfx, mechbayscreen, group, team, userinput, cone_glow, ablxstd, mlr_i_tmesh, multplyr (148 tiny stubbed-network methods).

| File | Lines | Funcs | Largest fn | Other units | Verdict |
|---|---:|---:|---:|---:|---|
| `GameOS/gameos/gos_terrain_lod_chunk_draw.cpp` | 1497 | 16 | 281 (18%) | 10 | PIECE |
| `GameOS/gameos/gos_particle_bridge.cpp` | 1439 | 49 | 454 (31%) | 86 | CHEAP |
| `GameOS/gameos/vulkan_water_island.cpp` | 1414 | 1 | 1414 (100%) | 0 | PHASE |
| `GameOS/gameos/gameosmain.cpp` | 1409 | 16 | 475 (33%) | 16 | PIECE |
| `GameOS/gameos/vulkan_mech_island.cpp` | 1364 | 1 | 1364 (100%) | 0 | PHASE |
| `GameOS/gameos/gos_postprocess_shadow.cpp` | 1303 | 16 | 488 (37%) | 0 | PIECE |
| `GameOS/gameos/gos_mech_batcher_flush.cpp` | 1293 | 27 | 160 (12%) | 5 | PIECE |
| `GameOS/gameos/gameos_graphics_renderer_internal.h` | 1280 | 0 | 0 (0%) | 0 | LEFT (header) |
| `GameOS/gameos/gameos_graphics_bridge.cpp` | 1272 | 21 | 380 (29%) | 6 | PIECE |
| `GameOS/gameos/gos_postprocess.cpp` | 1270 | 27 | 456 (35%) | 6 | PIECE |
| `GameOS/gameos/gos_static_prop_batcher_submit.cpp` | 1253 | 11 | 452 (36%) | 5 | PIECE |
| `GameOS/gameos/gos_static_prop_registry.cpp` | 1241 | 53 | 158 (12%) | 65 | PIECE |
| `GameOS/gameos/debug_state_dump.cpp` | 1222 | 15 | 851 (69%) | 50 | PHASE |
| `GameOS/gameos/gos_terrain_indirect_cement.cpp` | 1205 | 22 | 446 (37%) | 38 | PIECE |
| `GameOS/gameos/vulkan_post_screenfx_native.cpp` | 1198 | 1 | 1147 (95%) | 0 | PHASE |
| `GameOS/gameos/gameos_graphics_draw.cpp` | 1176 | 29 | 212 (18%) | 0 | PIECE |
| `GameOS/gameos/gos_render.cpp` | 1167 | 28 | 310 (26%) | 16 | CHEAP |
| `GameOS/gameos/gos_static_prop_batcher_seam.cpp` | 1117 | 74 | 114 (10%) | 3 | PIECE |
| `GameOS/gameos/utils/vec.cpp` | 1116 | 151 | 50 (4%) | 0 | CHEAP |
| `GameOS/gameos/gameos_graphics_water.cpp` | 1106 | 30 | 857 (77%) | 15 | PIECE+PHASE |
| `GameOS/gameos/gos_static_prop_batcher_finalize.cpp` | 1106 | 8 | 467 (42%) | 0 | PIECE |
| `GameOS/gameos/gos_static_prop_batcher_flush_dispatch.cpp` | 1101 | 14 | 408 (37%) | 0 | PIECE |
| `GameOS/gameos/gameos_graphics_hud.cpp` | 1087 | 17 | 487 (44%) | 0 | PIECE |
| `GameOS/gameos/gos_static_prop_batcher_state.cpp` | 1080 | 20 | 80 (7%) | 143 | PIECE |
| `GameOS/gameos/MC2Strings.cpp` | 1075 | 2 | 770 (71%) | 2 | PHASE |
| `GameOS/gameos/gos_mech_batcher.cpp` | 1061 | 16 | 170 (16%) | 140 | PIECE |
| `GameOS/gameos/gos_terrain_indirect_atlas.cpp` | 1055 | 22 | 469 (44%) | 21 | PIECE |
| `GameOS/gameos/gameos_graphics_overlay.cpp` | 1003 | 27 | 186 (18%) | 5 | PIECE |
| `GuiRuntime/GuiRuntime.cpp` | 1365 | 53 | 181 (13%) | 27 | CHEAP |
| `GuiRuntime/EditorInspector_sections.cpp` | 1239 | 20 | 214 (17%) | 2 | PIECE |
| `RenderCore/bcdec.h` | 1489 | 0 | 0 (0%) | 0 | LEFT (header) |
| `RenderWorld/RenderWorld.cpp` | 1159 | 39 | 133 (11%) | 26 | CHEAP |
| `code/objmgr_update.cpp` | 1498 | 24 | 548 (36%) | 23 | PIECE |
| `code/contact.cpp` | 1398 | 66 | 98 (7%) | 9 | CHEAP |
| `code/mech_loadout.cpp` | 1390 | 3 | 632 (45%) | 3 | PIECE |
| `code/objmgr.cpp` | 1367 | 33 | 214 (15%) | 5 | PIECE |
| `code/mission_init_load.cpp` | 1343 | 20 | 282 (20%) | 0 | PIECE |
| `code/mechcmd2.cpp` | 1295 | 3 | 1091 (84%) | 87 | PIECE+PHASE |
| `code/weaponbolt_update.cpp` | 1287 | 3 | 1263 (98%) | 0 | PIECE+PHASE |
| `code/missiongui_update.cpp` | 1276 | 2 | 1017 (79%) | 0 | PIECE+PHASE |
| `code/mover_path.cpp` | 1273 | 19 | 499 (39%) | 0 | PIECE |
| `code/warrior_move.cpp` | 1264 | 16 | 502 (39%) | 2 | PIECE |
| `code/objmgr_io.cpp` | 1243 | 12 | 628 (50%) | 0 | PIECE |
| `code/gameobj.h` | 1238 | 0 | 0 (0%) | 0 | LEFT (header) |
| `code/missiongui_orders.cpp` | 1234 | 79 | 145 (11%) | 2 | PIECE |
| `code/mover_combat.cpp` | 1234 | 63 | 172 (13%) | 0 | PIECE |
| `code/gvehicl_weapons.cpp` | 1231 | 15 | 563 (45%) | 0 | PIECE |
| `code/mission_update.cpp` | 1221 | 20 | 177 (14%) | 29 | PIECE |
| `code/multplyr.cpp` | 1203 | 148 | 14 (1%) | 33 | CHEAP |
| `code/gvehicl.cpp` | 1185 | 18 | 280 (23%) | 19 | PIECE |
| `code/mechbayscreen.cpp` | 1175 | 19 | 266 (22%) | 3 | CHEAP |
| `code/missiongui_style.cpp` | 1142 | 6 | 367 (32%) | 0 | PIECE |
| `code/group.cpp` | 1102 | 38 | 193 (17%) | 6 | CHEAP |
| `code/ablmc2_state.cpp` | 1093 | 48 | 72 (6%) | 0 | PIECE |
| `code/team.cpp` | 1077 | 29 | 336 (31%) | 11 | CHEAP |
| `code/warrior_orders.cpp` | 1077 | 47 | 119 (11%) | 0 | PIECE |
| `code/ablmc2_roster.cpp` | 1074 | 69 | 48 (4%) | 6 | PIECE |
| `code/mission.cpp` | 1063 | 25 | 309 (29%) | 71 | PIECE |
| `code/mech.cpp` | 1032 | 23 | 121 (11%) | 41 | PIECE |
| `code/mission_init.cpp` | 1027 | 17 | 153 (14%) | 0 | PIECE |
| `code/mech_update.cpp` | 1021 | 13 | 640 (62%) | 1 | PIECE+PHASE |
| `code/logistics.cpp` | 1002 | 10 | 380 (37%) | 11 | MID |
| `editor/EditorData_terrain.cpp` | 1416 | 12 | 553 (39%) | 4 | PIECE |
| `editor/EditorObjects.cpp` | 1365 | 47 | 144 (10%) | 7 | CHEAP |
| `editor/EditorInterface_imgui.cpp` | 1326 | 4 | 1081 (81%) | 0 | PIECE+PHASE |
| `editor/EditorInterface_editor.cpp` | 1308 | 17 | 283 (21%) | 0 | PIECE |
| `editor/EditorInterface.cpp` | 1280 | 35 | 248 (19%) | 6 | PIECE |
| `editor/EditorGosRender.cpp` | 1240 | 23 | 321 (25%) | 12 | CHEAP |
| `editor/EditorMFC.cpp` | 1213 | 20 | 321 (26%) | 64 | CHEAP |
| `editor/EditorObjectMgr.cpp` | 1204 | 46 | 216 (17%) | 1 | PIECE |
| `editor/MapGeneratorDialog.cpp` | 1173 | 1 | 1145 (97%) | 0 | PHASE |
| `editor/EditorInterface_mouse.cpp` | 1096 | 20 | 282 (25%) | 3 | PIECE |
| `editor/Editor.cpp` | 1093 | 9 | 507 (46%) | 48 | MID |
| `editor/EditorPlaytest.cpp` | 1007 | 32 | 356 (35%) | 1 | MID |
| `gui/logisticsscreen.cpp` | 1242 | 49 | 192 (15%) | 4 | CHEAP |
| `gui/UiDefs.cpp` | 1227 | 39 | 312 (25%) | 1 | PIECE |
| `gui/asystem.cpp` | 1145 | 66 | 153 (13%) | 14 | CHEAP |
| `mclib/txmmgr.h` | 1470 | 0 | 0 (0%) | 0 | LEFT (header) |
| `mclib/elist.h` | 1463 | 0 | 0 (0%) | 0 | LEFT (header) |
| `mclib/quad_draw.cpp` | 1413 | 11 | 419 (29%) | 10 | PIECE |
| `mclib/mlr/mlr_i_pmesh.cpp` | 1403 | 23 | 306 (21%) | 10 | CHEAP |
| `mclib/ablscan.cpp` | 1395 | 42 | 182 (13%) | 65 | CHEAP |
| `mclib/soundsys.cpp` | 1377 | 38 | 270 (19%) | 17 | CHEAP |
| `mclib/estring.cpp` | 1344 | 45 | 439 (32%) | 9 | CHEAP |
| `mclib/terrain.cpp` | 1312 | 57 | 180 (13%) | 85 | PIECE |
| `mclib/terrtxm2_bake.cpp` | 1284 | 10 | 551 (42%) | 2 | PIECE |
| `mclib/mech3d_render.cpp` | 1221 | 8 | 777 (63%) | 10 | PIECE+PHASE |
| `mclib/cevfx.cpp` | 1216 | 18 | 196 (16%) | 15 | CHEAP |
| `mclib/mech3d_geometry.cpp` | 1212 | 5 | 981 (80%) | 6 | PIECE+PHASE |
| `mclib/mlr/mlr_i_tmesh.cpp` | 1206 | 19 | 419 (34%) | 8 | CHEAP |
| `mclib/move.cpp` | 1190 | 36 | 129 (10%) | 48 | PIECE |
| `mclib/gosfx/cardcloud.cpp` | 1189 | 19 | 671 (56%) | 1 | PHASE |
| `mclib/terrain_init.cpp` | 1166 | 18 | 327 (28%) | 1 | PIECE |
| `mclib/gvactor.cpp` | 1159 | 13 | 383 (33%) | 9 | PIECE |
| `mclib/inifile_arrays.cpp` | 1150 | 15 | 96 (8%) | 0 | PIECE |
| `mclib/mech3d_update.cpp` | 1096 | 7 | 673 (61%) | 0 | PIECE+PHASE |
| `mclib/move_globalmap_build.cpp` | 1081 | 20 | 492 (45%) | 0 | PIECE |
| `mclib/ablxstd.cpp` | 1071 | 52 | 90 (8%) | 31 | CHEAP |
| `mclib/bdactor_update.cpp` | 1057 | 5 | 887 (83%) | 10 | PIECE+PHASE |
| `mclib/userinput.cpp` | 1036 | 8 | 315 (30%) | 22 | MID |
| `mclib/mech3d_init.cpp` | 1028 | 14 | 470 (45%) | 0 | PIECE |
| `mclib/cone_glow.cpp` | 1021 | 30 | 283 (27%) | 11 | CHEAP |
| `mclib/mech3d_type.cpp` | 1014 | 14 | 549 (54%) | 2 | PIECE |
| `mclib/mlr/mlrindexedtrianglecloud.cpp` | 1013 | 8 | 852 (84%) | 7 | PHASE |
| `mclib/txmconv.cpp` | 1004 | 0 | 0 (0%) | 1 | LEFT |
| `mclib/bdactor_static.cpp` | 1003 | 15 | 363 (36%) | 13 | PIECE |
| `mclib/inifile.cpp` | 1003 | 44 | 86 (8%) | 2 | PIECE |
| `shaders/static_prop.vert` | 1162 | 0 | 0 (0%) | 0 | LEFT (shader) |
| `shaders/gos_terrain.frag` | 1096 | 0 | 0 (0%) | 0 | LEFT (shader) |
