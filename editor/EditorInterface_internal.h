#include "Gates/preferences_legacy.h"
// Shared surface of the EditorInterface translation units (EditorInterface*.cpp).
//
// EditorInterface.cpp keeps key handling, camera input, painting, save/erase and the per-frame update/render;
// the rest of the implementation is split by responsibility.
#ifndef EDITORINTERFACE_INTERNAL_H
#define EDITORINTERFACE_INTERNAL_H

#include <cstdio>
#include <cstdarg>
#include <string>
#include "stdafx.h"
#include "EditorInterface.h"
#include "dstd.h"

#pragma warning( disable:4201 )
#include "mmsystem.h"
#pragma warning( default:4201 )

#ifndef EDITORCAMERA_H
#include "EditorCamera.h"
#endif

#ifndef EDITORMESSAGES_H
#include "EditorMessages.h"
#endif

#ifndef EDITORDATA_H
#include "EditorData.h"
#endif
#include "EditorNavLayer.h"

// S2.9: gpu_cull::substrate_frameBegin() hoist into editor's per-tick update.
// Mirrors game commit f8d6b171's fix in code/mission.cpp (~line 527): substrate
// must reset every tick regardless of pause/active state, or render-time
// submitMultiShape calls accumulate records across an un-reset ring slot,
// inflating per-bucket instanceCount in compute cull and causing coalesce
// MDI sub-draws to read adjacent types' modelMatrices — each prop's origin
// renders layered copies of other types' geometry.
#include "../GameOS/gameos/gpu_cull_substrate.h"

#ifndef OVERLAYBRUSH_H
#include "OverlayBrush.h"
#endif

#ifndef ACTION_H
#include "Action.h"
#endif

#ifndef TERRAINBRUSH_H
#include "TerrainBrush.h"
#endif

#ifndef UTILITIES_H
#include "Utilities.h"
#endif

#ifndef BUILDINGBRUSH_H
#include "BuildingBrush.h"
#endif

#ifndef ERASER_H
#include "Eraser.h"
#endif

#ifndef SELECTIONBRUSH_H
#include "SelectionBrush.h"
#endif

#ifdef MC2_IMGUI
#include "EditorInspector.h"
#include "imgui.h"
#include "MapGeneratorDialog.h"
#include "MissionValidation.h"
#include "EditorTaskRunner.h"
#include "EditorPlaytest.h"
#include "EditorDebugOverlay.h"
#include "ModPicker.h"
#include "EditorModProject.h"
#include "EditorRecent.h"
#include "SceneOutliner.h"
#include "BeautySidecarPreview.h"
#include "UnitActionFlowPanel.h"
#include "InspectorPanel.h"
#include "ObjectivesSummaryPanel.h"   // read-only objectives explainer
#include "CampaignSummaryPanel.h"     // read-only campaign explainer
#include "UnitBrainPanel.h"   // AI / Brain / Orders inspector panel
#include "TelemetryPanel.h"
#include "AssetBrowser.h"
#include "GameplayDebugger.h"
#include "EditorPlaytestResults.h"
#include "UndoHistoryPanel.h"
#include "CommandPalette.h"
#include "gameplay_pick.h"  // tryGameplayPick: shared pick spine, no game-object deps
#include "../EditorBridge/EditorRenderBridge.h"  // GPU ObjectID pick seam
#include "gameos.hpp"       // gos_GetViewport, Environment (drawableWidth/Height)
#include "gos_render.h"     // graphics::make_current_context
#include "gos_postprocess.h" // gosPostProcess + getGosPostProcess() — OID scan diagnostic
#include "GuiRuntime.h"      // RTT viewport rect (central-node origin) for pick offset
// g_imguiInitialized is defined in GuiRuntime/GuiRuntime.cpp.
extern bool g_imguiInitialized;

#endif

#ifndef FLATTENBRUSH_H
#include "FlattenBrush.h"
#include "HeightBrush.h"
#include "ScatterBrush.h"
#include "StampBrush.h"
#include "FoliageRender.h"
#include "object_recent_ring.h"
#include "../GameOS/gameos/gos_terrain_water_stream.h"
#endif

#ifndef HEIGHTDLG_H
#include "HeightDlg.h"
#endif

#include "SelectSlopeDialog.h"

#ifndef TERRAINDLG_H
#include "TerrainDlg.h"
#endif

#ifndef MAPSIZEDLG_H
#include "MapsizeDlg.h"
#endif

#include "..\resource.h"

#ifndef SUNDLG_H
#include "sunDlg.h"
#endif

#ifndef FOGDLG_H
#include "FogDlg.h"
#endif

#ifndef AFX_WATERDLG_H
#include "WaterDlg.h"
#include "EditorResourceFallback.h"
#include "EditorVersion.h"
#include "EditorResourceCatalog.h"
#endif

#ifndef VERTEX_H
#include "Vertex.h"
#endif

#include "NewSingleMission.h"

#ifndef SINGLEVALUEDLG_H
#include "SingleValueDlg.h"
#endif

#include "MissionSettingsDlg.h"

#include "PlayerSettingsDlg.h"

#include "SelectTerrainTypeDlg.h"

#include "FractalDialog.h"

#ifndef EDITOROBJECTS_H
#include "EditorObjects.h"
#endif

#ifndef MESSAGEBOX_H
#include "MessageBox.h"
#endif

#ifndef DRAGTOOL_H
#include "DragTool.h"
#endif

#ifndef DAMAGEBRUSH_H
#include "DamageBrush.h"
#endif

#ifndef MINEBRUSH_H
#include "MineBrush.h"
#endif

#include "resource.h"

#ifndef LINKBRUSH_H
#include "Linkbrush.h"
#endif
#ifndef DROPZONEBRUSH_H
#include "DropZoneBrush.h"
#endif

#include "UnitSettingsDlg.h"
#include "BuildingSettingsDlg.h"
#include "TilingFactorsDialog.h"

#include "MFCPlatform.hpp"

#include "MainFrm.h"

#include "EditorObjects.h"
#include "ForestDlg.h"
#include "EditForestDlg.h"

#include "CampaignDialog.h"
#include "../GameOS/gameos/gos_crashbundle.h"  // shutdown breadcrumbs

extern bool silentMode;

#include "../ARM/Microsoft.Xna.Arm.h"
using namespace Microsoft::Xna::Arm;

#ifndef AssetType_Virtual
#define AssetType_Virtual AssetType_Physical
#endif

#ifndef ProviderType_Secondary
#define ProviderType_Secondary ProviderType_Primary
#endif


#pragma warning( disable:4244 )

// Globals defined in EditorInterface.cpp (or elsewhere in the engine) that the split pieces share.
extern IProviderEngine * armProvider;
extern CameraPtr eye;
extern bool drawTerrainTiles;
extern long terrainLineChanged;
extern bool drawTerrainOverlays;
extern bool renderObjects;
extern bool renderTrees;
extern bool drawTerrainGrid;
extern bool drawEditorPassability;
extern bool drawLOSGrid;
extern bool useClouds;
extern bool	useFog;
extern long lightState;
extern bool reloadBounds;
extern bool s_bSensorMapEnabled;
extern bool justResaveAllMaps;
extern bool bIsLoading;
extern volatile int ProcessingError;
void EditorShutdownTrace(const char* fmt, ...);
extern bool gActive;
extern bool gGotFocus;
extern Editor* editor;
EditorObject* EditorInterface_PickObjectAtScreenPoint(int screenX, int screenY);
bool EditorInterface_SelectObjectAtScreenPoint(int screenX, int screenY, bool toggle);

const float HSCROLLBAR_RANGE = 30000.0;
const float VSCROLLBAR_RANGE = 30000.0;

extern char missionName[];

inline char szTGAFilter[] = "TGA Files (*.TGA)|*.tga||";
inline char szPAKFilter[] = "Pak Files (*.PAK)|*.pak||";

inline bool windowSizeChanged = false;
inline float g_newWidth = 0.0;
inline float g_newHeight = 0.0;

static const float SQRT_2 = 1.4142135623730950488016887242097f;

// by Methuselas: the embedded GL child forwards mouse messages back here, but
// click-select and double-click settings must be resolved by the MFC Editor
// shell.  Drag-select already works through the brush path; these small
// click-state helpers only rescue non-drag left clicks so the remastered child
// window does not leave single-click and double-click dependent on stale terrain
// inverse-pick behavior.
inline int s_editorLeftDownX = 0;
inline int s_editorLeftDownY = 0;
inline bool s_editorLeftButtonTracking = false;
inline DWORD s_editorLastObjectClickTime = 0;
inline int s_editorLastObjectClickX = 0;
inline int s_editorLastObjectClickY = 0;

EditorObject* EditorInterface_PickObjectAtScreenPoint(int screenX, int screenY);
EditorObject* EditorPickObjectAtScreen(int screenX, int screenY);
bool EditorInterface_SelectObjectAtScreenPoint(int screenX, int screenY, bool toggle);

#endif // EDITORINTERFACE_INTERNAL_H
