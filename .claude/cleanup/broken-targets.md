# Broken build targets (from build_logs/cxx23_probe*.out)

Read-only audit. No builds run, no sources edited.

## Errors

| Target | Error | Cause | Proposed fix | Effort |
|---|---|---|---|---|
| editor (EditRel) | C2084 `mc2_save_camera_bookmark` already has a body (editor/EditorGameOS.cpp:244 vs :226) | Two identical `extern "C"` no-op stubs added in the same file (same commit 560dfc1d). Second block at lines ~233-250 is a duplicate. | Delete the second definition and its comment block. | S (minutes) |
| editor (EditRel) | C2665 `Unit::save` no overload (editor/EditorObjects.cpp:407, :411) | Declared `bool save(FitIniFile*, int, int, char* objectProfile)` (editor/EditorObjects.h:310) but called with string literals (`const char[9]`), which cannot convert to `char*`. | Change the parameter (header + definition) to `const char*`. | S |
| text_tool | 165 unresolved externals (LNK2001/2019) | Links `gameos` static lib, which pulls game-layer globals/functions (`eye`, `land`, `Terrain::*` statics, `gamePaused`, `quitGame`, `mc2FreezeMoversActive`, `mc2VisualCaptureSetPaused`, `mc2_*_collect_witness`, ImGui/GuiRuntime, `TexResolveTable`, `Camera::*`). Those are defined in code/, mclib/, editor/ TUs not in its link set. CMake comment in data_tools/CMakeLists.txt documents the same class of problem. | Either mirror the data_tools link closure plus a stub TU (165 symbols: large), or set EXCLUDE_FROM_ALL. | L |
| mc2_asset_viewer | 9 unresolved (`mc2FullBakeDiagEnabled`, `cone_geom_hidden_by_alpha`, `g_txmmgr_vertex_block_exhausted`, `g_txmmgr_add_vertices_overflow_prevented`, `g_mechPreviewRenderDepth`, `g_txmmgrBoundsTrace`, `g_staticPropAlphaGeneration`, `g_fbDiagPtSpotZeroDir`, `g_fbDiagPtSpotConsumed`) | Hand-curated source list in tools/asset_viewer/CMakeLists.txt includes mclib/tgl.cpp and msl.cpp but not the TUs defining these: mclib/bdactor.cpp, mclib/cone_glow.cpp, mclib/txmmgr.cpp, GameOS/gameos/gos_static_prop_batcher.cpp. | Add the defining TUs to the source list (check transitive deps), or add a small stub TU. | M |
| makefst, pak, aseconv, makersp | ~20 unresolved each (`gamePaused`, `quitGame`, `theActionExecutorRegistry`, `mc2FreezeMoversActive`, `mc2SimIdentityWriteJson`, `mc2VisualCaptureSetPaused/GetPauseState/RestorePauseState`, `mc2_{unitprofile,unitpreview,component}_collect_witness`, `mc2_unitpreview_anim_tag_name`, `mc2UnitProfileDataEnabled`, ImGui_ImplVulkan_*, ImGui::GetIO/GetCurrentContext, GuiRuntime::NotifyResize) | data_tools/CMakeLists.txt DATA_TOOLS_ENGINE_LIBS lacks `imgui` and `gui_runtime` (mc2 target links both, line 606). Remaining game globals are not in data_tools_game_stubs.cpp (43 lines). | Add `imgui gui_runtime` to DATA_TOOLS_ENGINE_LIBS; add stubs for the rest in data_tools_game_stubs.cpp (pattern already exists in editor/unitprofile_editor_stub.cpp). | M |

Out of scope, seen in the same logs: C2666 `LogisticsVariant::operator ==` at code/mechlabscreen.cpp:1191 (mc2 target); C2445 ambiguous conditional at mclib/gosfx/effect.cpp:813 and mclib/particles/spawn_cardcloud.cpp:129; C1083 missing `imgui.h` for gameos_editor (GameOS/gameos/engine_present_vk.cpp:35).

## Default build membership

- editor: `option(MC2_EDITOR ... ON)` (CMakeLists.txt:112); `add_subdirectory(./editor)` at :752. In default build.
- text_tool: `add_subdirectory(./text_tool)` at :737, no EXCLUDE_FROM_ALL. In default build.
- data_tools: `add_subdirectory(./data_tools)` at :736, no EXCLUDE_FROM_ALL. In default build.
- tools/asset_viewer: guarded by EXISTS at :391-392, no EXCLUDE_FROM_ALL. In default build.

Users of these tools: .planning/codebase/INTEGRATIONS.md and .claude/agents/mc2-build-system-expert.md say the stock FST/PAK pipeline (mc2srcdata/build_scripts/) uses aseconv, makefst, makersp, mpak, text_tool. So data_tools and text_tool are live tooling, not dead code. No doc or script found referencing mc2_asset_viewer as a pipeline dependency (only compile_commands and build dirs).

## Verdicts

- editor (EditRel): **FIX**. Two small edits, unblocks the default build.
- data_tools (makefst, pak, aseconv, makersp): **FIX**. Used by the asset pipeline. Not a delete candidate.
- text_tool: **DROP from default build** (add `EXCLUDE_FROM_ALL`), keep target for explicit builds. Fix later (L effort). Used by the pipeline, so do not delete.
- mc2_asset_viewer: **FIX** (M effort, add defining TUs or stubs).
- DELETE candidates: none. An adversarial audit would be needed before any deletion, and nothing here supports it.

Caveat: fixes are inferred from source and logs. None were compiled or tested. The logs may predate later commits.
