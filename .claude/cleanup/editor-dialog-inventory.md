# Mission editor dialog and workflow inventory

Baseline inventory at `HEAD e9d18b8a7`. Count command:
`rg -n 'class [A-Za-z_][A-Za-z0-9_]*.*CDialog' editor | Measure-Object`
returns **37 dialog classes**. `rg -o 'CDialog\b' editor | Measure-Object`
returns 181 literal `CDialog` occurrences; `rg -o 'DDX_Text\(' editor |
Measure-Object` returns **93 text bindings**. The dialog list below is all 37
class declarations from the first command. DDX members are the controls/values
bound by each class, with their `DoDataExchange` source lines. Fields named
without DDX are copied by the dialog's explicit code.

`scripts/run_editor_smoke.py` retains the original 15 result rows and adds
`stock_roundtrip` (24 individual mission rows) and `workflows` (seven graded
facts covering six workflows). Action-handler reach means production actions
shared with the UI were invoked, authored state verified, undone, saved and
reloaded. It does not prove rendered widgets or legacy MFC dialog instantiation.
Model-only rows use `coverage=model-only reached=0`; the parser accepts this
explicit limit and rejects a model-only row claiming a panel reach.

The old `2026-10-13T11-19-02` proof was rejected: its loose Buildings.csv
was non-stock, and catalog activation was credited for an unrelated drop zone.
Final stock confirmation `scratch/editor-smoke/2026-10-13T12-52-11/workflows.stderr.txt`
contains five action-handler facts (`reached=1`) and two model-only facts
(`reached=0`), all `ok=1 authored=1 undo=1 saved=1 reloaded=1`.
The final full suite is PASS 40/40 with no retries: original 15/15,
24/24 frozen stock comparisons, and the workflow row. Waypoint undo now
restores the authored ownership flag as well as the order values.
The editor was rebuilt with `py -3 tools/build_quiet.py --target EditRel`.
See `editor-roundtrip-ledger.md` for the final full-suite run, stock dependency
manifest, frozen baseline, findings and adopted/deleted artifact decisions.
The 37 legacy dialog answers below describe what was actually instantiated;
the workflow table separately records the ImGui/tool/model probes.

| Workflow | Probe and required fact | smoke reaches |
|---|---|---|
| Terrain paint | Real material brush changes the middle vertex; undo restores it, redo authors it, mission PAK save/reload verifies the new material. | action-handler: PaintMaterial reached=1 ok=1. |
| Object placement | AssetBrowser::ActivateFirstObject selects the production BuildingBrush; that same brush places its catalog object, undo removes it, redo and PAK save/reload retain the ID and position. | action-handler: AssetBrowser reached=1 ok=1. |
| Teams/ABL | Inspector's applyObjectAlignment mutates the placed catalog object, undo/redo restores/reapplies its team; UnitBrain BeginWaypointPlace/HandlePlacementClick authors a real unit waypoint, undo restores order values and the ownership flag, save/reload retains the order, stance and point. | action-handler: Inspector and UnitBrain reached=1 ok=1. Raw ABL text editing is not proved. |
| Objectives | Objective/condition/activation flag appended directly to the model; collection Save/Read compares equality; snapshot restoration verifies model undo. The read-only summary has no authoring handler. | model-only: ObjectivesSummary reached=0 ok=1; no panel reach. |
| Campaign links | Mission/purchase link appended directly to campaign model; Save/Read compares equality and snapshot restoration verifies model undo. Overview Load uses its production loader; link authoring remains model-only. | model-only: CampaignOverview reached=0 ok=1; no panel reach. |
| Mission export | Authored terrain/object/team/order saved to PAK/FIT through the production quick-save writer, undone, then reloaded through initTerrainFromPCV; authored properties verified. Full MOVE/tac-map export remains outside this row. | action-handler: MissionWriter reached=1 ok=1. mc2_22 now compares after its headless default-OK warning. |

## Terrain paint and map environment

| Dialog | Fields / document values | Mode / smoke / SDL+ImGui destination | smoke reaches |
|---|---|---|---|
| `TerrainDlg` — `editor/TerrainDlg.h:19`; manual `terrain`, `TerrainDlg.cpp:29-56` | Initial map terrain index; map creation reads this selection (`EditorInterface_editor.cpp:100-109`). | Modal / no / terrain creation panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `MapSizeDlg` — `editor/mapsizedlg.h:19`; manual `mapSize`, `mapsizedlg.cpp:23-60` | Map dimension selection. | Modal / no / terrain dimensions panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `HeightDlg` — `editor/HeightDlg.h:20`; manual `min`, `max`, `HeightDlg.cpp:18-38` | Terrain height range. | Modal / no / terrain height panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `FractalDlg` — `editor/fractaldialog.h:18`; `m_threshold`, `m_noise`, `fractaldialog.cpp:22-27` | Terrain fractal threshold/noise inputs. | Modal / no / terrain generation panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `SelectSlopeDialog` — `editor/SelectSlopeDialog.h:17`; `m_MaxEdit`, `m_MinEdit`, `SelectSlopeDialog.cpp:32-37` | Slope selection bounds. | Modal / no / terrain selection panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `SelectTerrainTypeDlg` — `editor/SelectTerrainTypeDlg.h:17`; list control `m_terrainTypeList`, `.cpp:43-47` | Selected terrain type for terrain tools. | Modal / no / terrain palette. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `FogDlg` — `editor/FogDlg.h:18`; `m_blue`, `m_green`, `m_red`, `m_start`, `m_end`, `FogDlg.cpp:29-37` | Fog color and start/end distances. | Modal / no / atmosphere panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `SunDlg` — `editor/SunDlg.h:20`; `m_LightColor`, `m_AmbientEdit`, `m_Yaw`, `m_InitialSunPitch`, `sunDlg.cpp:36-43` | Sun direction and light/ambient colors; includes terrain-shadow/refractalize actions (`sunDlg.cpp:88-119,226-244`). | Modal / no / lighting panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `WaterDlg` — `editor/WaterDlg.h:17`; `amplitude`, `frequency`, `elevation`, `shallowElevation`, `alphaElevation`, `alphaDeep`, `alphaMiddle`, `alphaShallow`, `WaterDlg.cpp:40-52` | Water surface/wave and shallow/deep alpha settings. | Modal / no / water panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `WaveDlg` — `editor/WaveDlg.h:17`; manual public wave values, `WaveDlg.cpp:12-78` | Legacy wave amplitude/frequency/elevation/color values. `rg -n 'WaveDlg' editor` finds only its declaration/implementation: no live menu caller was found. | Uncalled legacy modal class / no / merge into water panel only after restoring a caller. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `TilingFactorsDialog` — `editor/TilingFactorsDialog.h:17`; `m_TerrainDetailTilingFactor`, `m_WaterDetailTilingFactor`, `m_WaterTilingFactor`, `.cpp:33-39` | Terrain/water texture tiling factors. | Modal / no / material tiling panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `ForestDlg` — `editor/ForestDlg.h:17`; density/height min/max, random flag, X/Y/radius, name and slots `m_1..m_10,m_12..m_15`, `ForestDlg.cpp:50-95` | Forest brush placement and per-forest parameters. | Modal / no / foliage painting panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `EditForestDlg` — `editor/EditForestDlg.h:17`; file list and `m_Name`, `EditForestDlg.cpp:37-42` | Forest definitions: load/save/rename and selected definition (`EditForestDlg.cpp:73-136,184-231`). | Modal / no / foliage definition panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `EditorTacMap` — `editor/EditorTacMap.h:26`; `picture`, `EditorTacMap.cpp:31-35` | Read-only map image/navigation; `EditorInterface` owns it as a member (`EditorInterface.h:405`). | **Modeless** `Create`/`ShowWindow` (`EditorInterface.cpp:1249-1253`) / no / persistent minimap panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |

## Object placement and team assignment

| Dialog | Fields / document values | Mode / smoke / SDL+ImGui destination | smoke reaches |
|---|---|---|---|
| `UnitSettingsDlg` — `editor/UnitSettingsDlg.h:21`; variant, pilot, mech, group, alignment, squad, self-repair, `.cpp:31-41` | Selected unit's type, assignment, alignment, squad and behavior; edits through `applyChanges`/undo (`UnitSettingsDlg.cpp:216-362`). | Modal / no / unit inspector and placement defaults. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `BuildingSettingsDlg` — `editor/BuildingSettingsDlg.h:21`; mech, group, alignment, X/Y, part ID, forest, damaged, `.cpp:38-49` | Selected building's position, type/part, group, forest and damage state; `applyChanges`/undo (`BuildingSettingsDlg.cpp:85-209`). | Modal / no / building inspector and placement defaults. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `ChooseUnitDlg` — `editor/ChooseUnitDlg.h:17`; pointer-based selected `Unit*`, no DDX (`ChooseUnitDlg.cpp:25-37,96-169`) | Choose or pointer-pick a unit reference for an objective predicate (`Objective_conditions.cpp:131,186`). | Modal / no / objective entity-reference picker. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `ChooseBuildingDlg` — `editor/ChooseBuildingDlg.h:17`; pointer-based selected `EditorObject*`, no DDX (`ChooseBuildingDlg.cpp:25-35,92-165`) | Choose or pointer-pick a building reference for an objective predicate (`Objective_conditions.cpp:257`). | Modal / no / objective entity-reference picker. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `PointerSelectObjectDlg` — `editor/PointerSelectObjectDlg.h:15`; map-pick state/button, no DDX (`PointerSelectObjectDlg.cpp:25-66`) | Uncalled class: `rg -n 'PointerSelectObjectDlg' editor` finds definitions and an include (`ChooseUnitDlg.cpp:18`), no instantiation. Actual chooser handoff toggles selection-only mode and consumes selected objects (`ChooseUnitDlg.cpp:125-159`; `ChooseBuildingDlg.cpp:121-155`). | Uncalled modal class / no / shared viewport picker supplied by the active chooser workflow. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `PlayerSettingsDlg` — `editor/PlayerSettingsDlg.h:17`; `m_defaultTeamComboBox`, `m_playerEdit`, `.cpp:34-39` | Player identity and default team (`EditorInterface_terrain.cpp:629-638`). | Modal / no / teams panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |

## Objectives and ABL-facing authoring

| Dialog | Fields / document values | Mode / smoke / SDL+ImGui destination | smoke reaches |
|---|---|---|---|
| `ObjectivesDlg` — `editor/ObjectivesDlg.h:53`; objective list/buttons/team, `.cpp:35-45` | Team's objective collection and ordering; add/edit/copy/remove/move actions (`ObjectivesDlg.cpp:54-56,142-245`). | Modal / no / team objectives panel. | model-only: collection authoring/restore/Save/Read; no panel or legacy dialog reach. |
| `ObjectiveDlg` — `editor/ObjectiveDlg.h:22`; priority, resource points, prerequisite flags, marker/hidden/activation/reset flags, X/Y, title/description and resource IDs, plus action/condition lists, `ObjectiveDlg.cpp:62-95` | Individual objective state and success/failure condition/action lists (`Objective.cpp:649`). | Modal / no / objective detail panel with condition/action editors. | model-only: authored title, condition and activation flag; no panel or legacy dialog reach. |
| `CObjectiveTemplateDlg` — `editor/ObjectivesDlg.h:21`; preset combo `m_PresetCombo`, `ObjectivesDlg.cpp:68-79` | Preset choice only; preset constructs a new objective (`ObjectivesDlg.cpp:231-263`). | Modal / no / objective templates panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `CBooleanFlagIsSetDialog` — `editor/BooleanFlagIsSetDialog.h:17`; `m_Value`, `m_FlagID`, `.cpp:32-37` | ABL flag condition's boolean value and flag ID (`Objective_conditions.cpp:493-502`). | Modal / no / condition editor. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `SingleValueDlg` — `editor/SingleValueDlg.h:20`; manual `val`, caption and static text (`SingleValueDlg.cpp:24-54`) | Numeric argument such as objective thresholds, terrain elevation or smoothing radius (`Objective_conditions.cpp:49`; `EditorInterface_terrain.cpp:438,467`). | Modal / no / typed condition/property editor. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `TargetAreaDlg` — `editor/TargetAreaDlg.h:17`; reference-bound center X/Y/radius, no DDX (`TargetAreaDlg.cpp:23,122-143`) | Objective target-area center and radius; map pick updates center (`:150-212`). | Modal / no / target-area editor attached to viewport. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `TextMessageDlg` — `editor/TextMessageDlg.h:17`; `m_TextMessage`, `TextMessageDlg.cpp:31-35` | Objective/message action text (`Objective_actions.cpp:180-189`). | Modal / no / localized text editor. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `ResourceStringSelectionDlg` — `editor/ResourceStringSelectionDlg.h:19`; resource combo `m_Combo`, `.cpp:45-49` | Resource-string ID selection (`Objective_actions.cpp:228-238`). | Modal / no / localization picker. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `CUserTextEdit` — `editor/UserTextEdit.h:17`; text and resource-string ID, `UserTextEdit.cpp:47-52` | Dual-mode authored text or localized resource ID, used by campaign, mission and objective labels (`CampaignDialog.cpp:389-400`; `MissionSettingsDlg.cpp:251-281`; `ObjectiveDlg.cpp:1293-1314`). | Modal / no / localization field editor. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `ViewConditionOrActionDlg` — `editor/ViewConditionOrActionDlg.h:17`; `m_Edit`, `.cpp:31-35` | Displays serialized condition/action text for inspection; objective handlers open it (`ObjectiveDlg.cpp:1438-1493`). | Modal / no / read-only condition/action inspector. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |

## Campaign graph and mission export/settings

| Dialog | Fields / document values | Mode / smoke / SDL+ImGui destination | smoke reaches |
|---|---|---|---|
| `CCampaignDialog` — `editor/CampaignDialog.h:21`; group list, CBills, final video, name and name resource ID, `.cpp:44-51` | Campaign identity, rewards/video and linked group ordering (`CampaignDialog.cpp:173-228,301-370`). | Modal / no / campaign graph panel. | model-only: stock campaign links Save/Read/restore; no panel or legacy dialog reach. |
| `CGroupDialog` — `editor/GroupDialog.h:19`; tune, mission list, operation/prevideo/video files, label, minimum missions, `.cpp:35-44` | Campaign group metadata and mission links (`GroupDialog.cpp:146-223`). | Modal / no / campaign group panel. | model-only: append/restore a group mission link; no panel or legacy dialog reach. |
| `CMissionDialog` — `editor/MissionDialog.h:17`; mission/purchase file paths and logistics/mandatory/promotion/purchase/salvage/selection flags, `.cpp:41-52` | Campaign entry's mission link and campaign behavior flags. It is nested inside group add/edit (`GroupDialog.cpp:146-180,207-223`). | Modal / no / campaign mission-link panel. | model-only: mission and purchase file links; no panel or legacy dialog reach. |
| `MissionSettingsDlg` — `editor/MissionSettingsDlg.h:17`; author, player/team limits, time, initial resource/CBills, mission type, name/blurb and resource IDs, video/URL, gameplay options, `.cpp:82-123` | Mission `.fit` settings/metadata and feature flags (`EditorInterface_terrain.cpp:534-553`). | Modal / no / mission settings panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `NewSingleMission` — `editor/newsinglemission.h:17`; no DDX fields; load/new/map-generator commands and mod choice (`EditorInterface_editor.cpp:35-123`; `newsinglemission.h:29-48`) | Selects mission creation/load path and active mod, not a field in the current document. | Modal / no / project start/open panel. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |
| `DlgFileOpen` — `editor/DlgFileOpen.h:19`; manual file name, extension, directory and save/load mode; fixed buffers `fileName[MAX_PATH]`, `m_directory[MAX_PATH]`, `.h:58-65`; selection/accept logic `.cpp:45-120` | Mission/campaign and asset file selection; path result feeds the owning workflow. | Modal / no / file browser integrated into project/campaign panels. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |

## Other

| Dialog | Fields / document values | Mode / smoke / SDL+ImGui destination | smoke reaches |
|---|---|---|---|
| `CAboutDlg` — `editor/EditorMFC.cpp:659`; no document fields, `.cpp:673-688` | About/version content. | Modal / no / About overlay. | No legacy dialog instantiation observed; headless probes use ImGui/tools and the authoring model. |

## Shared writers and ownership hazards

- The campaign editor has nested writers over one campaign graph: `CCampaignDialog`
  copies group values in/out (`CampaignDialog.cpp:173-220`), opens `CGroupDialog`
  (`:193-198,285-296`), and that dialog copies/edits `CMissionDialog` values
  (`GroupDialog.cpp:146-180,207-223`). A document command/undo layer must own one
  graph mutation boundary rather than preserving nested mutable copies.
- Objectives also have nested writers: `CObjectives::EditDialog` opens
  `ObjectivesDlg` and `CObjective::EditDialog` opens `ObjectiveDlg`
  (`Objective.cpp:649,873`). `ObjectivesDlg` owns team/list ordering while
  `ObjectiveDlg` edits an item and its ABL condition/action lists
  (`ObjectivesDlg.cpp:142-245`; `ObjectiveDlg.cpp:62-95`).
- `UnitSettingsDlg` and `BuildingSettingsDlg` receive references to selected
  pointer lists and an `ActionUndoMgr` (`UnitSettingsDlg.h:24`; `:80`;
  `BuildingSettingsDlg.h:24`; `:65`). Their handlers mutate the same live objects
  used by `EditorObjectMgr` and brush placement (`EditorInterface_wnd.cpp:231-252`;
  `EditorInterface_objects.cpp:8-20,90-140`).
- Localized fields are shared writers: `CUserTextEdit` nests `TextMessageDlg` and
  `ResourceStringSelectionDlg` (`UserTextEdit.cpp:94-118`); campaign, mission and
  objective handlers all reuse it (refs above). Store localized value/resource
  ID as an owned document value, not dialog-owned `CString` state.
- The editor creates `systemHeap = new UserHeap` (`Editor/Editor.cpp:279-288`)
  and tears it down manually (`:883-896`); it also creates a separate TGL heap
  (`:668-687`). Dialogs keep borrowed pointers/references to live model values,
  e.g. chooser output pointers (`ChooseUnitDlg.h:39-45`,
  `ChooseBuildingDlg.h:38-44`) and target-area floats (`TargetAreaDlg.h:41-48`).
  Selection can change or a document can close while those references are in
  flight; the owned model/command stack needs stable handles and explicit lifetime.
- `EditorTacMap` is a member yet owns a separately created HWND; teardown
  conditionally calls `DestroyWindow` (`EditorInterface.h:405`;
  `EditorInterface.cpp:1249-1253`; `EditorInterface_wnd.cpp:522-532`). The panel
  replacement should make view lifetime follow its editor window/document.
- `DlgFileOpen` stores paths in fixed raw character arrays (`DlgFileOpen.h:58-65`)
  and implements custom file selection/acceptance (`DlgFileOpen.cpp:45-120`);
  an owned path value removes truncation and buffer-lifetime hazards.
