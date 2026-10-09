# Narrowing baseline (lane2, claude/lane2-s3 @ f1fa193b2)

Source: full clean ninja build of `mc2` + `mc2_launcher` (741 steps, RelWithDebInfo, C++23), log `.claude/build-lane2-full.log`.
Counts are raw emissions (a header warning is re-emitted once per including TU); `unique sites` = distinct file:line.

Note: this lane's targets (mc2, mc2_launcher) emit 1581 warnings (1534 C4267), fewer than the ~3332 quoted from the owner's wider build (editor/tests targets not built here).

## Totals by code

| code | emissions | unique sites |
|---|---|---|
| C4267 | 1534 | 253 |
| C4244 | 22 | 22 |
| C4996 | 11 | 11 |
| C4477 | 6 | 2 |
| C4312 | 3 | 3 |
| C4018 | 3 | 3 |
| C4005 | 1 | 1 |
| C4838 | 1 | 1 |

## Code x origin file (sorted by emissions)

| code | origin file | emissions | unique sites |
|---|---|---|---|
| C4267 | mclib/mlr/mlrindexedprimitivebase.hpp | 106 | 2 |
| C4267 | mclib/mlr/mlr_i_l_det_pmesh.hpp | 104 | 2 |
| C4267 | mclib/mlr/mlr_i_l_det_tmesh.hpp | 104 | 2 |
| C4267 | mclib/mlr/mlr_i_l_dt_pmesh.hpp | 104 | 2 |
| C4267 | mclib/mlr/mlr_i_l_dt_tmesh.hpp | 104 | 2 |
| C4267 | mclib/mlr/mlr_i_l_pmesh.hpp | 104 | 2 |
| C4267 | mclib/mlr/mlr_i_l_tmesh.hpp | 104 | 2 |
| C4267 | mclib/mlr/mlr_i_pmesh.hpp | 104 | 2 |
| C4267 | mclib/mlr/mlr_i_tmesh.hpp | 104 | 2 |
| C4267 | mclib/mlr/mlr_i_c_det_pmesh.hpp | 52 | 1 |
| C4267 | mclib/mlr/mlr_i_c_det_tmesh.hpp | 52 | 1 |
| C4267 | mclib/mlr/mlr_i_c_dt_pmesh.hpp | 52 | 1 |
| C4267 | mclib/mlr/mlr_i_c_dt_tmesh.hpp | 52 | 1 |
| C4267 | mclib/mlr/mlr_i_c_pmesh.hpp | 52 | 1 |
| C4267 | mclib/mlr/mlr_i_c_tmesh.hpp | 52 | 1 |
| C4267 | mclib/mlr/mlrprimitiveclipping.hpp | 30 | 3 |
| C4267 | mclib/inifile.cpp | 23 | 23 |
| C4267 | mclib/mlr/mlrtriangleclipping.hpp | 22 | 2 |
| C4267 | mclib/csvfile.cpp | 19 | 19 |
| C4267 | mclib/ffile.cpp | 18 | 18 |
| C4267 | mclib/ablscan.cpp | 7 | 7 |
| C4267 | mclib/stuff/filestreammanager.cpp | 7 | 7 |
| C4267 | mclib/txmmgr.cpp | 6 | 6 |
| C4477 | mclib/stuff/note.cpp | 6 | 2 |
| C4267 | mclib/ewcharstring.cpp | 5 | 5 |
| C4267 | mclib/gosfx/debriscloud.cpp | 5 | 5 |
| C4267 | mclib/gosfx/tube.cpp | 5 | 5 |
| C4267 | mclib/mlr/mlr_i_c_det_tmesh.cpp | 5 | 5 |
| C4267 | mclib/mlr/mlr_i_c_tmesh.cpp | 5 | 5 |
| C4267 | mclib/mlr/mlr_i_pmesh.cpp | 5 | 5 |
| C4267 | mclib/stuff/memoryblock.cpp | 5 | 5 |
| C4267 | mclib/stuff/memorystream.cpp | 5 | 5 |
| C4244 | GameOS/gameos/utils/gl_utils.cpp | 4 | 4 |
| C4267 | mclib/file.cpp | 4 | 4 |
| C4267 | mclib/mlr/mlr_i_c_det_pmesh.cpp | 4 | 4 |
| C4267 | mclib/mlr/mlr_i_c_dt_pmesh.cpp | 4 | 4 |
| C4267 | mclib/mlr/mlr_i_c_dt_tmesh.cpp | 4 | 4 |
| C4267 | mclib/mlr/mlr_i_c_pmesh.cpp | 4 | 4 |
| C4267 | mclib/mlr/mlrprimitivebase.cpp | 4 | 4 |
| C4996 | mclib/stuff/filestream.cpp | 4 | 4 |
| C4018 | mclib/ffile.cpp | 3 | 3 |
| C4244 | mclib/echarstring.cpp | 3 | 3 |
| C4244 | mclib/estring.cpp | 3 | 3 |
| C4244 | mclib/ewcharstring.cpp | 3 | 3 |
| C4267 | code/mechcmd2.cpp | 3 | 3 |
| C4267 | mclib/abldbug.cpp | 3 | 3 |
| C4267 | mclib/ablenv.cpp | 3 | 3 |
| C4267 | mclib/cident.cpp | 3 | 3 |
| C4267 | mclib/heap.cpp | 3 | 3 |
| C4267 | mclib/mlr/mlrindexedprimitivebase.cpp | 3 | 3 |
| C4267 | mclib/mlr/mlrprimitivelighting.hpp | 3 | 1 |
| C4267 | mclib/mlr/mlrtrianglelighting.hpp | 3 | 1 |
| C4267 | mclib/stuff/mstring.cpp | 3 | 3 |
| C4267 | mclib/stuff/namelist.cpp | 3 | 2 |
| C4312 | mclib/heap.cpp | 3 | 3 |
| C4244 | GameOS/gameos/utils/timing.cpp | 2 | 2 |
| C4244 | mclib/stuff/memoryblock.cpp | 2 | 2 |
| C4267 | GameOS/gameos/gameos_fileio.cpp | 2 | 2 |
| C4267 | code/objective.cpp | 2 | 1 |
| C4267 | code/saveload.cpp | 2 | 2 |
| C4267 | code/warrior.cpp | 2 | 1 |
| C4267 | mclib/ablrtn.cpp | 2 | 2 |
| C4267 | mclib/ablsymt.cpp | 2 | 2 |
| C4267 | mclib/ablxstmt.cpp | 2 | 1 |
| C4267 | mclib/echarstring.cpp | 2 | 2 |
| C4267 | mclib/estring.cpp | 2 | 2 |
| C4267 | mclib/mlr/mlr_i_l_det_pmesh.cpp | 2 | 2 |
| C4267 | mclib/mlr/mlr_i_l_det_tmesh.cpp | 2 | 2 |
| C4267 | mclib/mlr/mlr_i_l_dt_pmesh.cpp | 2 | 2 |
| C4267 | mclib/mlr/mlr_i_l_dt_tmesh.cpp | 2 | 2 |
| C4267 | mclib/mlr/mlr_i_l_pmesh.cpp | 2 | 2 |
| C4267 | mclib/mlr/mlr_i_l_tmesh.cpp | 2 | 2 |
| C4267 | mclib/mlr/mlr_i_mt_pmesh.cpp | 2 | 2 |
| C4267 | mclib/stuff/memorystream_test.cpp | 2 | 2 |
| C4267 | mclib/terrtxm.cpp | 2 | 2 |
| C4996 | mclib/ablenv.cpp | 2 | 2 |
| C4996 | mclib/ffile.cpp | 2 | 2 |
| C4005 | GameOS/include/platform_io.h | 1 | 1 |
| C4244 | GameOS/gameos/vulkan_backend_skeleton.cpp | 1 | 1 |
| C4244 | mclib/ablexec.cpp | 1 | 1 |
| C4244 | mclib/ablstmt.cpp | 1 | 1 |
| C4244 | mclib/stuff/memorystream.cpp | 1 | 1 |
| C4244 | mclib/stuff/notationfile.cpp | 1 | 1 |
| C4267 | GameOS/gameos/utils/Image.cpp | 1 | 1 |
| C4267 | GameOS/gameos/utils/shader_builder.cpp | 1 | 1 |
| C4267 | code/logisticsdata.cpp | 1 | 1 |
| C4267 | code/mainmenu.cpp | 1 | 1 |
| C4267 | code/optionsarea.cpp | 1 | 1 |
| C4267 | mclib/abldecl.cpp | 1 | 1 |
| C4267 | mclib/ablexpr.cpp | 1 | 1 |
| C4267 | mclib/ablxexpr.cpp | 1 | 1 |
| C4267 | mclib/lzcomp.cpp | 1 | 1 |
| C4267 | mclib/lzdecomp.cpp | 1 | 1 |
| C4267 | mclib/mlr/mlr_i_tmesh.cpp | 1 | 1 |
| C4267 | mclib/mlr/mlr_terrain2.cpp | 1 | 1 |
| C4267 | mclib/msl.cpp | 1 | 1 |
| C4267 | mclib/packet.cpp | 1 | 1 |
| C4267 | mclib/particles/spec_library.cpp | 1 | 1 |
| C4267 | mclib/stuff/extentbox.cpp | 1 | 1 |
| C4267 | mclib/stuff/notationfile.cpp | 1 | 1 |
| C4267 | mclib/stuff/note.cpp | 1 | 1 |
| C4267 | mclib/stuff/page.cpp | 1 | 1 |
| C4267 | mclib/stuff/plane.cpp | 1 | 1 |
| C4267 | mclib/stuff/stuff.cpp | 1 | 1 |
| C4267 | mclib/stuff/table.cpp | 1 | 1 |
| C4838 | code/logisticsdialog.cpp | 1 | 1 |
| C4996 | code/ablmc2.cpp | 1 | 1 |
| C4996 | mclib/ablrtn.cpp | 1 | 1 |
| C4996 | mclib/ablsymt.cpp | 1 | 1 |

## Unique sites by code x directory

| code | dir | unique sites |
|---|---|---|
| C4267 | mclib | 112 |
| C4267 | mclib/mlr | 85 |
| C4267 | mclib/stuff | 31 |
| C4244 | mclib | 11 |
| C4267 | code | 10 |
| C4267 | mclib/gosfx | 10 |
| C4244 | GameOS/gameos | 7 |
| C4996 | mclib | 6 |
| C4244 | mclib/stuff | 4 |
| C4267 | GameOS/gameos | 4 |
| C4996 | mclib/stuff | 4 |
| C4018 | mclib | 3 |
| C4312 | mclib | 3 |
| C4477 | mclib/stuff | 2 |
| C4005 | GameOS/include | 1 |
| C4267 | mclib/particles | 1 |
| C4838 | code | 1 |
| C4996 | code | 1 |

## After (lane2 @ 684d572ad, same targets: mc2 + mc2_launcher, clean build, 741 steps)

| code | emissions before | emissions after | unique sites before | unique sites after |
|---|---|---|---|---|
| C4005 | 1 | 1 | 1 | 1 |
| C4018 | 3 | 3 | 3 | 3 |
| C4244 | 22 | 22 | 22 | 22 |
| C4267 | 1534 | 172 | 253 | 168 |
| C4312 | 3 | 0 | 3 | 0 |
| C4477 | 6 | 0 | 2 | 0 |
| C4838 | 1 | 1 | 1 | 1 |
| C4996 | 11 | 11 | 11 | 11 |

C4267 emissions dropped from the mlr_i_* header family (GetSize chain: int -> size_t) and the clip/lighting template bodies; mlr/*.cpp, note.cpp (C4477) and heap.cpp (C4312) read zero.

## Remaining C4267/C4244/C4018 origin (next slices)

mclib/*.cpp top level (inifile, csvfile, ffile, ablscan, txmmgr, ewcharstring, file, cident, heap, abl*), mclib/stuff, mclib/gosfx (debriscloud, tube), mclib/particles, GameOS/gameos/utils, code/mechcmd2.cpp. See `warn_table.py` output for per-file counts.

## /we coverage

`cmake/Mc2NarrowingErrors.cmake` promotes C4267/C4244/C4477/C4312/C4018 to errors on: mlr, fx_trace, rendercore, renderworld, gameadapters, editorbridge, gui, gui_runtime, windows. Add a target to the call once it reads zero. /we also enables level-4 variants of a code, so a target can be 'zero' under /W3 and still fail on /we (RenderCore needed a push/pop around vendored bcdec.h).

## Slice 2 (lane2 @ 9fcd5701f, mc2 + mc2_launcher, clean build, 741 steps)

| code | baseline | after slice 1 | after slice 2 | unique sites after slice 2 |
|---|---|---|---|---|
| C4005 | 1 | 1 | 1 | 1 |
| C4018 | 3 | 3 | 0 | 0 |
| C4244 | 22 | 22 | 1 | 1 |
| C4267 | 1534 | 172 | 20 | 18 |
| C4312 | 3 | 0 | 0 | 0 |
| C4477 | 6 | 0 | 0 | 0 |
| C4838 | 1 | 1 | 1 | 1 |
| C4996 | 11 | 11 | 11 | 11 |

Remaining C4267/C4244 are all in files owned by lane 1 or excluded from this slice: mclib/txmmgr.cpp (6), code/** (13), GameOS/gameos/gameos_fileio.cpp (2), GameOS/gameos/vulkan_backend_skeleton.cpp (1). EditRel adds no new ones.

/we now also covers stuff, gosfx, particles, mclib (all but bdactor/mech3d/txmmgr/terrain.cpp) and GameOS/gameos/utils (per-source, `mc2_narrowing_errors_sources`). Not covered: GameOS/gameos/*.cpp, code/, editor/.
