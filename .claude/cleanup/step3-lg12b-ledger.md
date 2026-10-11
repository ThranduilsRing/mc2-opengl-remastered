<!-- doc-status: current | reviewed:2026-10-11 -->
# Step 3 LG12b ledger: buffer-format calls that stay on sprintf/snprintf, and why

Written after B3b/B3c (`tools/tidy/printf_buffers.py` run over code/, mclib/, GameOS/, GuiRuntime/, GameAdapters/, RenderCore/ and the editor files the
per-site trial reached). Regenerate the counts by running `printf_buffers.convert` over the tree (the second result lists every refused call with its reason).
Ratchet LG12b (code, mclib, GameOS, GuiRuntime, GameAdapters, RenderCore, RenderWorld): 1497 -> see `tools/cxx17_ratchet/baseline.json`.

| count | why it stays | where | what would convert it |
|---:|---|---|---|
| 53 | file is changed on `claude/lane2-s3` (not touched by this lane until it merges); the formats are runtime strings or `PATH_SEPARATOR` ones | mechlopedia 19, logisticsdialog 10, pilotreviewarea 9, mpparameterscreen 6, mpstats 4+2, mainmenu 2, gvehicl, mech_loadout | rerun `run_tidy_safe --rewrite printf-buffers` on these files after the merge |
| 10 | lane-2 file, destination `char* s` | code/tacordr.cpp (order text for the debug window) | after the merge: take the bound from the caller's buffer, as for warrior_target |
| 37 | the format is a runtime value (resource string, `fmt[i]`, a parameter): `std::format_string` needs a literal | MissionValidation 4, mploadmap 3, ffile 3, controlgui_objectives 2, infowindow 2, BeautySidecarPreview 2, EditorData_save 2, ... | not convertible mechanically: these pass a data-supplied printf template; they stay on `snprintf`/`vsnprintf` (facade rule A4 for runtime templates) |
| 27 | `char* dest` / `char* message` parameters of the ABL debugger; the callers own buffers of unknown size | mclib/abldbug_print.cpp 20, abldbug.cpp 7 | change each function to take the capacity and the callers to pass `sizeof`; the debugger is compiled but only reachable with ABL debugging on, so left until someone touches it |
| 5 | `MechWarrior::printWeaponsStatus(char* s)`: the bound is a new parameter, which means editing code/warrior.h, a lane-2 file. Its only caller (mover_diag.cpp) passes `char ts[50]`; the longest output is 39 characters | code/warrior_target.cpp 686-702 | add `size_t cap` to the declaration and pass `sizeof(ts)`, after the lane-2 merge |
| 6 + 2 | headers with no `#include` to put `mc2fmt.h` after | gameos_graphics_material_internal.h, vulkan_shadow_island_live.cpp, platform_winbase.cpp, equipment_registry.h, EditorResourceFallback.h, mod_cache_sig.h (+ move_internal.h, terrtxm2_internal.h on lane 2) | add the include by hand |
| 4 + 1 | `_snprintf_s` / `_snprintf` whose return value is used: their truncation result (-1, no NUL) differs from `snprintf` | MC2Strings.cpp 3, gos_crashbundle.cpp 2 | review each caller's use of the return value |
| 2 | `%.Nd` precision on an integer (minimum digits): no `std::format` spelling | code/mpstats.cpp (lane 2) | zero-pad with `{:0Nd}` after checking the sign handling |
| 1 + 1 | `%*d` width argument; `%p` with a width/flag | mclib/fitblockwriter.cpp; mclib/heap.cpp | `std::vformat` with a dynamic width; hand-convert |

Done by hand in B3b (bound taken from the allocation or the enclosing array, one commit per site): gameos.cpp date (`new char[16]`), time, `mc2_conf_dir` x2,
`reg_file`; editor/EditorObjectMgr_io.cpp `fitExt+1` (what is left of `armName[512]`); mclib/genactor.cpp `changeSkyToSkyNum` (now takes the size of `newName[1024]`).

## LG12a (printf-family logging) after B4/B5/B6: what stays (593 of 1295 at 2026-10-11)
Converted: B4 untagged (GameOS 42 files + mclib/code 40), B5 tags nobody parses (~160 files), B6 parsed tags one at a time, each with its own run of
`tools/tidy/tidy_batch.py --rewrite printf-log-parsed` (MC2_PRINTF_LOG_TAG=NAME): ASSET_SCALE, TEXLOAD, PIPELINE_BIND, MOVER, RENDER_SNAPSHOT, VK_PRESENT, INSTR,
GPUPROPS, SOAK, SMOKE, TIMING, PERF. Every one passed compile + corpus + census + game-log shape comparison (mc2_03, mc2_24) before its commit.
What the ratchet still counts, with the reason:
- 88 calls whose value is used (`if (fprintf(...) < 0)`, `int n = printf(...)`): brain_special_dispatch 7, bdactor_static 6, brain_special_apply 4, gos_smoke 3, ... -> review each use of the result.
- SPEW macro (~180 sites), OutputDebugString (~19), puts/fputs: different entry points (facade P3: SPEW shim, debugger sink); not touched by printf-log.
- 16 headers with no #include to anchor mc2fmt.h on; 11 `%.Nd` (gos_render.cpp); 4 `%t` length (gameos_graphics_light_ssbo.cpp); 3 `%*d`; 1 `%S`; 4 runtime formats (gameosmain_frame.cpp).
- 3 crash/assert/allocator files (gos_crashbundle, gameos_debugging, memorymanager/heap): excluded on purpose, std::string must not be built there.
- lane-2 files (terrain_geometry.cpp: FASTPATH_DROP and QUADSETUP_SKIP are parsed tags waiting there; projectz_trace.cpp; terrtxm2_internal.h): after lane 2 merges, rerun
  `MC2_PRINTF_LOG_TAG=FASTPATH_DROP py -3 tools/tidy/tidy_batch.py --dir mclib --rewrite printf-log-parsed --corpus --log-identity`, same for QUADSETUP_SKIP.
- Parsed tags emitted through snprintf/SPEW/log_* rather than printf (CRASHBUNDLE, HEARTBEAT, CONFIG, ...) are not touched by this rewriter; their census records are unchanged.
