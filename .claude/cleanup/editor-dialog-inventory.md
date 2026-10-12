# Mission editor dialog and workflow inventory

Read-only inventory at `HEAD e9d18b8a7`. Count command:
`rg -n 'class [A-Za-z_][A-Za-z0-9_]*.*CDialog' editor | Measure-Object`
returns **37 dialog classes**. `rg -o 'CDialog\b' editor | Measure-Object`
returns 181 literal `CDialog` occurrences; `rg -o 'DDX_Text\(' editor |
Measure-Object` returns **93 text bindings**. The dialog list below is all 37
class declarations from the first command. DDX members are the controls/values
bound by each class, with their `DoDataExchange` source lines. Fields named
without DDX are copied by the dialog's explicit code.

`scripts/run_editor_smoke.py` declares 13 named cases at `:339-354` and adds
`gen_save_load` at `:381-384`; it launches the editor with CLI flags and checks
logs (`:118-167, 339-354`). `rg -n 'DoModal|IDD_|C(BooleanFlagIsSetDialog|BuildingSettingsDlg|CCampaignDialog|ChooseBuildingDlg|ChooseUnitDlg|DlgFileOpen|EditForestDlg|EditorTacMap|FogDlg|ForestDlg|FractalDlg|CGroupDialog|HeightDlg|MapSizeDlg|CMissionDialog|MissionSettingsDlg|NewSingleMission|ObjectiveDlg|CObjectiveTemplateDlg|ObjectivesDlg|PlayerSettingsDlg|PointerSelectObjectDlg|ResourceStringSelectionDlg|SelectTerrainTypeDlg|SingleValueDlg|SelectSlopeDialog|SunDlg|TargetAreaDlg|TerrainDlg|TextMessageDlg|CUserTextEdit|UnitSettingsDlg|TilingFactorsDialog|ViewConditionOrActionDlg|WaterDlg|WaveDlg)' scripts/run_editor_smoke.py`
returns 0 hits: **none of these MFC dialogs is directly reached by this smoke**.
Modal status below is from the editor call-site audit (`rg -n 'DoModal\(' editor`);
the exception is the modeless tac map (`EditorInterface.cpp:1249-1253`).

## Terrain paint and map environment

| Dialog | Fields / document values | Mode / smoke / SDL+ImGui destination |
|---|---|---|
| `TerrainDlg` — `editor/TerrainDlg.h:19`; manual `terrain`, `TerrainDlg.cpp:29-56` | Initial map terrain index; map creation reads this selection (`EditorInterface_editor.cpp:100-109`). | Modal / no / terrain creation panel. |
| `MapSizeDlg` — `editor/mapsizedlg.h:19`; manual `mapSize`, `mapsizedlg.cpp:23-60` | Map dimension selection. | Modal / no / terrain dimensions panel. |
| `HeightDlg` — `editor/HeightDlg.h:20`; manual `min`, `max`, `HeightDlg.cpp:18-38` | Terrain height range. | Modal / no / terrain height panel. |
| `FractalDlg` — `editor/fractaldialog.h:18`; `m_threshold`, `m_noise`, `fractaldialog.cpp:22-27` | Terrain fractal threshold/noise inputs. | Modal / no / terrain generation panel. |
| `SelectSlopeDialog` — `editor/SelectSlopeDialog.h:17`; `m_MaxEdit`, `m_MinEdit`, `SelectSlopeDialog.cpp:32-37` | Slope selection bounds. | Modal / no / terrain selection panel. |
| `SelectTerrainTypeDlg` — `editor/SelectTerrainTypeDlg.h:17`; list control `m_terrainTypeList`, `.cpp:43-47` | Selected terrain type for terrain tools. | Modal / no / terrain palette. |
| `FogDlg` — `editor/FogDlg.h:18`; `m_blue`, `m_green`, `m_red`, `m_start`, `m_end`, `FogDlg.cpp:29-37` | Fog color and start/end distances. | Modal / no / atmosphere panel. |
| `SunDlg` — `editor/SunDlg.h:20`; `m_LightColor`, `m_AmbientEdit`, `m_Yaw`, `m_InitialSunPitch`, `sunDlg.cpp:36-43` | Sun direction and light/ambient colors; includes terrain-shadow/refractalize actions (`sunDlg.cpp:88-119,226-244`). | Modal / no / lighting panel. |
| `WaterDlg` — `editor/WaterDlg.h:17`; `amplitude`, `frequency`, `elevation`, `shallowElevation`, `alphaElevation`, `alphaDeep`, `alphaMiddle`, `alphaShallow`, `WaterDlg.cpp:40-52` | Water surface/wave and shallow/deep alpha settings. | Modal / no / water panel. |
| `WaveDlg` — `editor/WaveDlg.h:17`; manual public wave values, `WaveDlg.cpp:12-78` | Legacy wave amplitude/frequency/elevation/color values. `rg -n 'WaveDlg' editor` finds only its declaration/implementation: no live menu caller was found. | Uncalled legacy modal class / no / merge into water panel only after restoring a caller. |
| `TilingFactorsDialog` — `editor/TilingFactorsDialog.h:17`; `m_TerrainDetailTilingFactor`, `m_WaterDetailTilingFactor`, `m_WaterTilingFactor`, `.cpp:33-39` | Terrain/water texture tiling factors. | Modal / no / material tiling panel. |
| `ForestDlg` — `editor/ForestDlg.h:17`; density/height min/max, random flag, X/Y/radius, name and slots `m_1..m_10,m_12..m_15`, `ForestDlg.cpp:50-95` | Forest brush placement and per-forest parameters. | Modal / no / foliage painting panel. |
| `EditForestDlg` — `editor/EditForestDlg.h:17`; file list and `m_Name`, `EditForestDlg.cpp:37-42` | Forest definitions: load/save/rename and selected definition (`EditForestDlg.cpp:73-136,184-231`). | Modal / no / foliage definition panel. |
| `EditorTacMap` — `editor/EditorTacMap.h:26`; `picture`, `EditorTacMap.cpp:31-35` | Read-only map image/navigation; `EditorInterface` owns it as a member (`EditorInterface.h:405`). | **Modeless** `Create`/`ShowWindow` (`EditorInterface.cpp:1249-1253`) / no / persistent minimap panel. |

## Object placement and team assignment

| Dialog | Fields / document values | Mode / smoke / SDL+ImGui destination |
|---|---|---|
| `UnitSettingsDlg` — `editor/UnitSettingsDlg.h:21`; variant, pilot, mech, group, alignment, squad, self-repair, `.cpp:31-41` | Selected unit's type, assignment, alignment, squad and behavior; edits through `applyChanges`/undo (`UnitSettingsDlg.cpp:216-362`). | Modal / no / unit inspector and placement defaults. |
| `BuildingSettingsDlg` — `editor/BuildingSettingsDlg.h:21`; mech, group, alignment, X/Y, part ID, forest, damaged, `.cpp:38-49` | Selected building's position, type/part, group, forest and damage state; `applyChanges`/undo (`BuildingSettingsDlg.cpp:85-209`). | Modal / no / building inspector and placement defaults. |
| `ChooseUnitDlg` — `editor/ChooseUnitDlg.h:17`; pointer-based selected `Unit*`, no DDX (`ChooseUnitDlg.cpp:25-37,96-169`) | Choose or pointer-pick a unit reference for an objective predicate (`Objective_conditions.cpp:131,186`). | Modal / no / objective entity-reference picker. |
| `ChooseBuildingDlg` — `editor/ChooseBuildingDlg.h:17`; pointer-based selected `EditorObject*`, no DDX (`ChooseBuildingDlg.cpp:25-35,92-165`) | Choose or pointer-pick a building reference for an objective predicate (`Objective_conditions.cpp:257`). | Modal / no / objective entity-reference picker. |
| `PointerSelectObjectDlg` — `editor/PointerSelectObjectDlg.h:15`; map-pick state/button, no DDX (`PointerSelectObjectDlg.cpp:25-66`) | Uncalled class: `rg -n 'PointerSelectObjectDlg' editor` finds definitions and an include (`ChooseUnitDlg.cpp:18`), no instantiation. Actual chooser handoff toggles selection-only mode and consumes selected objects (`ChooseUnitDlg.cpp:125-159`; `ChooseBuildingDlg.cpp:121-155`). | Uncalled modal class / no / shared viewport picker supplied by the active chooser workflow. |
| `PlayerSettingsDlg` — `editor/PlayerSettingsDlg.h:17`; `m_defaultTeamComboBox`, `m_playerEdit`, `.cpp:34-39` | Player identity and default team (`EditorInterface_terrain.cpp:629-638`). | Modal / no / teams panel. |

## Objectives and ABL-facing authoring

| Dialog | Fields / document values | Mode / smoke / SDL+ImGui destination |
|---|---|---|
| `ObjectivesDlg` — `editor/ObjectivesDlg.h:53`; objective list/buttons/team, `.cpp:35-45` | Team's objective collection and ordering; add/edit/copy/remove/move actions (`ObjectivesDlg.cpp:54-56,142-245`). | Modal / no / team objectives panel. |
| `ObjectiveDlg` — `editor/ObjectiveDlg.h:22`; priority, resource points, prerequisite flags, marker/hidden/activation/reset flags, X/Y, title/description and resource IDs, plus action/condition lists, `ObjectiveDlg.cpp:62-95` | Individual objective state and success/failure condition/action lists (`Objective.cpp:649`). | Modal / no / objective detail panel with condition/action editors. |
| `CObjectiveTemplateDlg` — `editor/ObjectivesDlg.h:21`; preset combo `m_PresetCombo`, `ObjectivesDlg.cpp:68-79` | Preset choice only; preset constructs a new objective (`ObjectivesDlg.cpp:231-263`). | Modal / no / objective templates panel. |
| `CBooleanFlagIsSetDialog` — `editor/BooleanFlagIsSetDialog.h:17`; `m_Value`, `m_FlagID`, `.cpp:32-37` | ABL flag condition's boolean value and flag ID (`Objective_conditions.cpp:493-502`). | Modal / no / condition editor. |
| `SingleValueDlg` — `editor/SingleValueDlg.h:20`; manual `val`, caption and static text (`SingleValueDlg.cpp:24-54`) | Numeric argument such as objective thresholds, terrain elevation or smoothing radius (`Objective_conditions.cpp:49`; `EditorInterface_terrain.cpp:438,467`). | Modal / no / typed condition/property editor. |
| `TargetAreaDlg` — `editor/TargetAreaDlg.h:17`; reference-bound center X/Y/radius, no DDX (`TargetAreaDlg.cpp:23,122-143`) | Objective target-area center and radius; map pick updates center (`:150-212`). | Modal / no / target-area editor attached to viewport. |
| `TextMessageDlg` — `editor/TextMessageDlg.h:17`; `m_TextMessage`, `TextMessageDlg.cpp:31-35` | Objective/message action text (`Objective_actions.cpp:180-189`). | Modal / no / localized text editor. |
| `ResourceStringSelectionDlg` — `editor/ResourceStringSelectionDlg.h:19`; resource combo `m_Combo`, `.cpp:45-49` | Resource-string ID selection (`Objective_actions.cpp:228-238`). | Modal / no / localization picker. |
| `CUserTextEdit` — `editor/UserTextEdit.h:17`; text and resource-string ID, `UserTextEdit.cpp:47-52` | Dual-mode authored text or localized resource ID, used by campaign, mission and objective labels (`CampaignDialog.cpp:389-400`; `MissionSettingsDlg.cpp:251-281`; `ObjectiveDlg.cpp:1293-1314`). | Modal / no / localization field editor. |
| `ViewConditionOrActionDlg` — `editor/ViewConditionOrActionDlg.h:17`; `m_Edit`, `.cpp:31-35` | Displays serialized condition/action text for inspection; objective handlers open it (`ObjectiveDlg.cpp:1438-1493`). | Modal / no / read-only condition/action inspector. |

## Campaign graph and mission export/settings

| Dialog | Fields / document values | Mode / smoke / SDL+ImGui destination |
|---|---|---|
| `CCampaignDialog` — `editor/CampaignDialog.h:21`; group list, CBills, final video, name and name resource ID, `.cpp:44-51` | Campaign identity, rewards/video and linked group ordering (`CampaignDialog.cpp:173-228,301-370`). | Modal / no / campaign graph panel. |
| `CGroupDialog` — `editor/GroupDialog.h:19`; tune, mission list, operation/prevideo/video files, label, minimum missions, `.cpp:35-44` | Campaign group metadata and mission links (`GroupDialog.cpp:146-223`). | Modal / no / campaign group panel. |
| `CMissionDialog` — `editor/MissionDialog.h:17`; mission/purchase file paths and logistics/mandatory/promotion/purchase/salvage/selection flags, `.cpp:41-52` | Campaign entry's mission link and campaign behavior flags. It is nested inside group add/edit (`GroupDialog.cpp:146-180,207-223`). | Modal / no / campaign mission-link panel. |
| `MissionSettingsDlg` — `editor/MissionSettingsDlg.h:17`; author, player/team limits, time, initial resource/CBills, mission type, name/blurb and resource IDs, video/URL, gameplay options, `.cpp:82-123` | Mission `.fit` settings/metadata and feature flags (`EditorInterface_terrain.cpp:534-553`). | Modal / no / mission settings panel. |
| `NewSingleMission` — `editor/newsinglemission.h:17`; no DDX fields; load/new/map-generator commands and mod choice (`EditorInterface_editor.cpp:35-123`; `newsinglemission.h:29-48`) | Selects mission creation/load path and active mod, not a field in the current document. | Modal / no / project start/open panel. |
| `DlgFileOpen` — `editor/DlgFileOpen.h:19`; manual file name, extension, directory and save/load mode; fixed buffers `fileName[MAX_PATH]`, `m_directory[MAX_PATH]`, `.h:58-65`; selection/accept logic `.cpp:45-120` | Mission/campaign and asset file selection; path result feeds the owning workflow. | Modal / no / file browser integrated into project/campaign panels. |

## Other

| Dialog | Fields / document values | Mode / smoke / SDL+ImGui destination |
|---|---|---|
| `CAboutDlg` — `editor/EditorMFC.cpp:659`; no document fields, `.cpp:673-688` | About/version content. | Modal / no / About overlay. |

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
