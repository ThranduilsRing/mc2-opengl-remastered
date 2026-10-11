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
- **3 objects differ in .data (missiongui, multplyr) or .bss + .data (warrior)**: RULED KEEP (coordinator, 2026-10-11): **layout-only, per-symbol identical**. Their static data is laid out in another order under
  any edit (delta-debugging: ALL 28 relaxed files these TUs include change them), while the symbol set and every symbol's bytes and relocation targets are identical. `tools/objhash.py --per-symbol` now reports exactly this
  as `LAYOUT-ONLY` instead of `DIFF` (a strengthening of the proof statement, not a relaxation: the plain proof stays the default and still says "changed"). Re-measured with the new tool on the saved H/P object pairs:
  the 3 are `LAYOUT-ONLY`; of the 36 order-only objects above 16 are `LAYOUT-ONLY` as well (a .data/.bss section reordered) and 20 are `ORDER-ONLY` (only symbol-table / relocation index order differs, every section identical by name):
  code/infowindow.cpp.obj, code/optionsarea.cpp.obj and the editor objects AssetThumbnailCache, BuildingLink, DamageBrush, EditForestDlg, Editor, EditorData, EditorDebugOverlay, EditorInterface_editor, EditorModProject, EditorObjectMgr_pick,
  EditorObjectMgr_render, EditorObjects_unit, InspectorPanel, ObjectSelectionBrush, ObjectivesDlg, TerrainDlg, UnitBrainPanel, tacMapTGA. A .bss symbol has no bytes in an object and its size is not recorded, so .bss is compared by symbol set only.
  The files behind them: code/dobjnum.h, mover.h, objmgr.h, warrior.h, dgameobj.h, gui/asystem.h, and the groups artlry.h+controlgui.h+forcegroupbar.h+mechicon.h+dbldng.h+dmover.h+dradio.h+dterrobj.h+objtype.h+tacordr.h+
  GameOS/gameos/utils/render_constants.h+trigger.h; gui/aedit.h+afont.h+mclib/cmponent.h+tacmap.h+code/dcontact.h; code/dobjtype.h+dteam.h+dwarrior.h+gameobj.h+mclib/bdactor.h.
  Reproduce: `py -3 tools/objhash.py diff A.obj B.obj` (IDENTICAL / LAYOUT-ONLY / ORDER-ONLY / DIFF); batch form `objhash.py hash --per-symbol ...` then `objhash.py compare --per-symbol before.json`.
- Not re-proven: nothing else; the proof set covered every object in the all-targets tree that includes a relaxed file.

## 3. Never-touch files in the strict commits (kept by ruling)
8+6+11+6+9+4+1 file edits listed per commit in git (`git show --name-only <sha>`); the never-touch rule applies from 2026-10-11 on and is enforced by `NEVER_TOUCH` in rewrite_defines.py / rewrite_enums.py.

## 4. Last LG13 batch of the second lane-2 session (2026-10-11, strict plain objhash, `--tree build-alltargets`)
- `code/unitdesg.h` (52 sites, 155 including objects): REVERTED by the proof, nothing kept. Cause 1 (fixed in 23de3aeda): `MC2_VERIFY_BOUNDS(i, MAX_COMMANDERS)` wraps `MC2_VERIFY`, whose `#cond` sees the
  macro-EXPANDED text, so the failure-message literal in mission_init.cpp.obj / mission_init_load.cpp.obj changed from the value to the name (symbol set, .rdata, .text differ). `rewrite_defines.py` now refuses a constant named
  in the arguments of such a wrapper (`stringized-by-wrapper-macro`); direct stringizers keep the raw name and stay eligible.
- Second run (51 sites): 154 of 155 objects identical; `controlgui.cpp.obj` differs only by its `.data` / `.bss` layout (`objhash.py diff` = LAYOUT-ONLY, symbol set and every symbol's bytes identical). The strict proof refuses it; accepting
  LAYOUT-ONLY in the rewriters' Proof is a proof-mode change and is NOT made by the lane. Decision for the orchestrator: an opt-in `--layout-only-ok` flag (never a default), or leave `unitdesg.h` as macros.
- Everything else the rewriter still lists is a wide header (30 files, about 660 sites, 334-1,080 including objects: mclib/move.h 137, sounds.h 207, userinput.h 64, mech3d.h 37, weaponfx.h 31, dbasegui.h 30, vfx.h 19, inifile.h 15, gameos.hpp 15,
  txmmgr.h 14, ...): needs the same proof-mode decision, because `editor/MapGeneratorDialog.cpp.obj` drops its stb_image tables under any header edit (see section 3 of the plan).

## 5. Rulings of 2026-10-11 applied (strict plain objhash; `--layout-only-ok` and `--vs-gate` are opt-in flags of `rewrite_defines.py` / `rewrite_enums.py`, defaults unchanged)
- LG14: 18 enums converted in 4 commits (929ecc662 cab34b9e2 edb92d936 b31a71d36), every including object of the Ninja trees AND of tests/unit + the harness trees IDENTICAL; details in `cxx23-step2-enums-note.md`.
- LG13 `code/unitdesg.h` (51 sites, 155 objects): kept in 69b935d96 with `--layout-only-ok`; the commit message names the two LAYOUT-ONLY objects: `CMakeFiles/mc2.dir/code/controlgui.cpp.obj`, `CMakeFiles/mc2.dir/code/mover.cpp.obj`.
- LG13 wide headers (30 files + 7 platform_win*/actor.h with no includer): NOT converted. A 5-header batch (genactor, gvactor, move, weaponfx, debugging) reverted all five on ONE object, `editor/SingleValueDlg.cpp.obj`, which is ORDER-ONLY
  (every section and symbol identical by name; only symbol-table / relocation indices shifted); a 1-site header (`dident.h`) does the same, so ORDER-ONLY on editor TUs is inherent to adding any inline variable to a wide header.
  A scan with all 37 headers edited at once (build-alltargets, 1,522 objects): 1,397 IDENTICAL, 82 LAYOUT-ONLY, 35 ORDER-ONLY (32 editor / MLR / gameos objects), 8 DIFF. Of the DIFFs: `MapGeneratorDialog.cpp.obj` (the stb_image tables dropped)
  is caused by 12 headers individually (gameos.hpp, daprtype, dbasegui, inifile, mech3d, move, sounds, terrain, txmmgr, userinput, vfx, weaponfx); `mechcmd2.cpp.obj` differs only with the full set together (no single culprit);
  `gos_crashbundle`, `gos_visual_capture`, `gameos_graphics` (x2 each: game and editor) have no culprit header (zero or one edited header reaches them: build noise). So the wide headers need TWO more rulings: ORDER-ONLY acceptance
  (a pure index shift) and the MapGeneratorDialog class (`--ignore-digest-file`, a compile-must-pass object whose unreferenced stb tables vanish). The lane applied neither.

## 6. Wide-header LG13 batches under `--layout-only-ok --order-only-ok --vs-gate` (rulings 2026-10-11, second message)
- ORDER-ONLY accepted as an opt-in (`--order-only-ok`); DIFF still reverts. Every accepted object is named with its class in the commit message ("Objects accepted without being IDENTICAL (N ...)", one line each).
- `MapGeneratorDialog.cpp.obj` is NOT ignored: `tools/tidy/proof_allow.json` holds one bounded entry (class `unreferenced-data-dropped`, the 12 headers that trigger it, the reason). The proof honours it only when the batch edits one of
  those headers AND `objhash.classify_allowed` confirms per symbol: every other section, the rest of the symbol set and every `.data` / `.bss` symbol identical, every static `.rdata` symbol present in both objects byte-identical, any
  static present in only one never a relocation target. Observed class: the unreferenced stbi_* tables and `k_biomeFoliage` sit in the one non-COMDAT `.rdata` and MSVC lays them out in another order after any new inline variable.
  Used once (04f05bb87). `--no-proof-allow` disables the ledger.
- Kept (5 commits, 19 headers, 587 sites; LG13 1,707 -> 1,120): 04f05bb87 debugging genactor gvactor move weaponfx (175); cc8a4ab2d cevfx crater objectappearance sounds soundsys (225); 17ecb3df2 appear daprtype ffile mech3d userinput (129);
  51e4ae3aa camera dident floathelp timing (8); 06e2957c9 bitflag dbasegui terrain terrtxm terrtxm2 (50). Accepted-not-identical objects per commit: 70, 46, 70, 35, 60 (LAYOUT-ONLY and ORDER-ONLY; see each message).
- Afterwards `mclib/tgl.h` (6 sites) went through alone with the same flags and gate (all 477 + 16 objects identical): the batch failure was an interaction. NOT converted (5 headers, ~50 sites; tgl.h was in the original six): `mclib/dstd.h`, `inifile.h` (the harness gate: `build64-tests::mc2_tests.dir/.../test_file_inifile.obj` differs or does not build), `tgl.h`, `txmmgr.h`, `vfx.h` (the gate failed those
  objects with no attributable error text, so they were blamed together: a bisect per header is the first thing to do), and `GameOS/include/gameos.hpp` (one object, `CMakeFiles/mc2.dir/code/controlgui_hudtabs.cpp.obj`, differs; it appeared once
  for dbasegui.h in a bigger batch and not when dbasegui.h went through alone, so it is flaky or combination-dependent: its diff must be classified before anyone decides). The 7 `GameOS/include/platform_win*.h` and `code/actor.h`
  have no including object in any tree (platform headers are the non-Windows arm): out of reach of an object proof.
- Checkpoint at ef8d2d2a5-level and again after the last batch (HEAD at 06e2957c9 plus docs): ninja build, `build_all_targets.py` every stage, doctest 2,719 cases, tier1 5/5 (13:13 and 14:40), board 165/168 (the three inherited reds).
