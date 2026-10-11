<!-- doc-status: current | reviewed:2026-10-11 -->
# Step 2 (LG13) audit: proof mode per commit and never-touch hits

Written for the meta-review. Proof modes: `plain` = tools/objhash.py (strict); `canonical` = tools/tidy/canonical_obj.py order-blind (symbol and section order);
`canonical+blind-data` = the same plus .data / .bss layout blindness. The tool now defaults to plain; `--relaxed-proof REASON` records the reason in the commit message.

## Why the relaxation was needed (what differed under strict)
- `code/infowindow.cpp.obj`, for a header edit that only turned `SCROLLUP` / `SCROLLDOWN` into inline constants: same symbol set and identical section contents, but one static data member moved in
  the symbol table, so every relocation carried another symbol index (`.text$mn` relocation bytes differ). The edit changes nothing at run time.
- `missiongui.cpp`, `multplyr.cpp`, `warrior.cpp`, `mission_init*.cpp`: a static data member (`MissionInterfaceManager::s_instance`) moved inside `.bss`, shifting the section size by an alignment pad.
- `editor/MapGeneratorDialog.cpp.obj` (only seen in the uncommitted wide-header batch): any new inline variable makes MSVC drop the unreferenced static const stb_image tables (asm diff: only the CONST array
  disappears). Handled by `--ignore-digest-file`, never used for a commit.

## Commits (chronological) and proof mode
| commit | sites | files | mode | never-touch files in it |
|---|---:|---:|---|---:|
| b2e18ac9e | 108 | 24 | plain | 8 |
| 15f309252 | 170 | 24 | plain | 6 |
| c2674b26b | 49 | 27 | plain | 11 |
| f90d911f2 | 29 | 12 | plain | 6 |
| 3a181227a | 21 | 10 | plain | 9 |
| 8aa5e975d | 166 | 23 | plain | 4 |
| 3a899cf80 | 32 | 5 | plain | 1 |
| a1cd5fc80 | 1 | 1 | canonical | 0 |
| 311b8c9d8 | 94 | 13 | canonical | 2 |
| 645576c12 | 17 | 6 | canonical | 0 |
| 3a80e116f | 1 | 1 | canonical | 0 |
| 11a3cf696 | 442 | 12 | canonical+blind-data | 2 |
| 8aac2756f | 92 | 6 | canonical+blind-data | 0 |
| d9230570c | 24 | 6 | canonical+blind-data | 5 |

## Never-touch files edited by those commits (54 file edits; revert-or-keep is the coordinator's decision)
- b2e18ac9e: code/carnage.cpp, code/gameobj.cpp, code/group_goals.cpp, code/gvehicl.cpp, code/gvehicl_control.cpp, code/mech_update.cpp, code/missiongui.cpp, code/missiongui_cursor.cpp
- 15f309252: code/mover_path.cpp, code/objmgr.cpp, code/objmgr_io.cpp, code/team_los.cpp, code/turret.cpp, mclib/ablenv.cpp
- c2674b26b: mclib/ablscan.cpp, mclib/camera_matrix.cpp, mclib/crater.cpp, mclib/estring.cpp, mclib/file.cpp, mclib/mech3d_geometry.cpp, mclib/mech3d_init.cpp, mclib/move.cpp, mclib/packet.cpp, mclib/soundsys.cpp, mclib/terrain_geometry.cpp
- f90d911f2: code/mechcmd2_internal.h, code/multplyr_internal.h, code/weaponbolt_internal.h, mclib/gvactor_internal.h, mclib/terrtxm2_internal.h, mclib/txmmgr_render_internal.h
- 3a181227a: code/controlgui_internal.h, code/gvehicl_internal.h, code/mission_internal.h, code/objmgr_internal.h, mclib/camera_internal.h, mclib/mech3d_internal.h, mclib/move_internal.h, mclib/quad_internal.h, mclib/terrain_internal.h
- 8aa5e975d: code/mover_internal.h, mclib/bdactor_internal.h, mclib/msl_internal.h, mclib/tgl_internal.h
- 3a899cf80: code/warrior_internal.h
- 311b8c9d8: code/artlry.cpp, editor/EditorObjectMgr_internal.h
- 11a3cf696: code/dmultplyr.h, code/multplyr.h
- d9230570c: mclib/abldbug.h, mclib/ablenv.h, mclib/ablerr.h, mclib/ablexec.h, mclib/ablscan.h

Not in the list: every other edited file. The uncommitted in-flight batches that would have touched `mclib/abl*`, `file.h`, `heap.h`, `packet.h`, `stuff` etc. were killed and their edits checked out.
