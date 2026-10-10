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
