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
