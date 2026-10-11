<!-- doc-status: current | reviewed:2026-10-11 -->
# Step 2 proof ledger (LG13): proof mode per commit, never-touch audit, strict re-proof of the relaxed commits

Proof modes: `plain` = tools/objhash.py (strict, now the tool default); `canonical` = tools/tidy/canonical_obj.py order-blind; `canonical+blind-data` = also .data / .bss layout blind.
`--relaxed-proof REASON` records the reason in a commit message. Everything below was measured on 2026-10-11 in build-alltargets (game, editor, tools).

## 1. Commits (chronological)
| commit | sites | mode | note |
|---|---:|---|---|
| b2e18ac9e | 108 | plain | .cpp-local; keeps 8 never-touch files (ruling: keep) |
| 15f309252 | 170 | plain | .cpp-local; 6 never-touch (keep) |
| c2674b26b | 49 | plain | .cpp-local; 11 never-touch (keep) |
| f90d911f2 | 29 | plain | headers; 6 *_internal.h (keep) |
| 3a181227a | 21 | plain | headers; 9 *_internal.h (keep) |
| 8aa5e975d | 166 | plain | headers; 4 *_internal.h (keep) |
| 3a899cf80 | 32 | plain | headers; warrior_internal.h (keep) |
| a1cd5fc80 | 1 | canonical | re-proved in section 2 |
| 311b8c9d8 | 94 | canonical | artlry.cpp + EditorObjectMgr_internal.h reverted in cdd861f05 |
| 645576c12 | 17 | canonical | re-proved in section 2 |
| 3a80e116f | 1 | canonical | re-proved in section 2 |
| 11a3cf696 | 442 | canonical+blind-data | multplyr.h + dmultplyr.h reverted in cdd861f05 |
| 8aac2756f | 92 | canonical+blind-data | re-proved in section 2 |
| d9230570c | 24 | canonical+blind-data | 5 abl headers reverted in cdd861f05 |

Revert commit `cdd861f05` (the never-touch files of the three relaxed commits, restored from fce6e0972); baseline rewrite `5246ce144` (LG13 +107, legitimately macros again).
The `setFallback(std::string_view)` change in `vulkan_water_island` (`331c9d34b`) stays as is (coordinator): it was made to keep the LG06 count level, which the new ratchet policy would not do.

## 2. Strict re-proof of the surviving relaxed sites
Method (`.claude/l2cfg/reprove.py`, scratch): the 36 files the relaxed commits still hold were put back to their pre-commit content, every object that includes any of them (479 objects) was rebuilt, and each
was compared with its HEAD build using plain objhash.

- **440 objects identical** under strict objhash: done.
- **36 objects differ only by symbol-table order / relocation index shift** (identical symbol set, every section identical once relocations are read by target name; no .data/.bss difference): recorded here, kept.
  code/controlgui.cpp.obj, code/gvehicl.cpp.obj, code/infowindow.cpp.obj, code/logisticsdata_init.cpp.obj, code/mover_diag.cpp.obj, code/multplyr_send.cpp.obj, code/objmgr_update.cpp.obj, code/optionsarea.cpp.obj, editor:AssetThumbnailCache.cpp.obj, editor:BuildingLink.cpp.obj, editor:CommandPalette.cpp.obj, editor:DamageBrush.cpp.obj, editor:EditForestDlg.cpp.obj, editor:Editor.cpp.obj, editor:EditorData.cpp.obj, editor:EditorDebugOverlay.cpp.obj, editor:EditorInterface_editor.cpp.obj, editor:EditorInterface_mouse.cpp.obj, editor:EditorModProject.cpp.obj, editor:EditorObjectMgr_pick.cpp.obj, editor:EditorObjectMgr_render.cpp.obj, editor:EditorObjectMgr_select.cpp.obj, editor:EditorObjects_unit.cpp.obj, editor:InspectorPanel.cpp.obj, editor:MissionFitBrainSource.cpp.obj, editor:ObjectSelectionBrush.cpp.obj, editor:Objective_actions.cpp.obj, editor:ObjectivesDlg.cpp.obj, editor:ObjectivesSummaryPanel.cpp.obj, editor:PlayerSettingsDlg.cpp.obj, editor:SelectionBrush.cpp.obj, editor:TelemetryPanel.cpp.obj, editor:TerrainDlg.cpp.obj, editor:UnitBrainPanel.cpp.obj, editor:mapsizedlg.cpp.obj, editor:tacMapTGA.cpp.obj
- **3 objects differ in .data (missiongui, multplyr) or .bss + .data (warrior)**: `canonical_obj` (blind to .data/.bss layout) says equal (same symbols, same per-symbol bytes), but their `.data` section bytes are laid out in
  another order, which is not one of the accepted classes (symbol order, relocation index, .bss pad). NOT resolved: delta-debugging showed ALL 28 relaxed files that these three TUs include change them (each one alone,
  or in the groups listed in `ddmin.json`), i.e. the three objects re-lay out their static data under any edit. Decision needed: keep (layout-only, evidence above) or revert up to ~28 files.
  The files: code/dobjnum.h, mover.h, objmgr.h, warrior.h, dgameobj.h, gui/asystem.h, and the groups artlry.h+controlgui.h+forcegroupbar.h+mechicon.h+dbldng.h+dmover.h+dradio.h+dterrobj.h+objtype.h+tacordr.h+
  GameOS/gameos/utils/render_constants.h+trigger.h; gui/aedit.h+afont.h+mclib/cmponent.h+tacmap.h+code/dcontact.h; code/dobjtype.h+dteam.h+dwarrior.h+gameobj.h+mclib/bdactor.h.
- Not re-proven: nothing else; the proof set covered every object in the all-targets tree that includes a relaxed file.

## 3. Never-touch files in the strict commits (kept by ruling)
8+6+11+6+9+4+1 file edits listed per commit in git (`git show --name-only <sha>`); the never-touch rule applies from 2026-10-11 on and is enforced by `NEVER_TOUCH` in rewrite_defines.py / rewrite_enums.py.
