/***************************************************************
* FILENAME: EditorInterface.cpp
* DESCRIPTION: Implements the MFC Editor interface, input routing, and UI shell.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 05/16/2026
* MODIFICATION: by Methuselas
* CHANGES: Remove temporary Editor init/dialog trace calls after port validation.
****************************************************************/

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
#include "../EditorBridge/EditorRenderBridge.h"  // EDITOR-OBJECTID-PICK-BRIDGE-1: GPU ObjectID pick seam
#include "gameos.hpp"       // gos_GetViewport, Environment (drawableWidth/Height)
#include "gos_render.h"     // graphics::make_current_context
#include "gos_postprocess.h" // gosPostProcess + getGosPostProcess() — OID scan diagnostic
#include "GuiRuntime.h"      // RTT viewport rect (central-node origin) for pick offset
// g_imguiInitialized is defined in GuiRuntime/GuiRuntime.cpp.
extern bool g_imguiInitialized;

// RTT viewport pick offset: the editor scene renders into the dockspace CENTRAL node
// (a sub-rect of the GL-child window), not the full window. Full-window client mouse
// coords must be shifted into the central-node's local space before any screen->world
// unproject, else the placement cursor / object pick diverge from the mouse (the
// historical "2x toward the right" bug). No-op when RTT is off (origin 0,0 == full).
static inline void EditorRttClientToViewport(int& x, int& y)
{
    if (!g_imguiInitialized || !GuiRuntime::RttEnabled())
        return;
    if (GuiRuntime::ViewportRectW() <= 0 || GuiRuntime::ViewportRectH() <= 0)
        return;
    x -= GuiRuntime::ViewportRectX();
    y -= GuiRuntime::ViewportRectY();
}
// Render context handle for explicit GL context bind before glReadPixels.
// SDL_PollEvent inside RunGameOSLogic fires before make_current_context;
// any WM_LBUTTONUP that arrives between RunGameOSLogic iterations needs
// the context made current before tryGameplayPick calls glReadPixels.
extern graphics::RenderContextHandle EditorGameOS_GetRenderContext();
#else
// Non-ImGui editor build: no RTT viewport, pick offset is a no-op.
static inline void EditorRttClientToViewport(int&, int&) {}
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
#include "../GameOS/gameos/gos_crashbundle.h"  // EDITOR-EXIT-CRASH-1: shutdown breadcrumbs

extern bool silentMode;

// ---------------------------------------------------------------------------
// EDITOR-EXIT-CRASH-1 shutdown breadcrumbs.
// Mirrors EditorData.cpp's EditorDataTrace: always feeds the crashbundle ring
// buffer (so the last ~64KB of activity lands in crashes/<ts>/last_trace.txt
// on any CTD, including one during exit), and additionally logs to
// editor-startup.log when MC2_EDITOR_TRACE is set for interactive debugging.
// ---------------------------------------------------------------------------
static void EditorShutdownTrace(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	crashbundle_append(buf);

	if (getenv("MC2_EDITOR_TRACE") == NULL)
		return;

	FILE* f = fopen("editor-startup.log", "a");
	if (!f)
		return;
	fputs(buf, f);
	fputc('\n', f);
	fclose(f);
}

// ARM
#include "../ARM/Microsoft.Xna.Arm.h"
using namespace Microsoft::Xna::Arm;

#ifndef AssetType_Virtual
#define AssetType_Virtual AssetType_Physical
#endif

#ifndef ProviderType_Secondary
#define ProviderType_Secondary ProviderType_Primary
#endif

IProviderEngine * armProvider;


#pragma warning( disable:4244 )

//-----------------------------------
// Frank at work!
CameraPtr eye = NULL;

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
extern bool useShadows;
extern bool useWaterInterestTexture;
long lightState = 0;
extern bool useFaceLighting;
extern bool useVertexLighting;
extern bool reloadBounds;
bool s_bSensorMapEnabled = false;
extern bool justResaveAllMaps;
extern bool bIsLoading;
EditorInterface* EditorInterface::s_instance = NULL;

extern volatile int ProcessingError;

const float HSCROLLBAR_RANGE = 30000.0;
const float VSCROLLBAR_RANGE = 30000.0;

extern char missionName[];

static char szTGAFilter[] = "TGA Files (*.TGA)|*.tga||";
static char szPAKFilter[] = "Pak Files (*.PAK)|*.pak||";

static bool windowSizeChanged = false;
static float g_newWidth = 0.0;
static float g_newHeight = 0.0;

// EDITOR stuff

void Editor::init( char* loader )
{

	volatile float crap = 0;
	bool bOK = false;

	if ( !eye )
	{
		eye = new EditorCamera;
	}

	Pilot::initPilots();

	FullPathFileName bdgFileName;
	bdgFileName.init(artPath,"Buildings",".csv");

	FullPathFileName objFileName;
	objFileName.init(objectPath,"Object2",".pak");

	EditorObjectMgr* pMgr = EditorObjectMgr::instance();

	pMgr->init( bdgFileName, objFileName );

	FullPathFileName mPath;
	mPath.init(missionPath,missionName,".pak");

	// S-CLI override: if -mission was passed on the command line, use that path
	// directly instead of (missionPath, missionName, ".pak"). This must happen
	// BEFORE the fileExists check so the NewSingleMission modal dialog (below)
	// is skipped -- the dialog has its own message pump and OnIdle never fires
	// during it, so the auto-load OnIdle hook can never run.
	extern std::string g_cliMissionPath;
	if (!g_cliMissionPath.empty()) {
		mPath.init("", g_cliMissionPath.c_str(), "");
	}

	// CLI auto-generate paths (-gen-map / -new-map) leave g_cliMissionPath empty,
	// so without this they fall into the interactive NewSingleMission chooser below
	// -- a modal with its own message pump that OnIdle never breaks out of, hanging
	// any headless/automated launch. Skip the chooser and let the OnIdle auto-load
	// hook (EditorMFC.cpp) run generateMission/initTerrainFromTGA on first idle.
	extern bool g_cliGenMap;
	extern bool g_cliNewMap;

	bool bCanceled = false;
	if (!justResaveAllMaps)
	{
				bool missionExists = fileExists(mPath);

		if (missionExists)
		{
			bOK = EditorData::initTerrainFromPCV(mPath);
		}
		else if (g_cliGenMap || g_cliNewMap)
		{
			bOK = true;   // defer to the OnIdle auto-generate hook; no modal
		}
		else
		{
			NewSingleMission dlg;
			bool resolved = false;
			while (!resolved)
			{
				int retVal = dlg.DoModal();

				if ( retVal == IDCANCEL )
				{
					if (EditorInterface::instance())
								{
									EditorInterface::instance()->SetBusyMode();
								}
								else
								{
								}
					resolved = true;
					gos_TerminateApplication();
					PostQuitMessage(0);
					bOK = true;
					bCanceled = true;
					if (EditorInterface::instance())
								{
									EditorInterface::instance()->UnsetBusyMode();
								}
								else
								{
								}
				}
				else if ( retVal == ID_NEWMISSION )
				{
					resolved = true;

					while ( !bOK )
					{
						TerrainDlg dlg;
						dlg.terrain = 0;

						int terrainRet = dlg.DoModal();

						if ( IDOK == terrainRet )
						{
							MapSizeDlg msdlg;
							msdlg.mapSize = 0;
							int mapRet = msdlg.DoModal();

							if ( IDOK == mapRet )
							{
								char path[256];
								strcpy( path, cameraPath );
								strcat( path, "cameras.fit" );

								FitIniFile camFile;
								long camResult = camFile.open( path );
								if ( NO_ERR != camResult )
								{
									// STOP(( "Need Camera File " ));
								}

								if (EditorInterface::instance())
								{
									EditorInterface::instance()->SetBusyMode();
								}
								else
								{
								}

								eye->init( &camFile );

								bOK = EditorData::initTerrainFromTGA( msdlg.mapSize, 0, 0, dlg.terrain );

								if (EditorInterface::instance())
								{
									EditorInterface::instance()->UnsetBusyMode();
								}
								else
								{
								}

								if ( !bOK )
								{
									resolved = false;
									break;
								}
							}
							else
							{
								resolved = false;
								break;
							}
						}
						else
						{
							resolved = false;
							break;
						}
					}
				}
				else if ( retVal == ID_MAPGENERATOR )
				{
					resolved = true;
					{
						char camPath[256];
						strcpy( camPath, cameraPath );
						strcat( camPath, "cameras.fit" );
						FitIniFile camFile;
						long camResult = camFile.open( camPath );
						if ( NO_ERR != camResult ) { /* cameras.fit missing */ }
						else eye->init( &camFile );
					}
					bOK = EditorData::initTerrainFromTGA( 3, 0, 0, 0 );
#ifdef MC2_IMGUI
					if ( bOK )
						MapGeneratorDialog::Open();
#endif
				}
				else
				{
					resolved = true;

					// Recent-mission shortcut: the startup dialog put the chosen path
					// here, so load it directly and skip the file browser. Falls back to
					// browse when empty.
					extern std::string g_editorPreselectMission;
					if ( !g_editorPreselectMission.empty() )
					{
						std::string preset = g_editorPreselectMission;
						g_editorPreselectMission.clear();
						if (EditorInterface::instance()) EditorInterface::instance()->SetBusyMode();
						bOK = EditorData::initTerrainFromPCV( preset.c_str() );
						if (EditorInterface::instance()) EditorInterface::instance()->UnsetBusyMode();
						if ( bOK )
							EditorRecent::Push( preset.c_str() );
						else
							resolved = false;   // re-show the startup dialog on failure
					}
					else
					{
					CFileDialog fileDlg( 1,  "pak", NULL, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR, szPAKFilter );
					// When a Mod Project is open, default to <root>\data\missions; else unchanged.
					{ const char* pd = EditorModProject::SaveDirOverride();
					  fileDlg.m_ofn.lpstrInitialDir = pd ? pd : missionPath; }

					bOK = false;
					while ( !bOK )
					{
						if (  IDOK == fileDlg.DoModal() )
						{
							if (EditorInterface::instance())
								{
									EditorInterface::instance()->SetBusyMode();
								}
								else
								{
								}
							const char* pFile = fileDlg.m_ofn.lpstrFile;
							bOK = EditorData::initTerrainFromPCV( pFile );
							if (EditorInterface::instance())
								{
									EditorInterface::instance()->UnsetBusyMode();
								}
								else
								{
								}
							if ( bOK )
								EditorRecent::Push( pFile );
						}
						else
						{
							resolved = false;
							break;
						}
					}
					}
				}
			}
		}
	}
	else
	{
		bOK = true;
	}


	if (bCanceled)
	{
		if ( !land )
		{
			land = new Terrain( );
						land->init( 0, (PacketFile*)NULL, EDITOR_VISIBLE_VERTICES, crap, 100  );
		}
	}

	if (bOK)
	{
		if (EditorInterface::instance())
	{
		EditorInterface::instance()->init( loader );
	}
	else
	{
		/*
			Editor migration:

			In the original MFC editor, EditorInterface is a real window object and
			its constructor registers s_instance before Editor::init reaches this
			point.

			In the Remastered SDL startup path, the editor can reach map/bootstrap
			logic before the MFC interface object exists. Calling
			EditorInterface::instance()->init(...) when instance() is NULL passes a
			null this pointer into EditorInterface::init, which crashes as soon as
			that method writes to member fields such as rotationInc.

			For the current migration stage, skip interface INI initialization until
			the window/interface construction path is restored. This keeps terrain
			and data initialization alive without dereferencing a missing UI shell.
		*/
	}
	}

	PlaySound("SystemDefault",NULL,SND_ASYNC);
}


void Editor::destroy (void)
{
	EditorShutdownTrace("[EDITOR_SHUTDOWN] phase=Editor_destroy_enter");

	if (EditorInterface::instance())
	{
		EditorInterface::instance()->terminate();
	}

	/*
	EditorData::clear()
	clears the ActionUndoMgr,
	terminates and deletes "land",
	and clears EditorObjectMgr
	among other things
	*/
	EditorData::clear();
	EditorShutdownTrace("[EDITOR_SHUTDOWN] phase=Editor_destroy_data_cleared");

	if ( eye )
		delete eye;
	eye = NULL;
	EditorShutdownTrace("[EDITOR_SHUTDOWN] phase=Editor_destroy_exit");
}

void Editor::resaveAll (void)
{
	//Startup the editor.
	if ( !eye )
		eye = new EditorCamera;

	// ARM
	CoInitialize(NULL);
	// by Methuselas: EditorInterface joins the Editor-owned SemVer lane;
	// do not reintroduce the legacy provider identity here.
	armProvider = CreateProviderEngine("MC2Editor", (char*)EditorVersion_GetSemVer());
	silentMode = true; // shut up the warnings from tgl export

	char campaignFiles[2][256] = {0};
	strcpy(campaignFiles[0], "Data\\Campaign\\campaign.fit");
	strcpy(campaignFiles[1], "Data\\Campaign\\tutorial.fit");

	for (int i = 0; i < 2; i++) // once for the campaign, then the tutorial
	{
		CCampaignData campaignData;
		campaignData.Read(campaignFiles[i]);
		IProviderAssetPtr campaignAssetPtr = armProvider->OpenAsset(campaignFiles[i], 
			AssetType_Physical, ProviderType_Primary);

		campaignAssetPtr->AddProperty("Type", "Campaign");

		int count = 0;
		char buf[512] = {0};
		
		// for all groups
		for ( EList<CGroupData, CGroupData>::EConstIterator iter = campaignData.m_GroupList.Begin();
			!iter.IsDone(); count++, iter++ )
		{
			CGroupData groupData = (*iter);

			// Make a virtual group name
			strcpy(buf, campaignFiles[i]);
			int campaignLen = (int)strlen(buf);
			buf[campaignLen-4] = '_';
			buf[campaignLen-3] = 0;
			strcat(buf, "group");
			char groupId[4] = {0};
			sprintf(groupId, "%02d", count+1);
			strcat(buf, groupId);

			// Add a relationship from the campaign to the virtual group
			campaignAssetPtr->AddRelationship("Group", buf);
			
			// Open the virtual asset
			IProviderAssetPtr groupAssetPtr = armProvider->OpenAsset(buf, AssetType_Virtual, ProviderType_Primary);

			groupAssetPtr->AddProperty("Type", "Mission Group");
			
			// Add all the relationships

			if (groupData.m_OperationFile != "")
			{
				strcpy(buf, "Data\\Art\\");
				strcat(buf, groupData.m_OperationFile.GetBuffer());
				strcat(buf, ".fit");
				groupAssetPtr->AddRelationship("Operation", buf);			
			}

			if (groupData.m_ABLScript != "")
			{
				strcpy(buf, "Data\\Missions\\");
				strcat(buf, groupData.m_ABLScript.GetBuffer());
				strcat(buf, ".abl");
				groupAssetPtr->AddRelationship("ABLScript", buf);			
			}

			if (groupData.m_PreVideoFile != "")
			{
				strcpy(buf, "Data\\Movies\\");
				strcat(buf, groupData.m_PreVideoFile.GetBuffer());
				// The only instances of PreVideoFile I've seen had the extension already
				groupAssetPtr->AddRelationship("PreVideo", buf);			
			}

			if (groupData.m_VideoFile != "")
			{
				strcpy(buf, "Data\\Movies\\");
				strcat(buf, groupData.m_VideoFile.GetBuffer());
				strcat(buf, ".bik");
				groupAssetPtr->AddRelationship("Video", buf);			
			}

			// for all missions part of this group
			for ( EList<CMissionData, CMissionData>::EConstIterator iter2 = groupData.m_MissionList.Begin();
				!iter2.IsDone(); iter2++ )
			{
				CMissionData missionData = *(iter2);

				if (missionData.m_MissionFile != "")
				{
					strcpy(buf, "Data\\Missions\\");
					strcat(buf, missionData.m_MissionFile.GetBuffer());
					strcat(buf, ".fit");
					groupAssetPtr->AddRelationship("Mission", buf);			
				}

				if (missionData.m_PurchaseFile != "")
				{
					strcpy(buf, "Data\\Missions\\");
					strcat(buf, missionData.m_PurchaseFile.GetBuffer());
					strcat(buf, ".fit");
					IProviderRelationshipPtr rel = groupAssetPtr->AddRelationship("PurchaseFile", buf);	
					rel->AddProperty("Mission", missionData.m_MissionFile.GetBuffer());
				}
			}

			groupAssetPtr->Close();
		}

		campaignAssetPtr->Close();
	}

	Pilot::initPilots();

	char bdgFileName[256];
	strcpy( bdgFileName, artPath );
	strcat( bdgFileName, "Buildings.csv" );
	_strlwr(bdgFileName);

	char objFileName[256];
	strcpy( objFileName, objectPath );
	strcat( objFileName, "object2.pak" );
	_strlwr(objFileName);

	EditorObjectMgr* pMgr = EditorObjectMgr::instance();
	pMgr->init( bdgFileName, objFileName );

	//----------------------------------------------------------------------------
	//Recurse through the data\missions directory and resave every .PAK you find!
	char findString[512];
	sprintf(findString,"%s*.pak",missionPath);

	WIN32_FIND_DATA	findResult;
	HANDLE searchHandle = FindFirstFile(findString,&findResult);
	do
	{
		if ((findResult.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			char baseName[1024];
			_splitpath(findResult.cFileName,NULL,NULL,baseName,NULL);
			
			FullPathFileName pakName;
			pakName.init(missionPath,baseName,".pak");
			
			FullPathFileName fitName;
			fitName.init(missionPath,baseName,".fit");
			
			if (fileExists(pakName))
			{
				//Force Attributes to RW!
				SetFileAttributes(pakName,FILE_ATTRIBUTE_NORMAL);
				SetFileAttributes(fitName,FILE_ATTRIBUTE_NORMAL);
				long result = EditorData::initTerrainFromPCV(pakName);
				if (result)
					data.save(pakName);

				printf( "Resaved Map %s", (const char*)pakName );

				//Force Back to RO!
				//Leave RW per bug 1252
//				SetFileAttributes(pakName,FILE_ATTRIBUTE_READONLY);
//				SetFileAttributes(fitName,FILE_ATTRIBUTE_READONLY);
			}
		}
	} while (FindNextFile(searchHandle,&findResult) != 0);

	FindClose(searchHandle);

	// ARM
	CoUninitialize();
}

void Editor::render()
{
	if ( eye )
		eye->render();
}

void Editor::update()
{
	// S2.9 — Editor's analog to code/mission.cpp Mission::update pre-pause-branch
	// substrate_frameBegin (commit f8d6b171). Editor::update() is invoked
	// unconditionally from DoGameLogic() every tick (Editor.cpp:198) — no
	// pause/active/turn gate — so this fires every frame. Render-time
	// BldgAppearance/Mech3DAppearance::render() calls (driven from
	// EditorObjectMgr::render) append GpuActorRecords into the substrate ring;
	// without a per-frame reset the ring slot, per-frame counter, and bucket
	// counts grow unbounded and coalesced MDI sub-draws overrun their type
	// bucket, layering other props' geometry at every prop's origin.
	// Calling without isEnabled gating is safe: substrate_frameBegin()
	// internally checks isEnabled() and is a no-op when disabled. ONE call
	// site by design — function is not double-call-safe.
	gpu_cull::substrate_frameBegin();

	if (windowSizeChanged)
	{
		windowSizeChanged = false;
		// Editor renders at NATIVE window resolution: disable the game's 800x600
		// HUD render-base clamp (gos_SetScreenMode) so the GL viewport matches the
		// MFC window/mouse space. Without this the clamp leaves gos_GetViewport at
		// 800x600 while the cursor lives in the real ~2535x1265 window, and every
		// editor object pick / drag-move forward-projection is scaled wrong ->
		// objects unselectable after a camera move and the hand tool flings them
		// off-map on release. The editor has no legacy 2D HUD (it uses ImGui).
		gos_SetHudResClampEnabled(false);
		gos_SetScreenMode(g_newWidth, g_newHeight);
	}

	//interMgr.update();

	mcTextureManager->clearArrays();

	// Guard all land/eye terrain work — land is NULL until the generator
	// produces a map. EditorInterface::update() below still runs so that
	// MapGeneratorDialog::TakeAction() (Generate/Preview) is processed.
	if ( land )
	{
		eye->update();
		if (land->terrainTextures2 && !(land->terrainTextures2->colorMapStarted) && (EditorInterface::instance()))
		{
			if (EditorInterface::instance())
				EditorInterface::instance()->SetBusyMode(false/*no redraw*/);
			land->update();
			if (EditorInterface::instance())
				EditorInterface::instance()->UnsetBusyMode();
		}
		else
		{
			land->update();
		}

		// S2.13-surgical: hoist camera projection-state refresh from
		// EditorCamera::render() to BEFORE land->geometry().
		{
			// Use the SCENE viewport size (Environment.drawable*), NOT gos_GetViewport's
			// viewMul*. With the docked map-resize, the GL scene renders into a centralW
			// sub-region (Environment.drawableWidth) while the GOS viewport width stays
			// the full window -> camera projection + inverseProject picking would use
			// fullW while the render uses centralW, so the cursor and the picked point
			// diverge increasingly toward the right. Driving screenResolution from the
			// scene size unifies render + projection + picking. (Equal to viewMul* when
			// not docked, so undocked behavior is unchanged.)
			Stuff::Vector3D newRes;
			newRes.x = (float)Environment.drawableWidth;
			newRes.y = (float)Environment.drawableHeight;
			newRes.z = 0.0f;
			eye->changeResolution(newRes);
		}

		land->clearObjBlocksActive();
		land->geometry();
	}

	if (EditorInterface::instance())
	{
		EditorInterface::instance()->update();
	}
	EditorObjectMgr::instance()->update();

	// S2.15 — mirrors code/mission.cpp:549 (Mission::update). Without this
	// per-tick call the MC_TextureManager texture LRU never advances: textures
	// loaded at mission init (water base, water detail frames, terrain detail)
	// are not refreshed against `turn`, and the cache eviction / handle
	// integrity bookkeeping that the renderLists() + water fast-path bridge
	// relies on for stable gosTextureHandles can drift. Mission's chain calls
	// this every frame outside of pause; editor has no pause concept, so it
	// runs unconditionally. Safe and idempotent.
	mcTextureManager->update();
}

//--------------------------------------------------------------------------------------
BEGIN_MESSAGE_MAP(EditorInterface,CWnd )
	//{{AFX_MSG_MAP(EditorInterface)
	ON_WM_CREATE()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_MOUSEMOVE()
	ON_WM_SETCURSOR()
	ON_WM_KEYUP()
	ON_WM_RBUTTONDOWN()
	ON_WM_RBUTTONUP()
	ON_WM_MOUSEWHEEL()
	ON_WM_HSCROLL()
	ON_WM_VSCROLL()
	ON_WM_SYSKEYDOWN()
	ON_WM_LBUTTONDBLCLK()
	ON_WM_PAINT()
	ON_COMMAND(ID_VIEW_REFRESHTACMAP, OnViewRefreshtacmap)
	ON_UPDATE_COMMAND_UI(ID_MISSION_PLAYER_PLAYER3, OnUpdateMissionPlayerPlayer3)
	ON_UPDATE_COMMAND_UI(ID_MISSION_PLAYER_PLAYER4, OnUpdateMissionPlayerPlayer4)
	ON_UPDATE_COMMAND_UI(ID_MISSION_PLAYER_PLAYER5, OnUpdateMissionPlayerPlayer5)
	ON_UPDATE_COMMAND_UI(ID_MISSION_PLAYER_PLAYER6, OnUpdateMissionPlayerPlayer6)
	ON_UPDATE_COMMAND_UI(ID_MISSION_PLAYER_PLAYER7, OnUpdateMissionPlayerPlayer7)
	ON_UPDATE_COMMAND_UI(ID_MISSION_PLAYER_PLAYER8, OnUpdateMissionPlayerPlayer8)
	ON_UPDATE_COMMAND_UI(ID_MISSION_TEAM_TEAM3, OnUpdateMissionTeamTeam3)
	ON_UPDATE_COMMAND_UI(ID_MISSION_TEAM_TEAM4, OnUpdateMissionTeamTeam4)
	ON_UPDATE_COMMAND_UI(ID_MISSION_TEAM_TEAM5, OnUpdateMissionTeamTeam5)
	ON_UPDATE_COMMAND_UI(ID_MISSION_TEAM_TEAM6, OnUpdateMissionTeamTeam6)
	ON_UPDATE_COMMAND_UI(ID_MISSION_TEAM_TEAM7, OnUpdateMissionTeamTeam7)
	ON_UPDATE_COMMAND_UI(ID_MISSION_TEAM_TEAM8, OnUpdateMissionTeamTeam8)
	ON_WM_DESTROY()
	ON_COMMAND(ID_FOREST_TOOL, OnForestTool)
	ON_COMMAND(ID_OTHER_EDITFORESTS, OnOtherEditforests)
	ON_COMMAND(ID_FOLIAGE_GENERATE, OnFoliageGenerate)
	ON_COMMAND(ID_FOLIAGE_REGENSEL, OnFoliageRegenSel)
	ON_COMMAND(ID_FOLIAGE_CLEAR, OnFoliageClear)
	ON_COMMAND(ID_FOLIAGE_TOGGLE, OnFoliageToggle)
	ON_COMMAND(ID_VIEW_ORTHOGRAPHICCAMERA, OnViewOrthographiccamera)
	ON_COMMAND(ID_VIEW_SHOWPASSABILITYMAP, OnViewShowpassabilitymap)
	ON_WM_MBUTTONUP()
	ON_WM_TIMER()
	//}}AFX_MSG_MAP
	ON_UPDATE_COMMAND_UI_RANGE(0, 0xffff, UpdateButton ) 
	ON_COMMAND_RANGE( 0, 0xffff, OnCommand )
END_MESSAGE_MAP()



EditorInterface::EditorInterface()
{
	bThisIsInitialized = false;
	painting = false;
	curBrush = NULL;
	selecting = true;
	realRotation = 0.0;
	currentBrushID = IDS_SELECT;
	currentBrushMenuID = ID_OTHER_SELECT;

	m_pDragObject = NULL;
	m_dragObjMoved = false;
	m_pDragAction = NULL;
	m_dragStartX = 0;
	m_dragStartY = 0;
	m_dragStartWorld.x = m_dragStartWorld.y = m_dragStartWorld.z = 0.0f;
	m_dragObjStartPos.x = m_dragObjStartPos.y = m_dragObjStartPos.z = 0.0f;
	m_dragLastScreenX = 0;
	m_dragLastScreenY = 0;
	m_sculptRadius = 400.0f;
	m_sculptStrength = 30.0f;
	m_scatterMode = false;
	m_stampRadius = 400.0f;
	m_stampStrength = 50.0f;
	m_paintMaterialType = -1;       // no material brush active yet
	m_paintMaterialSize = 400.0f;   // reserved (TerrainBrush is single-vertex in Slice 1)
	m_waterHeight = 0.0f;
	m_pendGenerateMission = false;

	smoothRadius = 2;
	dragging = false;
	prevBrush = NULL;
	prevSelecting = false;
	highlighted = false;
	prevPainting = false;
	prevDragging = false;
	gosASSERT( !s_instance );
	s_instance = this;
	bSmooth = false;

	lastKey = -1;

	lastClickPos.x = lastClickPos.y = lastClickPos.z = 0.;
	hCursor = 0;

	bObjectSelectOnlyMode = false;
	menus = NULL;
	menusAllocatedCount = 0;
	m_hAccelTable = LoadAccelerators(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDR_ACCELERATOR1));

	m_hSplashBitMap = NULL;
	m_hBusyCursor = NULL;
	m_AppIsBusy = 0;
	m_bInRunGameOSLogicCall = false;
	rightDrag = 0;
}

EditorInterface::~EditorInterface()
{
	if (bThisIsInitialized)
	{
		terminate();
	}
	if (m_hBusyCursor) {
		/*this is done here because m_hBusyCursor is sometimes used before EditorInterface
		is "initialized" and after it is "terminated" */
		DestroyCursor(m_hBusyCursor);
		m_hBusyCursor = NULL;
	}
	if (m_hSplashBitMap) {
		DeleteObject(m_hSplashBitMap);
		m_hSplashBitMap = NULL;
	}
	s_instance = NULL;
}

void EditorInterface::terminate()
{
	if (bThisIsInitialized)
	{
		bThisIsInitialized = false;
		EditorShutdownTrace("[EDITOR_SHUTDOWN] phase=terminate_enter menus=%p menusAllocatedCount=%d", menus, menusAllocatedCount);

		if ( curBrush )
			delete curBrush; curBrush = NULL;

		// EDITOR-EXIT-CRASH-1 (#2): menus is NULL whenever addBuildingsToNewMenu()
		// was skipped (init() only calls it when land != NULL — the map-generator
		// path defers it to postTerrainInit() and can reach terminate() first on
		// a no-map/generator-aborted exit). Guard on menus != NULL, and free
		// exactly menusAllocatedCount entries (recorded at alloc time) instead of
		// re-querying getBuildingGroupCount(), which walks OOB/garbage if menus
		// was never allocated or the group count has since changed.
		if ( menus )
		{
			for ( int i = 0; i < menusAllocatedCount; ++i )
			{
				delete menus[i]; menus[i] = NULL;
			}

			free( menus );
			menus = NULL;
		}
		menusAllocatedCount = 0;
		EditorShutdownTrace("[EDITOR_SHUTDOWN] phase=terminate_menus_freed");

		objectivesEditState.Clear();
		EditorShutdownTrace("[EDITOR_SHUTDOWN] phase=terminate_exit");
	}
}

void EditorInterface::init( const char* fileName )
{
	SetBusyMode(false/*no redraw*/);

	FitIniFile loader;
	loader.open( const_cast<char*>(fileName) );

	long result = loader.seekBlock("CameraData");
	gosASSERT(result == NO_ERR);
	
	result = loader.readIdFloat("ScrollIncrement",scrollInc);
	gosASSERT(result == NO_ERR);
	
	result = loader.readIdFloat("RotationIncrement",rotationInc);
	gosASSERT(result == NO_ERR);
	
	result = loader.readIdFloat("ZoomIncrement",zoomInc);
	gosASSERT(result == NO_ERR);
		
	result = loader.readIdFloat("ScrollLeft",screenScrollLeft);
	gosASSERT(result == NO_ERR);
		
	result = loader.readIdFloat("ScrollRight",screenScrollRight);
	gosASSERT(result == NO_ERR);
	
	result = loader.readIdFloat("ScrollUp",screenScrollUp);
	gosASSERT(result == NO_ERR);
	
	result = loader.readIdFloat("ScrollDown",screenScrollDown);
	gosASSERT(result == NO_ERR);

	result = loader.readIdFloat("BaseFrameLength",baseFrameLength);
	gosASSERT(result == NO_ERR);
	
	result = loader.readIdFloat("RotationDegPerSec",degPerSecRot);
	gosASSERT(result == NO_ERR);
	
	float missionDragThreshold;
	result = loader.readIdFloat("MouseDragThreshold",missionDragThreshold);
	gosASSERT(result == NO_ERR);

	float missionDblClkThreshold;
	result = loader.readIdFloat("MouseDoubleClickThreshold",missionDblClkThreshold);
	gosASSERT(result == NO_ERR);


	// addBuildingsToNewMenu requires land != NULL (uses land->terrainTextures at line ~1082).
	// For the generator path, terrain is not yet loaded here — defer to update() post-generate.
	if ( land )
		addBuildingsToNewMenu();

	curBrush = new SelectionBrush( false, -1 );
	ChangeCursor( IDC_MC2ARROW );

	// Guard: syncScrollBars/initTacMap require terrain (land != NULL).
	// For the ImGui generator path, terrain is not yet loaded when
	// EditorInterface::init() runs — skip and call postTerrainInit() after
	// generateFromDialogParams() sets up the terrain.
	if ( land )
	{
		syncScrollBars();
		initTacMap();
	}

	UnsetBusyMode();

	bThisIsInitialized = true;
}

void EditorInterface::addBuildingsToNewMenu()
{
	EditorObjectMgr* pMgr = EditorObjectMgr::instance();

	// Use catalog keys for menu header strings.
	// IDS_BUILDINGS / IDS_TERRAINS are legacy; the catalog key is the
	// authoritative source.  Fall back to EditorSafeLoadString if the
	// catalog entry is missing (not expected in normal builds).
	char BuildingHeader[64];
	char TerrainHeader[64];
	if (!EditorResourceCatalog::GetStringByKey("editor.menu.objects", BuildingHeader, 64))
		EditorSafeLoadString( IDS_BUILDINGS, BuildingHeader, 64, gameResourceHandle );
	if (!EditorResourceCatalog::GetStringByKey("editor.menu.terrains", TerrainHeader, 64))
		EditorSafeLoadString( IDS_TERRAINS, TerrainHeader, 64, gameResourceHandle );

	CMenu* pMenu = new CMenu;
	pMenu->CreatePopupMenu();

	// now i need to add the buildings to the toolbars/menus
	int groupCount = pMgr->getBuildingGroupCount( );

	const char** pNames = ( const char** )malloc( sizeof( const char *) * groupCount );
	pMgr->getBuildingGroupNames( pNames, groupCount );

	menus = (CMenu**) malloc( sizeof( CMenu* ) * (groupCount + 2) );
	// EDITOR-EXIT-CRASH-1 (#2): record exactly how many entries were allocated
	// (indices 0..groupCount+1 are written below: pMenu, groupCount child
	// menus, and the trailing terrain popup menu). terminate() frees this
	// many, not a re-queried getBuildingGroupCount()+1 which can both under-
	// free (leak) if the group count changed, or walk NULL/garbage entirely
	// if menus was never allocated.
	menusAllocatedCount = groupCount + 2;
	menus[0] = pMenu;

	int id = IDS_OBJECT_200;
	int i = 0;
	for ( i = 0; i < groupCount; ++i )
	{
		CMenu* pChildMenu = new CMenu;
		pChildMenu->CreatePopupMenu();
		menus[i + 1] = pChildMenu;
		
		// now add sub items to the group
		int buildingCount = pMgr->getNumberBuildingsInGroup( i );

		const char** pBuildingNames = (const char**)malloc( sizeof( const char * ) * buildingCount );
 
		pMgr->getBuildingNamesInGroup( i, pBuildingNames, buildingCount );

		for ( int j = 0; j < buildingCount; ++j, ++id )
		{
			CString oldName;
			bool bAdded = 0;
			int count = pChildMenu->GetMenuItemCount();
			for (int k = 0; k < count; ++k )
			{
				pChildMenu->GetMenuString( k, oldName, MF_BYPOSITION );
				if ( oldName > pBuildingNames[j] )
				{
					pChildMenu->InsertMenu( k, MF_BYPOSITION, id, pBuildingNames[j] );
					bAdded = 1;
					break;

				}
			}
			if ( !bAdded )
				pChildMenu->AppendMenu(MF_STRING, id, pBuildingNames[j] );
		}

		
		CString oldName;
		bool bAdded = 0;
		int count =  pMenu->GetMenuItemCount();
		for (int k = 0; k < count; ++k )
		{
			pMenu->GetMenuString( k, oldName, MF_BYPOSITION );
			if ( oldName > pNames[i] )
			{
				pMenu->InsertMenu( k, MF_BYPOSITION | MF_POPUP, (UINT_PTR)pChildMenu->m_hMenu, pNames[i] );
				bAdded = 1;
				break;

			}
		}
		if ( !bAdded )
			pMenu->AppendMenu(MF_POPUP, (UINT_PTR)pChildMenu->m_hMenu, pNames[i]);

		free( pBuildingNames );
	}
	AfxGetMainWnd()->GetMenu()->InsertMenu( 5, MF_BYPOSITION | MF_POPUP, (UINT_PTR)pMenu->m_hMenu, BuildingHeader );

	CMenu* pChildMenu = new CMenu;
	pChildMenu->CreatePopupMenu();
	menus[i + 1] = pChildMenu;

	int numTerrains = 0;
	numTerrains = land->terrainTextures->getNumTypes();

	for ( i = groupCount; i < numTerrains + groupCount; i++ )
	{
		
		char buffer[256];

		int nameID = land->terrainTextures->getTextureNameID(i - groupCount);

		// Cement types are excluded from the Terrain paint menu.
		// These numeric IDs map to cement texture entries.
		bool continueFlag = true;
		switch (nameID)
		{
		case 10020/*Cement 1*/:
		case 10021/*Cement 1 (angled)*/:
		case 10022/*Cement 2*/:
		case 10023/*Cement 2 (angled)*/:
		case 10024/*Cement 3*/:
		case 10025/*Cement 3 (angled)*/:
		case 10026/*Cement 1 (crumbled)*/:
		case 10027/*Cement 2 (crumbled)*/:
		case 10069/*Cement 3 (crumbled)*/:
			continueFlag = false;
			break;
		};
		if (continueFlag)
			continue;

		// Prefer catalog key lookup (stable, mc2res-independent).
		// Fall back to legacy EditorSafeLoadString if catalog has no entry.
		long terrainIndex = i - groupCount;
		if (!EditorResourceCatalog::GetTerrainNameByLegacyStringId(nameID, buffer, 256))
		{
			if (!EditorResourceCatalog::GetTerrainNameByIndex(terrainIndex, buffer, 256))
				EditorSafeLoadString( nameID, buffer, 256 );
		}

		int count = pChildMenu->GetMenuItemCount();
		bool bPlaced = 0;
		CString newStr( buffer );
		for ( int j = 0; j < count; ++j )
		{
			CString tmp;
			pChildMenu->GetMenuString( j, tmp, MF_BYPOSITION );
			if ( tmp > newStr )
			{
				pChildMenu->InsertMenu( j, MF_BYPOSITION, ID_TERRAINS_BLUEWATER + i - groupCount, buffer );
				bPlaced = 1;
				break;

			}
		}

		if ( !bPlaced )
			pChildMenu->AppendMenu( MF_STRING, ID_TERRAINS_BLUEWATER + i - groupCount, buffer );
	}
	AfxGetMainWnd()->GetMenu()->InsertMenu( 4, MF_BYPOSITION | MF_POPUP, (UINT_PTR)pChildMenu->m_hMenu, TerrainHeader );


	AfxGetMainWnd()->DrawMenuBar();
	
	free( pNames );

}

//--------------------------------------------------------------------------------------
void EditorInterface::handleNewMenuMessage( long specificMessage )
{
	if ( !eye || !eye->active )
		return;

	if (EditorInterface::instance()->ObjectSelectOnlyMode())
	{
		PlaySound("SystemDefault",NULL,SND_ASYNC);
		return;
	}

	// special check for building range
	if ( specificMessage >= IDS_OBJECT_200 && specificMessage <= 30800 )
	{
		paintBuildings( specificMessage );
	}

	if ( specificMessage >= ID_TERRAINS_BLUEWATER - 1 && specificMessage <= ID_TERRAINS_BLUEWATER + 255 )
	{
		PaintTerrain( specificMessage - ID_TERRAINS_BLUEWATER );
		return;
	}
	else if ( specificMessage >= ID_ALIGNMENT_TEAM1 && specificMessage <= ID_ALIGNMENT_TEAM1 + 9 )
	{
		Alignment( specificMessage ); 
	}
	else 
	{
		switch (specificMessage) {
			case ID_FILE_ASSIGNHEIGHTMAP:
			case ID_FOG:
			case ID_OTHER_RELOADBASETEXTURE:
			case ID_OTHER_SETBASETEXTURENAME:
			case ID_LIGHT:
			case ID_WAVES:
			case ID_OTHER_ASSIGNHEIGHT:
			case ID_MISSION_SETTINGS:
			case ID_MISSION_PLAYER_PLAYER1:
			case ID_MISSION_PLAYER_PLAYER2:
			case ID_MISSION_PLAYER_PLAYER3:
			case ID_MISSION_PLAYER_PLAYER4:
			case ID_MISSION_PLAYER_PLAYER5:
			case ID_MISSION_PLAYER_PLAYER6:
			case ID_MISSION_PLAYER_PLAYER7:
			case ID_MISSION_PLAYER_PLAYER8:
			case ID_OTHER_SELECTDETAILTEXTURE:
			case ID_OTHER_SELECTWATERTEXTURE:
			case ID_OTHER_SELECTWATERDETAILTEXTURE:
			case ID_OTHER_TEXTURETILINGFACTORS:
			case ID_SKY_SKY1:
			case ID_SKY_SKY2:
			case ID_SKY_SKY3:
			case ID_SKY_SKY4:
			case ID_SKY_SKY5:
			case ID_SKY_SKY6:
			case ID_SKY_SKY7:
			case ID_SKY_SKY8:
			case ID_SKY_SKY9:
			case ID_SKY_SKY10:
			case ID_SKY_SKY11:
			case ID_SKY_SKY12:
			case ID_SKY_SKY13:
			case ID_SKY_SKY14:
			case ID_SKY_SKY15:
			case ID_SKY_SKY16:
			case ID_SKY_SKY17:
			case ID_SKY_SKY18:
			case ID_SKY_SKY19:
			case ID_SKY_SKY20:
			case ID_SKY_SKY21:
			case ID_OTHER_REFRACTALIZETERRAIN:

			/*not sure these are still used*/
			case ID_PURGE_TRANSITIONS:
			case ID_SHOW_TRANSITIONS:
			case ID_DROPZONES_ADD:
			case ID_DROPZONES_VTOL:
			case IDS_SAVE_CAMERAS:
				{
					if (EditorData::instance)
					{
						EditorData::instance->MissionNeedsSaving(true);
					}
				}
				break;
		}
		switch (specificMessage) {
			case ID_FILE_NEW2: { New(); } break;
			case ID_FILE_OPEN2: { FileOpen(); } break;
			case ID_FILE_SAVEAS: { SaveAs(); } break;
			case ID_FILE_SAVE: {
				// Gate on blocking failures (e.g. no terrain loaded).
				// Warnings (MOVE data, objectives) are surfaced but do NOT
				// block the save -- the user can proceed and review the
				// checklist separately.
				if (MissionValidator::HasBlockingFailures()) {
					MissionValidator::Open();
				} else {
					Save();
				}
				break;
			}
			case ID_FILE_QUICKSAVE: { QuickSave(); } break;
			case ID_FILE_ASSIGNHEIGHTMAP: { NewHeightMap(); } break;
			case ID_FILE_EXIT: { Quit(); } break;
			case ID_EDIT_UNDO2: { Undo(); } break;
			case ID_EDIT_REDO2: { Redo(); } break;
			case ID_EDIT_COPY2: { ; } break;
			case ID_OVERLAYS_DIRTROAD: { PaintDirtRoad(); } break;
			case ID_OVERLAYS_PAEVEDROAD: { PaintPaved(); } break;
			case ID_OVERLAYS_ROUGH: { PaintRocks(); } break;
			case ID_OVERLAYS_TWOLANEDIRTROAD: { PaintTwoLaneDirtRoad(); } break;
			case ID_OVERLAYS_DAMAGEDROAD: { PaintDamagedRoad(); } break;
			case ID_OVERLAYS_RUNWAY: { PaintRunway(); } break;
			case ID_OVERLAYS_BRIDGE: { PaintBridge(); } break;
			case ID_OVERLAYS_DAMAGEDBRIDGE: { PaintDamagedBridge(); } break;
			case ID_OTHER_ERASE: { Erase(); } break;
			case ID_OTHER_SELECT: { Select(); } break;
			case ID_OTHER_FLATTEN: { Flatten(); } break;
			case IDS_SELECT_SLOPES: SelectSlopes(); break;
			case IDS_SELECT_ALTITUDE: SelectAltitude(); break;
			case ID_SELECT_TERRAIN_TYPE: SelectTerrainType(); break;
			case ID_FOG:	Fog(); break;
			case ID_OTHER_RELOADBASETEXTURE: ReloadBaseTexture(); break;
			case ID_OTHER_SETBASETEXTURENAME: SetBaseTexture(); break;
			case ID_PURGE_TRANSITIONS: PurgeTransitions(); break;
			case ID_SHOW_TRANSITIONS: ShowTransitions(); break;
			case ID_LIGHT:  Light(); break;
			case IDS_SAVE_CAMERAS: SaveCameras(); break;
			case ID_WAVES: Waves(); break;
			case ID_DRAGSMOOTH : DragSmooth(); break;
			case ID_DRAGNORMAL : DragRough(); break;
			case ID_OTHER_ASSIGNHEIGHT : AssignElevation(); break;
			case ID_SMOOTH_RADIUS: SmoothRadius(); break;
			case ID_SAVE_HEIGHT_MAP : 	SaveHeightMap(); break;
			case ID_MISSION_SETTINGS: MissionSettings(); break;
			case ID_MISSION_TEAM_TEAM1: Team(0); break;
			case ID_MISSION_TEAM_TEAM2: Team(1); break;
			case ID_MISSION_TEAM_TEAM3: Team(2); break;
			case ID_MISSION_TEAM_TEAM4: Team(3); break;
			case ID_MISSION_TEAM_TEAM5: Team(4); break;
			case ID_MISSION_TEAM_TEAM6: Team(5); break;
			case ID_MISSION_TEAM_TEAM7: Team(6); break;
			case ID_MISSION_TEAM_TEAM8: Team(7); break;
			case ID_MISSION_PLAYER_PLAYER1: Player(0); break;
			case ID_MISSION_PLAYER_PLAYER2: Player(1); break;
			case ID_MISSION_PLAYER_PLAYER3: Player(2); break;
			case ID_MISSION_PLAYER_PLAYER4: Player(3); break;
			case ID_MISSION_PLAYER_PLAYER5: Player(4); break;
			case ID_MISSION_PLAYER_PLAYER6: Player(5); break;
			case ID_MISSION_PLAYER_PLAYER7: Player(6); break;
			case ID_MISSION_PLAYER_PLAYER8: Player(7); break;
			case ID_OTHER_DAMAGE: Damage( 1); break;
			case ID_OTHER_REPAIR: Damage( 0 ); break;
			case ID_OTHER_LINK: Link( true ); break;
			case ID_OTHER_UNLINK: Link( false ); break;
			case ID_OTHER_LAYMINES: LayMines(); break;
			case ID_OTHER_SELECTDETAILTEXTURE: SelectDetailTexture(); break;
			case ID_OTHER_SELECTWATERTEXTURE: SelectWaterTexture(); break;
			case ID_OTHER_SELECTWATERDETAILTEXTURE: SelectWaterDetailTexture(); break;
			case ID_OTHER_TEXTURETILINGFACTORS: TextureTilingFactors(); break;
			case ID_DROPZONES_ADD: DropZone( false ); break;
			case ID_DROPZONES_VTOL : DropZone( true ); break;
			case ID_UNITSETTINGS : UnitSettings(); break;
			case ID_SKY_SKY1  : SetSky(1); break;
			case ID_SKY_SKY2  : SetSky(2); break;
			case ID_SKY_SKY3  : SetSky(3); break;
			case ID_SKY_SKY4  : SetSky(4); break;
			case ID_SKY_SKY5  : SetSky(5); break;
			case ID_SKY_SKY6  : SetSky(6); break;
			case ID_SKY_SKY7  : SetSky(7); break;
			case ID_SKY_SKY8  : SetSky(8); break;
			case ID_SKY_SKY9  : SetSky(9); break;
			case ID_SKY_SKY10 : SetSky(10); break;
			case ID_SKY_SKY11 : SetSky(11); break;
			case ID_SKY_SKY12 : SetSky(12); break;
			case ID_SKY_SKY13 : SetSky(13); break;
			case ID_SKY_SKY14 : SetSky(14); break;
			case ID_SKY_SKY15 : SetSky(15); break;
			case ID_SKY_SKY16 : SetSky(16); break;
			case ID_SKY_SKY17 : SetSky(17); break;
			case ID_SKY_SKY18 : SetSky(18); break;
			case ID_SKY_SKY19 : SetSky(19); break;
			case ID_SKY_SKY20 : SetSky(20); break;
			case ID_SKY_SKY21 : SetSky(21); break;
			case ID_OTHER_REFRACTALIZETERRAIN : RefractalizeTerrain(1);	break;
			case ID_CAMPAIGN_EDITOR : CampaignEditor(); break;
		}
	}
}


//--------------------------------------------------------------------------------------
int EditorInterface::Quit()
{
	int res = PromptAndSaveIfNecessary();
	if (IDCANCEL != res) {
		SetBusyMode();
		gos_TerminateApplication();
		PostQuitMessage(0);
		UnsetBusyMode();
	}
	return 1;
}

//--------------------------------------------------------------------------------------
int EditorInterface::PromptAndSaveIfNecessary()
{
	int res = IDNO;
	bool endFlag = false;
	while (!endFlag) {
		endFlag = true;
		if (EditorInterface::instance() && EditorInterface::instance()->ThisIsInitialized()
			&& EditorData::instance) {
			if (EditorInterface::instance()->undoMgr.ThereHasBeenANetChangeFromWhenLastSaved()) {
				res = AfxMessageBox(IDS_DO_YOU_WANT_TO_SAVE_YOUR_CHANGES, MB_YESNOCANCEL);
			} else if (EditorData::instance->MissionNeedsSaving()) {
				res = AfxMessageBox(IDS_DO_YOU_WANT_TO_SAVE_THIS_MISSION, MB_YESNOCANCEL);
			}
		}
		if (IDYES == res) {
			SetBusyMode();
			int saveRes = EditorInterface::instance()->Save();
			UnsetBusyMode();
			if (IDCANCEL == saveRes) {
				endFlag = false;
			}
		}
	}
	return res;
}

//--------------------------------------------------------------------------------------
int EditorInterface::New()
{
	int res = PromptAndSaveIfNecessary();
	if (IDCANCEL == res) {
		return false;
	}
	bool bOK = false;
	while ( !bOK )
	{
		TerrainDlg dlg;
		dlg.terrain = 0;
		if ( IDOK == dlg.DoModal() )
		{
			MapSizeDlg msdlg;
			msdlg.mapSize = 0;
			if ( IDOK == msdlg.DoModal() )
			{
				SetBusyMode();
				eye->reset();
				bOK = EditorData::initTerrainFromTGA( msdlg.mapSize, 0, 0, dlg.terrain );
				UnsetBusyMode();
				if ( !bOK )
				{
					// Terrain init failed (missing texture, etc.).
					// Let the user retry with different settings
					// instead of looping endlessly.
					continue;
				}
			}
			else
			{
				return true;
			}
		}
		else		//They pressed cancel.  Let 'em cancel, dammit!
		{
			return true;
		}
	}

	tacMap.UpdateMap();
	syncScrollBars();
	return true;
}

//--------------------------------------------------------------------------------------
// Generate a pseudo-random, editable mission: pick terrain type + size, then run
// the terrain generator (Path B) and build the map from its heightmap + colormap.
int EditorInterface::NewGeneratedMission()
{
	int res = PromptAndSaveIfNecessary();
	if (IDCANCEL == res)
		return false;

	TerrainDlg dlg;
	dlg.terrain = 0;
	if ( IDOK != dlg.DoModal() )
		return true;

	MapSizeDlg msdlg;
	msdlg.mapSize = 0;
	if ( IDOK != msdlg.DoModal() )
		return true;

	SetBusyMode();
	eye->reset();
	bool bOK = EditorData::generateMission( msdlg.mapSize, dlg.terrain, (unsigned long)GetTickCount() );
	UnsetBusyMode();

	if ( bOK )
	{
		tacMap.UpdateMap();
		syncScrollBars();
		PlaySound("SystemDefault",NULL,SND_ASYNC);
	}
	return true;
}

//--------------------------------------------------------------------------------------
int EditorInterface::FileOpen()
{
	int res = PromptAndSaveIfNecessary();
	if (IDCANCEL == res) {
		return false;
	}

	CFileDialog fileDlg( 1,  "pak", NULL, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR, szPAKFilter );
	// When a Mod Project is open, default to <root>\data\missions; else unchanged.
	{ const char* pd = EditorModProject::SaveDirOverride();
	  fileDlg.m_ofn.lpstrInitialDir = pd ? pd : missionPath; }

	if (  IDOK == fileDlg.DoModal() )
	{
		SetBusyMode();
		const char* pFile = fileDlg.m_ofn.lpstrFile;		
		EditorData::initTerrainFromPCV( pFile );
		tacMap.UpdateMap();
		syncScrollBars();
		PlaySound("SystemDefault",NULL,SND_ASYNC);
		UnsetBusyMode();
	}

	return true;

}

//--------------------------------------------------------------------------------------

void EditorInterface::OpenMissionByPath( const char* pakPath )
{
	if ( !pakPath || !pakPath[0] )
		return;
	SetBusyMode();
	EditorData::initTerrainFromPCV( pakPath );
	tacMap.UpdateMap();
	syncScrollBars();
	PlaySound("SystemDefault",NULL,SND_ASYNC);
	UnsetBusyMode();
}

//--------------------------------------------------------------------------------------

void EditorInterface::handleLeftButtonDown( int PosX, int PosY )
{
	if ( !eye || !eye->active  )
		return;

	// RTT: viewport-local coords for the press pick + drag origin, so they live in
	// the same space as handleMouseMove (which also offsets). No-op when RTT off.
	EditorRttClientToViewport( PosX, PosY );

	Stuff::Vector3D vector;
	Stuff::Vector2DOf<long> v2( PosX, PosY );
	eye->inverseProject( v2, vector );

	// Slice 1 -- Marker XY map-picker (PICKER-SELECT-POINT-RECON-1, Option B).
	// The Objective dialog's "Pick from map" button armed pendingPickPoint and
	// unwound the modal stack. This one click delivers the world XY (vector, set
	// by inverseProject above). Capture it, disarm the pick, flag the result, and
	// re-open the objectives dialog chain (still in ObjectSelectOnlyMode, exactly
	// like the unit/building object-pick re-open in SelectionBrush::endPaint). The
	// re-opened ObjectiveDlg::OnInitDialog consumes pendingPickResultReady, writes
	// the marker fields, and clears ObjectSelectOnlyMode. Checked BEFORE the
	// waypoint/object-pick handling so the point pick takes priority.
	if ( ObjectSelectOnlyMode() && objectivesEditState.pendingPickPoint )
	{
		// inverseProject ALWAYS yields a usable on-map world coord: it returns 0 on a
		// real terrain hit (camera.cpp:1488) and 1 on an off-map miss that still
		// clamps to the nearest on-map vertex (camera.cpp:1482) -- BOTH are valid
		// points to accept. The only degenerate is the camera-not-ready case
		// (turn < 4), which zeroes the point (camera.cpp:997) and is unreachable
		// during an interactive pick. So guard on the zeroed POINT, not the return
		// code (return 0 is the COMMON hit case -- gating on it would reject every
		// valid click). Skip only an exact (0,0) so a later real click can deliver.
		if ( vector.x != 0.0f || vector.y != 0.0f )
		{
			objectivesEditState.pendingPickX = vector.x;
			objectivesEditState.pendingPickY = vector.y;
			objectivesEditState.pendingPickPoint = false;
			objectivesEditState.pendingPickResultReady = true;
			ReleaseCapture();
			// BUG A fix: do NOT call Team() here. This runs inside WM_LBUTTONDOWN
			// dispatch while MFC still holds SetCapture(); re-entering a nested
			// DoModal from mouse-down-with-capture loses the WM_LBUTTONUP and the
			// click never reaches a clean state (user had to ESC 2-3 times). Arm a
			// flag instead and let EditorInterface::update() -- a per-frame point
			// with no modal/capture on the stack -- reopen the dialog chain via
			// Team(), exactly like the Locate path. ObjectSelectOnlyMode stays true
			// through the reopen; the re-opened dialog's consumer clears
			// pendingPickResultReady. Covers both marker and area-center picks.
			objectivesEditState.pendingPickReopen = true;
		}
		return;
	}

	// Waypoint placement mode (UnitBrainPanel "Add Waypoints"): drop a patrol/move
	// waypoint at the SAME click world position the editor uses to place buildings
	// (eye->inverseProject -> vector). The bridge pick returns 0,0 here, so use the
	// building-placement coordinate; snap Z to terrain.
	if ( UnitBrainPanel::WaypointPlaceActive() )
	{
		Stuff::Vector3D wp = vector;
		if ( land )
			wp.z = land->getTerrainElevation( wp );
		if ( UnitBrainPanel::HandlePlacementClick( wp.x, wp.y, wp.z ) )
			return;
	}

	// Object drag-move: in the select/pointer tool, a press that lands on an
	// object grabs it for dragging (and selects it) instead of starting a
	// terrain/area paint. The drag itself runs in handleMouseMove().
	if ( currentBrushID == IDS_SELECT )
	{
		EditorObject* pHit = EditorObjectMgr::instance()->getObjectAtScreenPosition( PosX, PosY );
		if ( pHit )
		{
			// Grab for a potential drag-move only. Do NOT change selection here:
			// selection is owned by the click-release path (normal/Ctrl click via
			// EditorInterface_SelectObjectAtScreenPoint). Mutating it on press
			// fought that path -- a release pick-miss could deselect the grab.
			m_pDragObject = pHit;
			m_dragObjMoved = false;
			m_dragStartX = PosX;
			m_dragStartY = PosY;
			m_dragStartWorld = vector;
			m_dragObjStartPos = pHit->appearance() ? pHit->appearance()->position : vector;
			m_dragLastScreenX = PosX;
			m_dragLastScreenY = PosY;
			// One-shot drag coordinate-space diagnostic (MC2_EDITOR_DRAG_TRACE=1).
			// At grab the cursor is ON the object, so the object's FORWARD-projected
			// screen pos (projectModernClipGL + gos_GetViewport remap, the correct
			// world->screen direction) should equal (PosX,PosY) if the mouse coords
			// and the projection share a pixel space. A ~2x / ~0.5x ratio pins a
			// client-vs-FBO/screenResolution scale mismatch as the "drag moves ~2x the
			// cursor" root; a 1:1 match instead implicates the worldToClipGL INVERSE
			// (X-collapse) used by screenToGroundPlaneApprox. PosX/PosY here are
			// already RTT-remapped (handleMouseMove); this path is the press handler.
			{
				static const bool s_dragTrace = ( getenv("MC2_EDITOR_DRAG_TRACE") != NULL );
				if ( s_dragTrace && eye && pHit->appearance() )
				{
					Stuff::Vector3D wp = pHit->appearance()->position;
					ModernClipResult r = eye->projectModernClipGL( wp );
					float vmx = 0.f, vmy = 0.f, vax = 0.f, vay = 0.f;
					gos_GetViewport( &vmx, &vmy, &vax, &vay );
					float objSx = -1.f, objSy = -1.f;
					if ( r.clip.w > 1e-4f )
					{
						const float ndcX = r.clip.x / r.clip.w;
						const float ndcY = r.clip.y / r.clip.w;
						objSx = vax + (ndcX * 0.5f + 0.5f) * vmx;
						objSy = vay + (1.0f - (ndcY * 0.5f + 0.5f)) * vmy;
					}
					CRect cr; GetClientRect( &cr );
					fprintf( stderr,
						"[DragTrace] cursorRemapped=(%d,%d) objScreenGL=(%.1f,%.1f) clipW=%.3f "
						"vpMul=(%.1f,%.1f) vpAdd=(%.1f,%.1f) client=(%dx%d)\n",
						PosX, PosY, objSx, objSy, r.clip.w,
						vmx, vmy, vax, vay, (int)cr.Width(), (int)cr.Height() );
					fflush( stderr );
				}
			}
			m_pDragAction = new ModifyBuildingAction;
			// Group drag: if the grabbed object is part of a multi-selection, record
			// EVERY selected object's start state so the whole group is one undoable
			// move (and handleMouseMove translates them all). Otherwise just the grab.
			EditorObjectMgr* pDragMgr = EditorObjectMgr::instance();
			if ( pHit->isSelected() && pDragMgr->getSelectionCount() > 1 )
			{
				EditorObjectMgr::EDITOR_OBJECT_LIST sel = pDragMgr->getSelectedObjectList();
				for ( EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = sel.Begin();
					!it.IsDone(); it++ )
					if ( *it && (*it)->appearance() )
						m_pDragAction->addBuildingInfo( **it );
			}
			else
			{
				m_pDragAction->addBuildingInfo( *pHit );
			}
			lastClickPos = vector;
			return;
		}
	}

	if ( curBrush )
	{
		painting = true;
		curBrush->beginPaint( );

		if ( curBrush->canPaint( vector, PosX, PosY, 0 ) )
			curBrush->paint( vector, PosX, PosY );
	}

	lastClickPos = vector;

}

void EditorInterface::handleMouseMove( int PosX, int PosY )
{
	if (  !eye || !eye->active  )
		return;

	// RTT: shift full-window client coords into the central-node viewport space so
	// every screen->world unproject below (ground-plane, drag jacobian) matches the
	// rect the scene is actually drawn in. No-op when RTT off. Camera-rotate/drag
	// deltas are offset-invariant, so this is safe for all consumers here.
	EditorRttClientToViewport( PosX, PosY );

	Stuff::Vector3D vector;
	Stuff::Vector2DOf<long> v2( PosX, PosY );
	// Per-move cursor world position. Use the cheap O(1) ground-plane unproject,
	// NOT the legacy terrain picker (eye->inverseProject scans ~40k quads — it was
	// 125-296ms PER mouse-move here, the residual editor pan/paint hitch). Every
	// consumer below is cell-based (worldToCell uses x,y); elevation is not read.
	// Precise terrain picks remain where they matter: click (handleLeftButtonDown)
	// and the object drag-move jacobian (4 inverseProject calls just below).
	eye->screenToGroundPlaneApprox( v2.x, v2.y, vector );
	// One-step elevation refinement for terrain sculpting: screenToGroundPlaneApprox
	// intersects z=0; on elevated terrain the x,y diverges from the actual brush cell.
	// Re-intersect at the terrain z so HeightBrush paints where the cursor visually lands.
	if ( painting && land )
	{
		const float ze = land->getTerrainElevation( vector );
		if ( fabsf( ze ) > 0.5f )
			eye->screenToGroundPlaneApprox( v2.x, v2.y, vector, ze );
	}

	// Object drag-move: while an object is grabbed, follow the cursor by moving
	// it to the cell under the mouse. moveBuilding() snaps to the cell grid and
	// keeps any building links in sync; it returns false if the target cell is
	// occupied, in which case the object simply stays put.
	if ( m_pDragObject )
	{
		// Require a deliberate drag before moving: until the cursor leaves a small
		// tolerance around the press point, treat the gesture as a click (select),
		// not a drag. Without this, a click that merely grazed an object nudged it
		// by a cell ("mechs move when I click near them").
		int ddx = PosX - m_dragStartX;
		int ddy = PosY - m_dragStartY;
		if ( !m_dragObjMoved && (ddx * ddx + ddy * ddy) < (8 * 8) )
			return;

		// Free (un-snapped) positioning via a world-space delta from the grab point.
		// Using a delta cancels any constant offset between the inverseProject world
		// space and the object's stored-position space, so the building tracks the
		// cursor exactly and can be aligned precisely. moveBuilding keeps cell/link
		// bookkeeping current; we then override its grid-snapped position.
		// Continuous free placement that tracks the cursor under the tilted camera.
		// Sample the screen->world map LOCALLY around the current cursor each frame
		// (so it matches the perspective where the object is), then advance the
		// object by the per-frame pixel delta * that local map. A single grab-time
		// map overshoots because world-units-per-pixel varies across the screen.
		ObjectAppearance* pDragApp = m_pDragObject->appearance();
		if ( !pDragApp )
			return;
		// FORWARD-projection drag jacobian (metafix rule: forward-project via
		// worldToClipGL, NEVER unproject cursor->world). Every screen->world
		// unproject available here -- inverseProject, screenToGroundPlaneApprox,
		// screenToTerrainApprox -- inverts worldToClipGL, and that inverse is
		// distorted (the documented "X-collapse", see Camera::inverseProject): the
		// dragged prop tracked ~2x the cursor and zig-zagged on a straight drag.
		// MC2_EDITOR_DRAG_TRACE measurement confirmed the FORWARD projection
		// (projectModernClipGL + gos_GetViewport remap) lands the object exactly
		// under the cursor with NO coordinate-space scale (client == FBO == viewport,
		// vpAdd=0), so no mouse-coord normalization is needed. Build dScreen/dWorld by
		// forward-projecting the object's world pos +-D world-units in X and Y (the
		// well-conditioned matrix the GPU renders with), invert that 2x2, and map the
		// per-frame cursor pixel delta to a world delta. Recomputed at the object's
		// real position each move -> 1:1 tracking, no jitter, and the near-plane guard
		// replaces the inverseProject garbage-far-point teleport the prior fix chased.
		auto projGL = [&]( const Stuff::Vector3D& wp, float& outSx, float& outSy ) -> bool {
			ModernClipResult r = eye->projectModernClipGL( wp );
			if ( r.clip.w <= 1e-4f )            // at/behind the near plane (signed w)
				return false;
			float vmx = 0.f, vmy = 0.f, vax = 0.f, vay = 0.f;
			gos_GetViewport( &vmx, &vmy, &vax, &vay );
			const float ndcX = r.clip.x / r.clip.w;
			const float ndcY = r.clip.y / r.clip.w;
			outSx = vax + (ndcX * 0.5f + 0.5f) * vmx;
			outSy = vay + (1.0f - (ndcY * 0.5f + 0.5f)) * vmy;  // GL bottom-left -> screen-Y flip
			return true;
		};
		const float D = 50.0f;   // world-unit probe for the forward jacobian
		Stuff::Vector3D w0  = pDragApp->position;
		Stuff::Vector3D wXp = w0; wXp.x += D;
		Stuff::Vector3D wYp = w0; wYp.y += D;
		float s0x, s0y, sXx, sXy, sYx, sYy;
		if ( !projGL( w0, s0x, s0y ) || !projGL( wXp, sXx, sXy ) || !projGL( wYp, sYx, sYy ) )
		{
			// Object at/behind the near plane this frame: skip the move (do NOT fall
			// back to a broken unproject that would teleport it across the map).
			m_dragLastScreenX = PosX;
			m_dragLastScreenY = PosY;
			return;
		}
		// Forward jacobian J = dScreen/dWorld (rows screenX/Y, cols worldX/Y).
		const float invD = 1.0f / D;
		const float Jxx = ( sXx - s0x ) * invD;   // dScreenX / dWorldX
		const float Jyx = ( sXy - s0y ) * invD;   // dScreenY / dWorldX
		const float Jxy = ( sYx - s0x ) * invD;   // dScreenX / dWorldY
		const float Jyy = ( sYy - s0y ) * invD;   // dScreenY / dWorldY
		const float det = Jxx * Jyy - Jxy * Jyx;
		float dsx = (float)( PosX - m_dragLastScreenX );
		float dsy = (float)( PosY - m_dragLastScreenY );
		m_dragLastScreenX = PosX;
		m_dragLastScreenY = PosY;
		// World translation for this frame (J^-1 * screen delta), computed once from
		// the grabbed object's jacobian and applied as a RIGID delta to the group.
		float worldDx = 0.f, worldDy = 0.f;
		if ( fabsf( det ) > 1e-9f )
		{
			const float invDet = 1.0f / det;
			worldDx = (  Jyy * dsx - Jxy * dsy ) * invDet;
			worldDy = ( -Jyx * dsx + Jxx * dsy ) * invDet;
		}

		// Apply the same world delta to one object (free move + cell/link bookkeeping
		// + recipe re-bake). Used for both the single grab and each group member.
		auto dragMoveOne = [&]( EditorObject* pObj ) {
			if ( !pObj ) return;
			ObjectAppearance* pApp = pObj->appearance();
			if ( !pApp ) return;
			Stuff::Vector3D np = pApp->position;
			np.x += worldDx;
			np.y += worldDy;
			int row = 0, col = 0;
			land->worldToCell( np, row, col );
			EditorObjectMgr::instance()->moveBuilding( pObj, row, col );
			np.z = land->getTerrainElevation( np );
			pApp->position = np;
			// Invalidate the baked static recipe BEFORE update() so update() runs
			// unregistered and re-transforms from the free position; the next render
			// re-bakes at the free pose. Invalidate-after-update never re-bakes.
			pApp->invalidateStaticRegistration();
			pApp->update();
			// Re-bake the static recipe at the new pose (invalidate only destroys the
			// old recipe; nothing else re-registers it).
			pApp->registerStatic();
		};

		// BUG3 (group drag): if the grabbed object is part of a multi-selection, move
		// the WHOLE selection by the same world delta. Otherwise move just the grab.
		EditorObjectMgr* pMoveMgr = EditorObjectMgr::instance();
		if ( m_pDragObject->isSelected() && pMoveMgr->getSelectionCount() > 1 )
		{
			EditorObjectMgr::EDITOR_OBJECT_LIST sel = pMoveMgr->getSelectedObjectList();
			for ( EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = sel.Begin();
				!it.IsDone(); it++ )
				dragMoveOne( *it );
		}
		else
		{
			dragMoveOne( m_pDragObject );
		}
		m_dragObjMoved = true;
		return;
	}

	if ( curBrush && ( painting ) )
	{
		// Discrete object placement (BuildingBrush) must continue-paint from the
		// SAME precise projection the click used (handleLeftButtonDown ->
		// inverseProject), NOT the approx O(1) ground-plane unproject used for the
		// cursor hot-path above. Otherwise the click places at the inverseProject
		// cell and the first (often sub-pixel / synthesized WM_MOUSEMOVE) move
		// places a SECOND object at the diverging approx cell -- "places twice per
		// click in different locations". The bug was latent until the per-move
		// unproject went O(1) approx (315ddec9) and diverged from the click cell.
		// Recompute precisely only during an active LMB paint stroke and only for
		// terrain-sculpt brushes (HeightBrush, FlattenBrush) and BuildingBrush, so
		// the per-move cursor perf win is preserved and ScatterBrush (intentional
		// drag-scatter) is unaffected.
		//
		// HeightBrush / FlattenBrush mirror fix: these brushes paint the TERRAIN
		// SURFACE. handleLeftButtonDown uses inverseProject (correct terrain hit);
		// handleMouseMove used screenToGroundPlaneApprox (z-plane intersection),
		// which diverges from the actual terrain surface on elevated or sloped
		// terrain. Windows synthesizes a WM_MOUSEMOVE immediately after LButtonDown,
		// so a single click fires paint() TWICE -- once at the correct terrain hit
		// and once at the diverging approx position. With a tilted camera on elevated
		// terrain the two positions can fall on OPPOSITE sides of the camera
		// look-at point, producing the observed bilateral mirror stroke.
		// Fix: use the same precise inverseProject path for all terrain-sculpt
		// brushes during an active stroke (painting == true), matching the existing
		// BuildingBrush guard. The per-move cursor position (vector) remains approx
		// for the cursor overlay / non-painting hover display.
		Stuff::Vector3D paintPos = vector;
		if ( dynamic_cast<BuildingBrush*>( curBrush ) ||
		     dynamic_cast<HeightBrush*>( curBrush )   ||
		     dynamic_cast<FlattenBrush*>( curBrush )  ||
		     dynamic_cast<TerrainBrush*>( curBrush ) )   // Paint-Material brush: same terrain-surface mirror fix
		{
			Stuff::Vector2DOf<long> vp( PosX, PosY );
			eye->inverseProject( vp, paintPos );
		}
		if ( curBrush->canPaint( paintPos, PosX, PosY, 0 ) )
			curBrush->paint( paintPos, PosX, PosY );
	}

 	//------------------------------------------------
	// Right drag is camera rotation and tilt now.
	if ( rightDrag )
	{
		
		long mouseXDelta = PosX - lastX;
		float actualRot = rotationInc * 0.1f * abs(mouseXDelta);
		if (mouseXDelta > 0)
		{
			eye->rotateRight(actualRot);
		}
		else if (mouseXDelta < 0)
		{
			eye->rotateLeft(actualRot);
		}
		
		long mouseYDelta = PosY - lastY;
		float actualTilt = rotationInc * 0.1f * abs(mouseYDelta);
		if (mouseYDelta > 0)
		{
			eye->tiltDown(actualTilt);
		}
		else if (mouseYDelta < 0)
		{
			eye->tiltUp(actualTilt);
		}

		lastX = PosX;
		lastY = PosY;

		SafeRunGameOSLogic();
		tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
		syncScrollBars();
	}


	char buffer2[256];
	sprintf( buffer2, "%.3f, %.3f", vector.x, vector.y );

	// need to put this value in the appropriate place.
	((MainFrame*)AfxGetMainWnd())->m_wndDlgBar.GetDlgItem( IDC_COORDINATES_EDIT )->SetWindowText( buffer2 );

	char buffer3[256];
	sprintf( buffer3, "%.3f", vector.z );

	// need to put this value in the appropriate place.
	((MainFrame*)AfxGetMainWnd())->m_wndDlgBar.GetDlgItem( IDC_ALTITUDE_EDIT )->SetWindowText( buffer3 );

	vector -= lastClickPos;
	float distance = vector.GetLength();

	char buffer[256];
	sprintf( buffer, "%.3f", distance );

	// need to put this value in the appropriate place.
	((MainFrame*)AfxGetMainWnd())->m_wndDlgBar.GetDlgItem( IDC_DISTANCE_EDIT )->SetWindowText( buffer );

	//IF there is a selected object, find distance to it from camera.
	float eyeDistance = 0.0f;
	long selectionCount = EditorObjectMgr::instance()->getSelectionCount();
	if (selectionCount)
	{
		EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjectsList = EditorObjectMgr::instance()->getSelectedObjectList();
		EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = selectedObjectsList.Begin();
		const EditorObject* pInfo = (*it);
		const ObjectAppearance* pAppearance = pInfo ? pInfo->appearance() : NULL;
		if ( pAppearance )
		{
			// by Methuselas: selected-object distance is updated during mouse move,
			// including immediately after left-click selection.  Guard the legacy
			// appearance pointer so restored mouse routing cannot crash the status
			// bar when a catalog object is only partially initialized.
			Stuff::Point3D eyePosition(eye->getCameraOrigin());
			Stuff::Point3D objPosition;
			objPosition.x = -pAppearance->position.x;
			objPosition.y = pAppearance->position.z;
			objPosition.z = pAppearance->position.y;
	
			Stuff::Vector3D Distance;
			Distance.Subtract(objPosition,eyePosition);
			eyeDistance = Distance.GetApproximateLength();
		}
	}	

	// need to put this value in the appropriate place.
	sprintf( buffer, "%.3f", eyeDistance);
	((MainFrame*)AfxGetMainWnd())->m_wndDlgBar.GetDlgItem( IDC_OBJDISTANCEEDIT )->SetWindowText( buffer );
}



// EDITOR-CRASH-HARDENING-1: scrub the drag pointer when the object it points at
// is being deleted (e.g. delete-during-drag), so the next handleMouseMove cannot
// deref a freed object. Discard the in-flight drag undo action too — it captured a
// snapshot of the now-dead object and its undo() would relocate a freed pointer.
// Mirrors the m_pDragAction teardown in handleLeftButtonUp below.
void EditorInterface::notifyObjectDeleted( const EditorObject* pObj )
{
	if ( m_pDragObject == pObj )
	{
		delete m_pDragAction;
		m_pDragAction = NULL;
		m_pDragObject = NULL;
		m_dragObjMoved = false;
	}
}

void EditorInterface::handleLeftButtonUp( int PosX, int PosY )
{
	if ( !eye || !eye->active  )
		return;

	// Finalize an object drag-move: commit the undo action if the object
	// actually moved, otherwise discard it (it was a plain click-select).
	if ( m_pDragObject )
	{
		if ( m_dragObjMoved && m_pDragAction )
			undoMgr.AddAction( m_pDragAction );
		else
			delete m_pDragAction;
		m_pDragAction = NULL;
		m_pDragObject = NULL;
		m_dragObjMoved = false;
		tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
		return;
	}

	if ( curBrush && painting )
	{
		painting = false;
		Action* pAction = curBrush->endPaint();
		if ( pAction )
			undoMgr.AddAction( pAction );
	}
}

//--------------------------------------------------------------------------------------
// THE EDITOR CLASS, kind of like the mission in MCommander
void EditorInterface::handleKeyDown( int Key )
{
	if ( !eye || !eye->active  )
		return;

	if ( DebuggerActive )
		return;

	//----------------------
	// Adjust for frameRate
	float frameFactor = frameLength / baseFrameLength;
	float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;

	bool shiftDn = GetAsyncKeyState( KEY_LSHIFT ) ? true : false;
	bool ctrlDn = GetAsyncKeyState( KEY_LCONTROL ) ? true : false;
	long altDn = GetAsyncKeyState( KEY_LMENU ) ? true : false;

	if ( Key == KEY_SPACE )
	{
		if ( !dragging )
		{
			prevBrush = curBrush;
			prevSelecting = selecting;
			prevPainting = painting;
			prevDragging = dragging;
			curBrush = new DragTool;
			oldCursor = curCursorID;
			ChangeCursor( IDC_HAND );
			dragging = true;
		} 
	}
	
	BuildingBrush* rotBB = ( currentBrushID >= IDS_OBJECT_200 && currentBrushID <= IDS_OBJECT_200 + 600 )
		? dynamic_cast<BuildingBrush*>(curBrush) : NULL;   // ScatterBrush is in range but not a BuildingBrush
	if ( rotBB )
	{
		if (Key == KEY_LBRACKET)
		{
			rotBB->rotateBrush( 1 );
		}
		else if (Key == KEY_RBRACKET)
		{
			rotBB->rotateBrush( -1 );
		}
	}
	else 
	{
		if (Key == KEY_LBRACKET)
		{
			rotateSelectedObjects( 1 );
		}
		else if (Key == KEY_RBRACKET)
		{
			rotateSelectedObjects( -1 );
		}
	}

	if ( lastKey != Key ) // only want to do these if something has changed
	{
		// Frame camera on the current selection (UE-style 'F'). Plain key, no
		// modifiers (Ctrl+Alt+F is the fog toggle below). WantTextInput already
		// consumed keystrokes when an ImGui field is focused, so the Outliner
		// search box won't trigger this.
		if ( Key == KEY_F && !shiftDn && !ctrlDn && !altDn )
		{
			frameSelectedObjects();
		}

		// Ctrl+P — toggle the Command Palette (Ctrl+Alt+P is ortho camera below).
		if ( Key == KEY_P && ctrlDn && !altDn && !shiftDn )
		{
			CommandPalette::Toggle();
		}

		if ( Key == KEY_ESCAPE)
		{
			Select();
			DragRough();

			if (EditorInterface::instance()->ObjectSelectOnlyMode())
			{
				ReleaseCapture();
				EditorInterface::instance()->Team(EditorInterface::instance()->objectivesEditState.alignment);
			}
		}


		if (GetAsyncKeyState(KEY_T) && ctrlDn && altDn && !shiftDn)
		{
			drawTerrainTiles ^= TRUE;
			terrainLineChanged = turn;
		}
		
		if (GetAsyncKeyState(KEY_O) && ctrlDn && altDn && !shiftDn)
		{
			drawTerrainOverlays ^= TRUE;
			terrainLineChanged = turn;
		}
		
		if (GetAsyncKeyState(KEY_S) && ctrlDn && altDn && !shiftDn)
		{
			renderObjects ^= TRUE;
			terrainLineChanged = turn;
		}

		if (GetAsyncKeyState(KEY_G) && ctrlDn && altDn && !shiftDn)
		{
			//drawTerrainGrid ^= TRUE;
			OnViewShowpassabilitymap();
			terrainLineChanged = turn;
		}

		if (GetAsyncKeyState (KEY_L) && ctrlDn && altDn && !shiftDn)
		{
			drawLOSGrid ^= TRUE;
			terrainLineChanged = turn;
		}

		if (GetAsyncKeyState (KEY_Q) && ctrlDn && altDn && !shiftDn)
		{
			land->reCalcLight(true);
		}

		if (GetAsyncKeyState(KEY_C) && ctrlDn && altDn && !shiftDn)
		{
			useClouds ^= true;
			terrainLineChanged = turn;
		}

		if (GetAsyncKeyState(KEY_F) && ctrlDn && altDn && !shiftDn)
		{
			useFog ^= true;
			terrainLineChanged = turn;
		}

		if (GetAsyncKeyState(KEY_P) && ctrlDn && altDn && !shiftDn)
		{
			//eye->usePerspective ^= true;
			OnViewOrthographiccamera();
			terrainLineChanged = turn;
		}

		if (GetAsyncKeyState(KEY_R) && ctrlDn && altDn && !shiftDn)
		{
			reloadBounds = true;
			terrainLineChanged = turn;
		}

		if (GetAsyncKeyState(KEY_V) && ctrlDn && altDn && !shiftDn)
		{
			useWaterInterestTexture ^= true;
			terrainLineChanged = turn;
		}

		if (GetAsyncKeyState(KEY_W) && ctrlDn && altDn && !shiftDn)
		{
			Terrain::mapData->recalcWater();
			terrainLineChanged = turn;
		}
		
		if (GetAsyncKeyState(KEY_D) && ctrlDn && altDn && !shiftDn)
		{
			useShadows ^= true;
			terrainLineChanged = turn;
		}
	}

	lastKey = Key;
	// Camera scroll/zoom/rotate is handled per-frame in updateCameraInput() (called from
	// render()) so it runs at render rate (~100fps) rather than key-repeat rate (~30Hz).
	
	//------------------------------------------------
	//IF there is a selected object, find distance to it from camera.
	char buffer[256];
	float eyeDistance = 0.0f;
	long selectionCount = EditorObjectMgr::instance()->getSelectionCount();
	if (selectionCount)
	{
		EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjectsList = EditorObjectMgr::instance()->getSelectedObjectList();
		EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = selectedObjectsList.Begin();
		const EditorObject* pInfo = (*it);
		// EDITOR-CRASH-HARDENING-1: appearance() can be NULL; guard before ->position
		// (mirror sibling readout ~line 1975).
		const ObjectAppearance* pAppearance = pInfo ? pInfo->appearance() : NULL;
		if ( pAppearance )
		{
			Stuff::Point3D eyePosition(eye->getCameraOrigin());
			Stuff::Point3D objPosition;
			objPosition.x = -pAppearance->position.x;
			objPosition.y = pAppearance->position.z;
			objPosition.z = pAppearance->position.y;

			Stuff::Vector3D Distance;
			Distance.Subtract(objPosition,eyePosition);
			eyeDistance = Distance.GetApproximateLength();
		}
	}

	// need to put this value in the appropriate place.
	sprintf( buffer, "%.3f", eyeDistance);
	((MainFrame*)AfxGetMainWnd())->m_wndDlgBar.GetDlgItem( IDC_OBJDISTANCEEDIT )->SetWindowText( buffer );
}

// Called every render frame from render().  Handles all continuous camera input:
// arrow-key scroll, numpad +/- zoom, Shift+arrow tilt/rotate, Ctrl+arrow light,
// and mouse-edge scrolling.  Running here at render rate (~100 fps) instead of
// WM_KEYDOWN key-repeat rate (~30 Hz) gives smooth camera motion on large maps.
void EditorInterface::updateCameraInput()
{
	if ( !eye || !eye->active )
		return;

	float frameFactor = frameLength / baseFrameLength;
	float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;

	bool shiftDn = GetAsyncKeyState( KEY_LSHIFT ) ? true : false;
	bool ctrlDn  = GetAsyncKeyState( KEY_LCONTROL ) ? true : false;
	long altDn   = GetAsyncKeyState( KEY_LMENU ) ? true : false;

	//------------------------
	// Keyboard + mouse-edge scroll flags
	bool scrollUp = (GetAsyncKeyState(KEY_UP)    && !shiftDn && !ctrlDn && !altDn);
	bool scrollDn = (GetAsyncKeyState(KEY_DOWN)  && !shiftDn && !ctrlDn && !altDn);
	bool scrollLf = (GetAsyncKeyState(KEY_LEFT)  && !shiftDn && !ctrlDn && !altDn);
	bool scrollRt = (GetAsyncKeyState(KEY_RIGHT) && !shiftDn && !ctrlDn && !altDn);

	// Mouse-edge scrolling: cursor within 24px of any viewport edge pans the camera.
	// Suppressed when ImGui owns the mouse and when the editor is not the foreground window.
	if ( ::GetForegroundWindow() == GetSafeHwnd() || ::IsChild( GetSafeHwnd(), ::GetForegroundWindow() )
	     || ::GetForegroundWindow() == ::GetParent( GetSafeHwnd() ) )
	{
		bool imguiMouse = false;
#ifdef MC2_IMGUI
		imguiMouse = ( ImGui::GetCurrentContext() != NULL ) && ImGui::GetIO().WantCaptureMouse;
#endif
		if ( !imguiMouse )
		{
			POINT cp;
			::GetCursorPos( &cp );
			ScreenToClient( &cp );
			RECT rc;
			GetClientRect( &rc );
			const int margin = 24;
			if ( cp.x >= rc.left && cp.x < rc.right && cp.y >= rc.top && cp.y < rc.bottom )
			{
				if ( cp.x <  rc.left  + margin ) scrollLf = true;
				if ( cp.x >= rc.right - margin ) scrollRt = true;
				if ( cp.y <  rc.top   + margin ) scrollUp = true;
				if ( cp.y >= rc.bottom- margin ) scrollDn = true;
			}
		}
	}

	bool zoomOut    = (GetAsyncKeyState(KEY_SUBTRACT) && !shiftDn && !ctrlDn && !altDn);
	bool zoomIn     = (GetAsyncKeyState(KEY_ADD)      && !shiftDn && !ctrlDn && !altDn);

	bool rotateLf   = (GetAsyncKeyState(KEY_LEFT)  && shiftDn && !ctrlDn && !altDn);
	bool rotateRt   = (GetAsyncKeyState(KEY_RIGHT) && shiftDn && !ctrlDn && !altDn);

	bool rotateLightLf = (GetAsyncKeyState(KEY_LEFT)  && !shiftDn && ctrlDn && !altDn);
	bool rotateLightRt = (GetAsyncKeyState(KEY_RIGHT) && !shiftDn && ctrlDn && !altDn);
	bool rotateLightUp = (GetAsyncKeyState(KEY_UP)    && !shiftDn && ctrlDn && !altDn);
	bool rotateLightDn = (GetAsyncKeyState(KEY_DOWN)  && !shiftDn && ctrlDn && !altDn);

	bool tiltUp    = (GetAsyncKeyState(KEY_DOWN) && shiftDn && !ctrlDn && !altDn);
	bool tiltDn    = (GetAsyncKeyState(KEY_UP)   && shiftDn && !ctrlDn && !altDn);
	bool tiltNormal= (GetAsyncKeyState(KEY_HOME) && !shiftDn && !ctrlDn && !altDn);

	// Apply camera movement
	if (scrollLf) { eye->moveLeft(scrollFactor);  syncScrollBars(); }
	if (scrollRt) { eye->moveRight(scrollFactor); syncScrollBars(); }
	if (scrollDn) { eye->moveDown(scrollFactor);  syncScrollBars(); }
	if (scrollUp) { eye->moveUp(scrollFactor);    syncScrollBars(); }

	if (zoomOut)
	{
		eye->ZoomOut(zoomInc * frameFactor * eye->getScaleFactor());
		if (eye->getScaleFactor() <= 0.3) renderTrees = false;
		syncScrollBars();
		tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
	}
	if (zoomIn)
	{
		eye->ZoomIn(zoomInc * frameFactor * eye->getScaleFactor());
		if (eye->getScaleFactor() > 0.3) renderTrees = true;
		syncScrollBars();
		tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
	}

	if (tiltDn)    { eye->tiltDown(scrollFactor * frameFactor * 10.0f); syncScrollBars(); tacMap.Invalidate(FALSE); }
	if (tiltUp)    { eye->tiltUp  (scrollFactor * frameFactor * 10.0f); syncScrollBars(); tacMap.Invalidate(FALSE); }
	if (tiltNormal){ eye->tiltNormal();                                  syncScrollBars(); tacMap.Invalidate(FALSE); }

	if (rotateLf)
	{
		realRotation += degPerSecRot * frameFactor;
		if (realRotation >= rotationInc) { eye->rotateLeft(rotationInc); realRotation = 0; }
		syncScrollBars();
	}
	if (rotateRt)
	{
		realRotation -= degPerSecRot * frameFactor;
		if (realRotation <= -rotationInc) { eye->rotateRight(rotationInc); realRotation = 0; }
		syncScrollBars();
	}

	if (rotateLightRt) eye->rotateLightRight(rotationInc);
	if (rotateLightLf) eye->rotateLightLeft(rotationInc);
	if (rotateLightUp) eye->rotateLightUp(rotationInc);
	if (rotateLightDn) eye->rotateLightDown(rotationInc);
}

void EditorInterface::KillCurBrush()
{
	if ( curBrush ) // might want to do a check to make sure this guy isn't being used
	{
		delete curBrush;
	}

	curBrush = NULL;

	selecting = false;

	ChangeCursor( IDC_MC2ARROW );
}

// EDITOR-SIMPLIFY-S1: single source of truth for the active tool.
//
// Previously every brush-switch site inlined the same triplet
//   KillCurBrush(); curBrush = new XBrush(...); currentBrushID = ...;  [currentBrushMenuID = ...]
// and several sites forgot to update currentBrushMenuID, which left the ImGui
// toolbar highlight (currentBrushMenuID == tools[i].cmdId, ~:5272) stale/wrong for
// those tools. This helper folds the COMMON teardown + assignment into one place:
//   - KillCurBrush() (delete old brush, curBrush=NULL, selecting=false, default cursor)
//   - curBrush / currentBrushID / currentBrushMenuID set together.
// Call sites keep their own ChangeCursor() (cursor is per-tool) and any site-specific
// state. currentBrushID values are UNCHANGED (other code range-tests them); only
// currentBrushMenuID is now always set consistently.
//
// menuId convention:
//   - tools WITH a toolbar entry (Select/Flatten/Erase/Mine/Link/Damage): pass the
//     toolbar tools[].cmdId (the base ID_OTHER_* — independent of any +variant offset
//     baked into currentBrushID) so the highlight matches.
//   - tools WITHOUT a toolbar entry (object placement, overlay, terrain, sculpt,
//     stamp, drop-zone): pass brushId. These ids never equal a tools[].cmdId, so the
//     highlight check simply never lights up for them — which is correct.
void EditorInterface::setActiveBrush( Brush* newBrush, int brushId, int menuId )
{
	KillCurBrush();
	curBrush          = newBrush;
	currentBrushID    = brushId;
	currentBrushMenuID = menuId;
}

int EditorInterface::PaintDirtRoad()
{
	return PaintOverlay( DIRT_ROAD, PAINT_OVERLAY_DIRT );
}

int EditorInterface::PaintRocks()
{
	return PaintOverlay( ROUGH, PAINT_OVERLAY_ROUGH );
}

int EditorInterface::PaintPaved()
{
	return PaintOverlay( PAVED_ROAD, PAINT_OVERLAY_PAVED );
}

int EditorInterface::PaintTwoLaneDirtRoad()
{
	return PaintOverlay( TWO_LANE_DIRT_ROAD, PAINT_OVERLAY_PAVED );
}

int EditorInterface::PaintDamagedRoad()
{
	return PaintOverlay( DAMAGED_ROAD, PAINT_OVERLAY_PAVED );
}

int EditorInterface::PaintRunway()
{
	return PaintOverlay( RUNWAY, PAINT_OVERLAY_PAVED );
}

int EditorInterface::PaintBridge()
{
	return PaintOverlay( OBRIDGE, PAINT_OVERLAY_PAVED );
}

int EditorInterface::PaintDamagedBridge()
{
	return PaintOverlay( DAMAGED_BRIDGE, PAINT_OVERLAY_PAVED );
}


int EditorInterface::PaintOverlay( int type, int message )
{
	// No toolbar entry -> menuId = brushId (message). Highlight never matches it.
	setActiveBrush( new OverlayBrush( type ), message, message );
	ChangeCursor( IDC_PAINT );

	return true;
}

int EditorInterface::PaintTerrain( int type )
{
	if ( selecting && ( land->hasSelection() ) )
	{
		TerrainBrush	brush( type );
		Action* pRetAction = brush.applyToSelection( );
		if ( pRetAction )
			undoMgr.AddAction( pRetAction );
	}
	else
	{
			// No toolbar entry -> menuId = brushId (type). Highlight never matches it.
			setActiveBrush( new TerrainBrush( type ), type, type );
			ChangeCursor( IDC_PAINT );

	}
	return true;

}

int EditorInterface::SaveAs()
{
	int retVal = IDOK;
	CFileDialog	fileDlg( 0,  "pak", NULL, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR, szPAKFilter );

	{
		/* if the mission directory doesn't exist, we attempt to create it */
		int curDirStrSize = GetCurrentDirectory(0, NULL);
		TCHAR *curDirStr = (TCHAR *)gos_Malloc(curDirStrSize);
		GetCurrentDirectory(curDirStrSize, curDirStr);
		BOOL result = SetCurrentDirectory(missionPath);
		SetCurrentDirectory(curDirStr);
		delete curDirStr; curDirStr = 0;

		if (0 == result)
		{
			gosASSERT(false);
			CreateDirectory(missionPath, NULL);
			CreateDirectory(warriorPath, NULL);
			CreateDirectory(terrainPath, NULL);
			CreateDirectory(texturePath, NULL);
		}
	}

	// When a Mod Project is open, default the Save As dialog to <root>\data\missions.
	{ const char* pd = EditorModProject::SaveDirOverride();
	  fileDlg.m_ofn.lpstrInitialDir = pd ? pd : missionPath; }
	retVal = fileDlg.DoModal();
	if (  IDOK == retVal )
	{
		SetBusyMode();
		const char* pFile = fileDlg.m_ofn.lpstrFile;
		
		//-----------------------------------------------------
		//New Stuff here.
		// Must resave the following for the new map methods to insure goodness.
		// 	-Base height Map (in terrainPath)
		//	-Hi-res height Map (in terrainPath)
		//	-Color Map (texturePath)
		//	-Color Map Burnin (texturePath)
		//	-Detail Map (texturePath)
		//	-Water Map (texturePath)
		//	-Water Detail Maps (texturePath)
		char name[1024];
		char name2[1024];
		_splitpath(pFile,NULL,NULL,name2,NULL);

		if (EditorData::instance->getMapName())
			_splitpath(EditorData::instance->getMapName(),NULL,NULL,name,NULL);
		else
			strcpy(name,name2);
		
		FullPathFileName oldBaseFile;
		oldBaseFile.init(terrainPath,name,".tga");
		
		FullPathFileName newBaseFile;
		newBaseFile.init(terrainPath,name2,".tga");
		
		SetFileAttributes(newBaseFile,FILE_ATTRIBUTE_NORMAL);
		CopyFile(oldBaseFile,newBaseFile,false);
		
		oldBaseFile.init(terrainPath,name,".height.tga");
		newBaseFile.init(terrainPath,name2,".height.tga");
		
		SetFileAttributes(newBaseFile,FILE_ATTRIBUTE_NORMAL);
		CopyFile(oldBaseFile,newBaseFile,false);
		
		oldBaseFile.init(texturePath,name,".tga");
		newBaseFile.init(texturePath,name2,".tga");
		
		SetFileAttributes(newBaseFile,FILE_ATTRIBUTE_NORMAL);
		CopyFile(oldBaseFile,newBaseFile,false);
		
		oldBaseFile.init(texturePath,name,".burnin.tga");
		newBaseFile.init(texturePath,name2,".burnin.tga");
		
		SetFileAttributes(newBaseFile,FILE_ATTRIBUTE_NORMAL);
		CopyFile(oldBaseFile,newBaseFile,false);
		
		oldBaseFile.init(texturePath,name,".detail.tga");
		newBaseFile.init(texturePath,name2,".detail.tga");
		
		SetFileAttributes(newBaseFile,FILE_ATTRIBUTE_NORMAL);
		CopyFile(oldBaseFile,newBaseFile,false);
		
		oldBaseFile.init(texturePath,name,".water.tga");
		newBaseFile.init(texturePath,name2,".water.tga");
		
		SetFileAttributes(newBaseFile,FILE_ATTRIBUTE_NORMAL);
		CopyFile(oldBaseFile,newBaseFile,false);

		for (long i=0;i<MAX_WATER_DETAIL_TEXTURES;i++)
		{
			char detailExt[256];
			sprintf(detailExt,".water%04d.tga",i);
			oldBaseFile.init(texturePath,name,detailExt);
			newBaseFile.init(texturePath,name2,detailExt);
			
			SetFileAttributes(newBaseFile,FILE_ATTRIBUTE_NORMAL);
			CopyFile(oldBaseFile,newBaseFile,false);
		}

		//Then do a regular save of everything else!!		
     	EditorData::instance->save( pFile );
		UnsetBusyMode();
	}

	return retVal;
}

int EditorInterface::Save()
{
	const char* pFileName = EditorData::instance->getMapName();
	if ( pFileName && (0 != strcmp("data\\missions\\newmap.pak", pFileName)) )
	{
		EditorData::instance->save( pFileName );
	}
	else
		return SaveAs();	

	return IDOK;
}

int EditorInterface::QuickSave()
{
	const char* pFileName = EditorData::instance->getMapName();
	if ( pFileName && (0 != strcmp("data\\missions\\newmap.pak", pFileName)) )
		EditorData::instance->quickSave( pFileName );
	else
		return SaveAs();	

	return true;
}

int EditorInterface::Undo()
{
	if ( undoMgr.HaveUndo() )
		undoMgr.Undo();

	return true;
} 

int EditorInterface::Redo()
{
	if ( undoMgr.HaveRedo() )
		undoMgr.Redo();
	
	return true;
}

// MRU list of placed objects, shared by the menu path and the companion panel.
static ObjectRecentRing g_objectRecentRing;

const ObjectRecentRing& EditorInterface::objectRecentRing() { return g_objectRecentRing; }

// Current placement alignment, read from the team-alignment menu radio (the
// single source the menu path has always used).
int EditorInterface::currentAlignmentFromMenu()
{
	int alignment = EDITOR_TEAM2;
	for ( int j = 0; j < 9; ++j )
	{
		if ( GetParent()->GetMenu()->GetMenuState( ID_ALIGNMENT_TEAM1 + j, MF_BYCOMMAND ) & MF_CHECKED )
		{
			if (j != 8)
				alignment = EDITOR_TEAM1 + j;
			else
				alignment = EDITOR_TEAMNONE;
		}
	}
	return alignment;
}

// The WM_COMMAND message id the menu uses for a given object slot
// (IDS_OBJECT_200 + cumulative building count of preceding groups + index).
int EditorInterface::objectMessageId( int group, int indexInGroup )
{
	EditorObjectMgr* pMgr = EditorObjectMgr::instance();
	int id = IDS_OBJECT_200;
	for ( int i = 0; i < group; ++i )
		id += pMgr->getNumberBuildingsInGroup( i );
	return id + indexInGroup;
}

// Make (group, indexInGroup) the active placement object. Shared by paintBuildings()
// (menu) and the companion panel so both behave identically: same mover-neutral
// guard, same alignment source, same brush construction, and MRU tracking.
bool EditorInterface::selectBuildingObject( int group, int indexInGroup )
{
	EditorObjectMgr* pMgr = EditorObjectMgr::instance();
	if ( group < 0 || group >= pMgr->getBuildingGroupCount() )
		return false;
	if ( indexInGroup < 0 || indexInGroup >= pMgr->getNumberBuildingsInGroup( group ) )
		return false;

	const int alignment = currentAlignmentFromMenu();

	// Movers (groups 4 and 6) must not be neutral.
	if (((group == 4) || (group == 6)) && (alignment == EDITOR_TEAMNONE))
	{
		EMessageBox(IDS_NO_NEUTRAL_MOVERS,IDS_ERROR,MB_OK);
		return false;
	}

	// Scatter mode paints many jittered copies in a radius (the generalised forest
	// brush); normal mode places one object per click.
	Brush* objBrush = m_scatterMode
		? (Brush*)new ScatterBrush( group, indexInGroup, alignment )
		: (Brush*)new BuildingBrush( group, indexInGroup, alignment );
	const int message = objectMessageId( group, indexInGroup );
	// Object placement has no toolbar entry; menuId == brushId == message (unchanged).
	setActiveBrush( objBrush, message, message );

	g_objectRecentRing.push( group, indexInGroup );
	return true;
}

int	EditorInterface::paintBuildings( int message )
{
	EditorObjectMgr* pMgr = EditorObjectMgr::instance();
	int groupCount = pMgr->getBuildingGroupCount();

	int id = IDS_OBJECT_200;
	for ( int i = 0; i < groupCount; ++i )
	{
		int buildCount = pMgr->getNumberBuildingsInGroup( i );
		if ( id <= message && message < id + buildCount )
		{
			selectBuildingObject( i, message - id );
			break;
		}
		id += buildCount;
	}

	return true;
}

int EditorInterface::Erase()
{
	if ( selecting && ( EditorObjectMgr::instance()->hasSelection() ||
		land->hasSelection() ) )
	{
		Eraser tmp;
		Action* pRetAction = tmp.applyToSelection( );
		if ( pRetAction )
			undoMgr.AddAction( pRetAction );
	}
	else if (0 == dynamic_cast<Eraser *>(curBrush))
	{
		setActiveBrush( new Eraser(), ID_OTHER_ERASE, ID_OTHER_ERASE );
		ChangeCursor( IDC_ERASER );
	}
	
	return true;
}

void EditorInterface::update()
{
	if ( !bThisIsInitialized )
		return;

	// Drain finished async editor tasks (terrain gen, etc.) and fire their
	// main-thread result callbacks.  MUST run on the main thread; this is it.
	EditorTaskRunner::PumpMainThread();

	// BUG A fix -- deferred pick re-open. handleLeftButtonDown captured the picked
	// world XY (marker or area-center) and armed pendingPickReopen instead of
	// calling Team() under mouse capture. Reopen the objectives dialog chain HERE,
	// the per-frame point with no modal/capture on the stack -- mirroring the
	// pendingLocate consumer below. pendingPickResultReady stays set: the re-opened
	// ObjectiveDlg/TargetAreaDlg OnInitDialog consumer reads and clears it (and the
	// pendingPickTarget discriminator routes marker vs area). Do NOT clear
	// pendingPickResultReady or leave ObjectSelectOnlyMode here. Distinct flag from
	// pendingLocate, and this returns, so only one consumer fires per frame.
	if ( objectivesEditState.pendingPickReopen )
	{
		objectivesEditState.pendingPickReopen = false;
		Team( objectivesEditState.alignment );
		return;
	}

	// Slice 3 -- Locate (PICKER-SELECT-POINT-RECON-1 section 6). The Objective
	// dialog's "Locate" button armed pendingLocate and EndDialog'd to unwind the
	// modal stack. Unlike the marker pick (which waits for a map click in
	// handleLeftButtonDown), Locate needs NO click: it fires here, the per-frame
	// main-thread tick, as soon as the modal stack is gone. Center the camera on
	// the target world XY and (only when a live, appearance-backed object was
	// forwarded) select/highlight it, then disarm and leave ObjectSelectOnlyMode.
	// Locate means "show it on the map": do NOT reopen the objectives dialog chain.
	// The old Team() reopen re-posted the objectives dialog in a stuck/locked state
	// over the camera result, forcing the user to close it before the jump was
	// usable. Leaving the dialogs closed shows the camera result immediately. (The
	// SEPARATE pendingPickReopen consumer above keeps its Team() call.)
	if ( objectivesEditState.pendingLocate )
	{
		const float locX = objectivesEditState.pendingLocateX;
		const float locY = objectivesEditState.pendingLocateY;
		EditorObject *pLocObj = objectivesEditState.pendingLocateObj;

		// Disarm the flags up front so a later frame's update() cannot re-fire this
		// consumer on stale state.
		objectivesEditState.pendingLocate = false;
		objectivesEditState.pendingLocateObj = 0;

		EditorObjectMgr *pMgr = EditorObjectMgr::instance();
		if ( pMgr )
		{
			pMgr->unselectAll();
			// select() internally re-checks appearance() and no-ops if null, but we
			// only forwarded ptrs that were non-null with a non-null appearance.
			if ( pLocObj )
				pMgr->select( *pLocObj, true );
		}

		if ( eye )
			eye->setPosition( Stuff::Vector3D( locX, locY, 0.0f ), true );

		// Leave the object-select handoff. Do NOT reopen the objectives dialog
		// chain: Locate ends with the dialogs closed so the camera move is visible.
		ObjectSelectOnlyMode( false );
		return;
	}

#ifdef MC2_IMGUI
	// An async Generate task has finished -> apply the terrain here on the main thread
	// in the EXACT order the old blocking Generate path (and LoadPreset) used.
	if ( MapGeneratorDialog::GenerateReady() )
	{
		//   1. eye->reset()  -- camera zoom/rotation/frustum back to defaults FIRST.
		//   2. apply         -- generateFromDialogParams primes the terrain face cache
		//                       against the CURRENT camera; priming after reset culls
		//                       against the final view. Priming BEFORE reset (the old
		//                       async bug) left the whole terrain culled -> empty/black,
		//                       only the debug grid visible.
		//   3. setPosition() -- derives z from getTerrainElevation, lifting the camera
		//                       above the generated surface.
		eye->reset();
		bool applied = MapGeneratorDialog::ApplyPendingGenerate();
		if ( applied && land )
		{
			eye->setPosition( Stuff::Vector3D(0.0f, 0.0f, 0.0f), false );
			addBuildingsToNewMenu();
			syncScrollBars();
			initTacMap();
		}
		if ( applied )
		{
			tacMap.UpdateMap();
			PlaySound("SystemDefault", NULL, SND_ASYNC);
		}
	}
#endif

	// Headless terrain-probe capture: when MC2_TERRAIN_PROBE=1, emit one TERRAIN_PROBE
	// line the first frame a map is loaded (and again after each (re)load via the
	// land-pointer-change guard). Lets a -gen-map / -mission smoke capture same-session
	// before/after-fix evidence without the ImGui panel. Read-only.
	{
		static bool   s_probeEnabled = ( getenv("MC2_TERRAIN_PROBE") != NULL );
		static void*  s_lastProbedLand = (void*)1;   // != NULL and != any real land yet
		if ( s_probeEnabled && land && (void*)land != s_lastProbedLand )
		{
			s_lastProbedLand = (void*)land;
			EditorDebugOverlay::RunProbeOnce();
		}
	}

	// Deferred "Generate Map" (legacy MFC path — kept for non-ImGui builds).
	if ( m_pendGenerateMission )
	{
		m_pendGenerateMission = false;
		NewGeneratedMission();
		return;
	}

#ifdef MC2_IMGUI
	// ImGui Map Generator dialog: handle preview / generate actions deferred
	// outside the ImGui render pass (so blocking python shell-outs are safe).
	{
		MapGeneratorDialog::PendingAction act = MapGeneratorDialog::TakeAction();
		if (act == MapGeneratorDialog::PendingAction::Preview)
		{
			// Async: starts a background task and returns immediately (no UI block).
			MapGeneratorDialog::ExecutePreview();
		}
		else if (act == MapGeneratorDialog::PendingAction::Generate)
		{
			// Async: starts a background task and returns immediately. The terrain
			// apply + camera/UI re-seat happen later via TakePostGenerateApplied()
			// once the subprocess succeeds (see below).
			MapGeneratorDialog::ExecuteGenerate();
		}
		else if (act == MapGeneratorDialog::PendingAction::Import)
		{
			// Import an external heightfield (Gaea / Blender / EXR). Async, and
			// applies via the SAME deferred path as Generate (GenerateReady() ->
			// ApplyPendingGenerate()), so no separate apply needed here.
			MapGeneratorDialog::ExecuteImport();
		}
		else if (act == MapGeneratorDialog::PendingAction::LoadPreset)
		{
			// Load pre-baked flat preset: same camera/UI setup as Generate.
			SetBusyMode();
			eye->reset();
			MapGeneratorDialog::ExecuteLoadPreset();
			UnsetBusyMode();
			if (!MapGeneratorDialog::IsOpen())
			{
				if ( land )
					eye->setPosition( Stuff::Vector3D(0.0f, 0.0f, 0.0f), false );
				if ( land )
				{
					addBuildingsToNewMenu();
					syncScrollBars();
					initTacMap();
				}
				tacMap.UpdateMap();
				PlaySound("SystemDefault", NULL, SND_ASYNC);
			}
			return;
		}
	}

	// Mission Save Readiness checklist: handle per-check action buttons and
	// "Prepare Saveable Mission" outside the ImGui render pass so MFC dialogs
	// (SaveAs, Team) can be called safely.
	{
		ChecklistAction checklistAct = MissionValidator::TakeAction();
		switch (checklistAct)
		{
			case ChecklistAction::OpenMapGenerator:
				MapGeneratorDialog::Open();
				break;

			case ChecklistAction::OpenSaveAs:
				SaveAs();
				MissionValidator::QueueAction(ChecklistAction::None); // clear any re-queue
				break;

			case ChecklistAction::OpenObjectives:
				Team(0);   // opens Mission > Teams > Team 1 (objectives live here)
				break;

			case ChecklistAction::BuildMove:
			{
				// Build MOVE pathfinding in-memory from the current terrain + objects.
				// Sets GameMap + GlobalMoveMap[0..2] so passability grid and AI work.
				// No save needed -- results are immediately usable.
				std::string moveErr;
				bool moveOk = EditorData::RebuildMoveFromCurrentTerrain(&moveErr);
				if (!moveOk && !moveErr.empty()) {
					printf("[EDITOR_UPDATE] BuildMove failed: %s\n", moveErr.c_str());
					fflush(stdout);
				}
				// Do NOT auto-show the passability overlay here: drawLine() iterates
				// 9 sub-cells per visible quad via GameMap->getCell(), which becomes
				// hundreds of thousands of GL calls per frame on large maps and kills
				// editor panning performance. The user can toggle View > Show Passability
				// Map manually when they want to inspect it.
				break;
			}

			case ChecklistAction::PrepareSaveable:
			{
				// Re-validate and trigger the first failing check's action.
				// Does NOT create mission content -- only navigates to the
				// right editor panel.
				auto checks = MissionValidator::ValidateForPakSave();
				for (const auto& c : checks)
				{
					if (!c.passed
					 && c.action != ChecklistAction::None
					 && c.action != ChecklistAction::PrepareSaveable)
					{
						MissionValidator::QueueAction(c.action);
						break;
					}
				}
				break;
			}

			default:
				break;
		}

		// MOVE rebuild is now user-triggered, not automatic.
		// Preset/generate load calls MarkMoveDataDirty(); the user then clicks
		// "Build MOVE" in the Mission Checklist (ChecklistAction::BuildMove above)
		// or saves (save() calls RebuildMoveFromCurrentTerrain internally).
		// Eager auto-rebuild was removed because it crashed on systemHeap exhaustion
		// before any user interaction was possible.
	}
#endif

	if ( curBrush )
	{
		POINT pt;
		GetCursorPos( &pt );
		ScreenToClient( &pt );
		
		curBrush->update( pt.x, pt.y );
	}
}

void EditorInterface::render()
{
	if ( !bThisIsInitialized )
		return;

	// Poll keyboard + mouse-edge every render frame so camera responds at render rate
	// (~100 fps) rather than WM_KEYDOWN key-repeat rate (~30 Hz).
	updateCameraInput();

	// Phase 5: PCG foliage billboard preview, drawn over the already-rendered
	// terrain. Pure visual overlay -- no terrain/save/load state touched.
	FoliageRender::Render( eye );

	// Phase 2: diagnostic world overlays (chunk/superchunk grid, water bounds).
	// Pure visual; same frame/projection context as the foliage preview.
	EditorDebugOverlay::RenderWorldOverlay( eye );

	// Selected unit's patrol/move path overlay (only while the AI/Brain panel is open).
	EditorDebugOverlay::RenderPatrolPaths( eye );

	ModifyStyle( 0, WS_HSCROLL | WS_VSCROLL );

	Stuff::Vector3D worldPos;
	Stuff::Vector2DOf<long> screenPos;

	screenPos.x = Environment.screenWidth/2;
	screenPos.y = Environment.screenHeight/2;

	//eye->inverseProject( screenPos, worldPos );
	/*if ( worldPos.x != 0.0 || worldPos.y != 0.0 )
	{

		worldPos.y += 128.f;
		
		Stuff::Vector4D screenPos2;

		eye->projectZ( worldPos, screenPos2 );

		gos_VERTEX vertices[2];

		memset( vertices, 0, sizeof( vertices ) );

		float val = 1.4142135623730950488016887242097f;

		Stuff::Vector2DOf<float> screenVector;
		screenVector.x = screenPos.x - screenPos2.x;
		screenVector.y = screenPos.y - screenPos2.y;

		screenVector.Normalize( screenVector );

		Rotate( screenVector, 45.f );


		vertices[0].x = (float)screenPos.x;
		vertices[0].y = (float)screenPos.y;
		vertices[0].argb = 0xffff0000;
		vertices[0].rhw = 1.0;

		vertices[1].x = screenPos2.x;
		vertices[1].y = screenPos2.y;
		vertices[1].argb = 0xffff0000;
		vertices[1].rhw = 1.0;

		gos_DrawLines( vertices, 2 );

		Stuff::Vector4D arrowVector;
		arrowVector.x = screenVector.x + val;
		arrowVector.x = screenVector.x;
		
		vertices[0].x = screenPos2.x + 10 * screenVector.x;
		vertices[0].y = screenPos2.y + 10 * screenVector.y;

		gos_DrawLines( vertices, 2 );

		Rotate( screenVector, -90.f );
		vertices[0].x = screenPos2.x + 10 * screenVector.x;
		vertices[0].y = screenPos2.y + 10 * screenVector.y;
		gos_DrawLines( vertices, 2 );
		}*/


	if ( curBrush )
	{
		POINT pt;
		GetCursorPos( &pt );
		ScreenToClient( &pt );

		//curBrush->beginPaint();
		// RTT: the brush preview unprojects to world, so it needs viewport-local
		// coords. Offset a copy; the edge-scroll logic below stays in full-window
		// space (it compares against Width()).
		int brushX = (int)pt.x, brushY = (int)pt.y;
		EditorRttClientToViewport( brushX, brushY );
		curBrush->render( brushX, brushY );
		renderTerrainSelection();
		//curBrush->endPaint();

		if ( painting && !dragging )
		{
			GetCursorPos( &pt );
			ScreenToClient( &pt );
			int PosX = (int)pt.x;
			int PosY = (int)pt.y;

			// scroll if painting
			int scrollLf = (PosX <= (screenScrollLeft));
			int scrollRt = (PosX >= (Width() - screenScrollRight));
			
			int scrollUp = (PosY <= (screenScrollUp));
			int scrollDn = (PosY >= (Height() - screenScrollDown));

			float frameFactor = frameLength / baseFrameLength;
			float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;


			if (scrollLf)
			{
				eye->moveLeft(scrollFactor);
				syncScrollBars();
			}

			if (scrollRt)
			{
				eye->moveRight(scrollFactor);
				syncScrollBars();
			}

			if (scrollDn)
			{
				eye->moveDown(scrollFactor);
				syncScrollBars();
			}

			if (scrollUp)
			{
				eye->moveUp(scrollFactor);
				syncScrollBars();
			}
		}


	}
}

int EditorInterface::NewHeightMap()
{
	CFileDialog	fileDlg( 1,  "tga", NULL, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = terrainPath;

	bool endFlag = false;
	while (!endFlag)
	{
		endFlag = true;
		if (  IDOK == fileDlg.DoModal() )
		{
			const char* pFile = fileDlg.m_ofn.lpstrFile;
			
			if (pFile)
			{
				File tgaFile;

				gosASSERT( strstr( pFile, ".tga" ) || strstr( pFile, ".TGA" ) );

				long result = tgaFile.open(const_cast<char*>(pFile));
				gosASSERT(result == NO_ERR);

				struct TGAFileHeader theader;
				tgaFile.read((MemoryPtr)&theader,sizeof(TGAFileHeader));

				if ((theader.width != land->realVerticesMapSide) || (theader.height != land->realVerticesMapSide))
				{
					/*wrong size*/
					AfxMessageBox(IDS_WRONG_SIZE_HEIGHT_MAP);
					endFlag = false;
					continue;
				}
			}
			else
			{
				gosASSERT(false);
				endFlag = false;
				continue;
			}

			HeightDlg htDlg;
			long tileR, tileC;

			htDlg.SetMin( (int)land->getLowestVertex( tileR, tileC ) );
			htDlg.SetMax( (int)land->getHighestVertex( tileR, tileC ) );
			
			bool chkStatus = false;
			if ( IDOK == htDlg.DoModal() )
			{
				SetBusyMode();
				chkStatus = EditorData::reassignHeightsFromTGA( pFile, htDlg.GetMin(), htDlg.GetMax() );
				UnsetBusyMode();
			}

			if (chkStatus)
				tacMap.UpdateMap();
		}
	}

	return true;
}

#define MAX_THRESHOLD	5
#define MAX_NOISE		5
#define MIN_THRESHOLD	1
#define MIN_NOISE		0

int EditorInterface::RefractalizeTerrain(long Threshold)
{
	FractalDlg fDlg;
	fDlg.SetThreshold(Terrain::fractalThreshold);
	fDlg.SetNoise(Terrain::fractalNoise);
	
	if (IDOK == fDlg.DoModal())
	{
		SetBusyMode();
		Terrain::fractalThreshold = fDlg.GetThreshold();
		Terrain::fractalNoise = fDlg.GetNoise();

		if (Terrain::fractalThreshold > MAX_THRESHOLD)
			Terrain::fractalThreshold = MAX_THRESHOLD;
			
		if (Terrain::fractalThreshold < MIN_THRESHOLD)
			Terrain::fractalThreshold = MIN_THRESHOLD;
 			
		if (Terrain::fractalNoise > MAX_NOISE)
			Terrain::fractalNoise = MAX_NOISE;
			
		if (Terrain::fractalNoise < MIN_NOISE)
			Terrain::fractalNoise = MIN_NOISE;
			
 		if (Terrain::terrainTextures2)
			Terrain::terrainTextures2->refractalizeBaseMesh(Terrain::terrainName, Terrain::fractalThreshold, Terrain::fractalNoise);
		UnsetBusyMode();
	}
		
	return true;
}

int EditorInterface::SetSky (long skyId)
{
	EditorData::instance->TheSkyNumber(skyId);

	return true;
}

int EditorInterface::Select()
{
	if ( currentBrushID == IDS_SELECT )
	{
		land->unselectAll();
		EditorObjectMgr::instance()->unselectAll();
	}
	else
	{
		int radius = GetParent()->GetMenu()->GetMenuState( ID_DRAGNORMAL, MF_BYCOMMAND ) & MF_CHECKED? -1 : smoothRadius;
		setActiveBrush( new SelectionBrush( false, radius ), IDS_SELECT, ID_OTHER_SELECT );

		ChangeCursor( IDC_MC2ARROW );
	}

	selecting = true;

	return true;
}

int EditorInterface::Flatten()
{
	if ( selecting && ( land->hasSelection() ) )
	{
		FlattenBrush brush;
		Action* pAction = brush.applyToSelection();
		if ( pAction )
		{
			undoMgr.AddAction( pAction );
		}
	}
	else if (0 == dynamic_cast<FlattenBrush *>(curBrush))
	{
		setActiveBrush( new FlattenBrush(), IDS_FLATTEN, ID_OTHER_FLATTEN );
		ChangeCursor( IDC_FLATTEN );
	}

	return true;

}

int EditorInterface::SaveCameras()
{
	if ( !EditorData::getMapName() )
	{
		// message box that you must save here
		EMessageBox( IDS_SAVE_CAMERA_FAIL, IDS_ERROR, MB_OK );
	}
	else
	{
		char	base[256];

		strcpy( base, cameraPath );
		strcat( base, EditorData::getMapName() );
		strcat( base, "cam" );
		strcat( base, ".fit" );

		FitIniFile file;
		file.open( base );

		return eye->save( &file );

	}

	return true;
}

int EditorInterface::SelectSlopes()
{
	SelectSlopeDialog dlg;
	if ( dlg.DoModal() == IDOK )
	{
		float minAngle = dlg.m_MinEdit;
		float maxAngle = dlg.m_MaxEdit;

		float minHeight = float(fabs(float(tan( minAngle * float(PI)/180.f ) * land->worldUnitsPerVertex)));
		float maxHeight = float(fabs(float(tan( maxAngle * float(PI)/180.f ) * land->worldUnitsPerVertex)));

		float centerElv;
		float tmpElv;

		int left;
		int right;
		int top;
		int bottom;

		land->unselectAll();

		for ( int j = 0; j < land->realVerticesMapSide; ++j )
		{
			for ( int i = 0; i < land->realVerticesMapSide; ++i )
			{
				left = i > 0 ? i - 1 : 0;
				right = i < land->realVerticesMapSide ? i + 1 : i;
				top = j > 0 ? j - 1 : 0;
				bottom = j < land->realVerticesMapSide ? j + 1 : j;

				centerElv = land->getVertexHeight( j * land->realVerticesMapSide + i );
				tmpElv = land->getVertexHeight( top * land->realVerticesMapSide + left ); 
				if ( fabs(tmpElv - centerElv) >= minHeight && fabs(tmpElv - centerElv) <= maxHeight )
				{
					land->selectVertex( j, i );
				}
				else
				{
					tmpElv = land->getVertexHeight( top * land->realVerticesMapSide + right );
					if ( fabs(tmpElv - centerElv) >= minHeight && fabs(tmpElv - centerElv) <= maxHeight )
					{
						land->selectVertex( j, i );
					}
					else
					{
						tmpElv = land->getVertexHeight( bottom * land->realVerticesMapSide + right );
						if ( fabs(tmpElv - centerElv) >= minHeight && fabs(tmpElv - centerElv) <= maxHeight )
						{
							land->selectVertex( j, i );
						}
						else
						{
							tmpElv = land->getVertexHeight( bottom * land->realVerticesMapSide + left );
							if ( fabs(tmpElv - centerElv) >= minHeight && fabs(tmpElv - centerElv) <= maxHeight )
							{
								land->selectVertex( j, i );
							}
						}
					}
				}
			}
		}// done looping over vertices
	}

	selecting = true;
	return true;
}

int EditorInterface::SelectAltitude()
{
	HeightDlg dlg;
	if ( dlg.DoModal() == IDOK )
	{
		float minHeight = (float)dlg.GetMin();
		float maxHeight = (float)dlg.GetMax();

		float centerElv;

		land->unselectAll();

		for ( int j = 0; j < land->realVerticesMapSide; ++j )
		{
			for ( int i = 0; i < land->realVerticesMapSide; ++i )
			{
				centerElv = land->getVertexHeight( j * land->realVerticesMapSide + i );
				if ( (centerElv >= minHeight) && (centerElv <= maxHeight) )
				{
					land->selectVertex( j, i );
				}
			}
		}// done looping over vertices
	}

	selecting = true;
	return true;
}

int EditorInterface::SelectTerrainType()
{
	SelectTerrainTypeDlg dlg;
	dlg.SelectedTerrainType(-1);
	if (( dlg.DoModal() == IDOK ) && (ID_TERRAINS_BLUEWATER <= dlg.SelectedTerrainType()))
	{
		const int selectedTerrainType = dlg.SelectedTerrainType() - ID_TERRAINS_BLUEWATER;
		land->unselectAll();

		for ( int j = 0; j < land->realVerticesMapSide; ++j )
		{
			for ( int i = 0; i < land->realVerticesMapSide; ++i )
			{
				int tmp = land->getTerrain( j, i );
				if (land->getTerrain( j, i ) == selectedTerrainType)
				{
					land->selectVertex( j, i );
				}
			}
		}// done looping over vertices
	}

	selecting = true;
	return true;
}

int EditorInterface::PurgeTransitions()
{
	land->purgeTransitions();

	highlighted = false;
	Terrain::mapData->unhighlightAll();

	return true;
}

int EditorInterface::ShowTransitions()
{
	highlighted ^= true;
	if (highlighted)
		Terrain::mapData->highlightAllTransitionsOver2();
	else
		Terrain::mapData->unhighlightAll();

	return true;
}

int EditorInterface::Fog()
{
	FogDlg dlg;
	dlg.m_blue = (eye->dayFogColor) & 0xff;
	dlg.m_green = (eye->dayFogColor >> 8) & 0xff;
	dlg.m_red = (eye->dayFogColor >> 16) & 0xff;
	dlg.m_start = eye->fogStart;
	dlg.m_end = eye->fogFull;

	if ( IDOK == dlg.DoModal() )
	{
		eye->dayFogColor = ((DWORD)dlg.m_blue) + (((DWORD)dlg.m_green) << 8) + (((DWORD)dlg.m_red) << 16);
		eye->fogColor = eye->dayFogColor;
		eye->fogStart = dlg.m_start;
		eye->fogFull = dlg.m_end;

//		land->reCalcLight();
//		eye->updateDaylight();
	}

	return true;
}

int EditorInterface::Light()
{
	SunDlg dlg;
	if ( IDOK == dlg.DoModal() )
	{
		/*
		SafeRunGameOSLogic();
		land->reCalcLight();
		SafeRunGameOSLogic();
		tacMap.UpdateMap();
		*/
	}
	return true;
}

int EditorInterface::Waves()
{
	WaterDlg dlg;

	dlg.alphaDeep = Terrain::alphaDeep;
	dlg.alphaElevation = Terrain::mapData->alphaDepth;
	dlg.alphaMiddle = Terrain::alphaMiddle;
	dlg.alphaShallow = Terrain::alphaEdge;

	dlg.amplitude = Terrain::waterAmplitude;
	dlg.elevation = Terrain::mapData->waterDepth;
	dlg.frequency = Terrain::waterFreq;
	dlg.shallowElevation = Terrain::mapData->shallowDepth;

	if ( IDOK == dlg.DoModal() )
	{
		Terrain::alphaDeep = dlg.alphaDeep;
		Terrain::alphaMiddle = dlg.alphaMiddle;
		Terrain::alphaEdge = dlg.alphaShallow;

		Terrain::waterAmplitude = dlg.amplitude;
		Terrain::waterElevation = dlg.elevation;
		Terrain::waterFreq = dlg.frequency;
		Terrain::mapData->shallowDepth = dlg.shallowElevation;
		Terrain::mapData->alphaDepth = dlg.alphaElevation;
		Terrain::mapData->waterDepth = dlg.elevation;

		land->recalcWater();

		tacMap.UpdateMap();

	}

	return true;	

}


int EditorInterface::DragSmooth()
{
	if ( IDS_SELECT == currentBrushID )
	{
		currentBrushID = -1;
		currentBrushMenuID = -1;
		GetParent()->GetMenu()->CheckMenuItem( ID_DRAGSMOOTH, MF_BYCOMMAND | MF_CHECKED );
		GetParent()->GetMenu()->CheckMenuItem( ID_DRAGNORMAL, MF_BYCOMMAND | MF_UNCHECKED );
		Select();
	}

	return true;

}
int EditorInterface::DragRough()
{
	if ( IDS_SELECT == currentBrushID )
	{
		currentBrushID = -1;
		currentBrushMenuID = -1;
		GetParent()->GetMenu()->CheckMenuItem( ID_DRAGSMOOTH, MF_BYCOMMAND | MF_UNCHECKED );
		GetParent()->GetMenu()->CheckMenuItem( ID_DRAGNORMAL, MF_BYCOMMAND | MF_CHECKED );
		Select();
	}

	return true;

}

int EditorInterface::AssignElevation()
{
	if ( !land->hasSelection() )
	{
		// nothing to assign heights to, let the user know
		char buffer[256];
		EditorSafeLoadString( IDS_NO_VERTEX_SEL, buffer, 256, gameResourceHandle );
		MessageBox( buffer );
	}
	else
	{
		FlattenBrush tmp;
		float val = tmp.getAverageHeightOfSelection( );
		SingleValueDlg dlg( IDS_ASSIGN_ELEVATION, IDS_ELEVATION, (int)val );
		dlg.SetVal( (int)val );
		if ( IDOK == dlg.DoModal() )
		{
			ActionPaintTile *pAction = new ActionPaintTile;
			for ( int  j = 0; j < land->realVerticesMapSide; ++j )
			{
				for ( int i = 0; i < land->realVerticesMapSide; ++i )
				{
					if ( land->isVertexSelected( j, i ) )
					{
						land->setVertexHeight( j * land->realVerticesMapSide + i, (float)dlg.GetVal() );
					}
				}
			}

			//Action* pAction = tmp.applyHeightToSelection( (float)dlg.GetVal() );
			if ( pAction )
				undoMgr.AddAction( pAction );

			/*Designers say refreshing the tacmap takes too long. User can do it manually.*/
			//tacMap.UpdateMap();
		}
	}


	return true;
}

int EditorInterface::SmoothRadius()
{
	SingleValueDlg dlg( IDS_SMOOTH_RADIUS, IDS_RADIUS, smoothRadius );
	if ( IDOK == dlg.DoModal() )
	{
		smoothRadius = dlg.GetVal();

		
		if ( IDS_SELECT == currentBrushID )
		{
			currentBrushID = -1;
			currentBrushMenuID = -1;
			Select();
		}
	}

	return true;
}

int EditorInterface::Alignment( int specific )
{
	
	for ( int i = 0; i < 9; ++i )
		GetParent()->GetMenu()->CheckMenuItem( ID_ALIGNMENT_TEAM1 + i, MF_BYCOMMAND | MF_UNCHECKED );
		
	
	GetParent()->GetMenu()->CheckMenuItem( specific, MF_BYCOMMAND | MF_CHECKED );
	
	if ( currentBrushID >= IDS_OBJECT_200 && currentBrushID <= 30800 )
	{
		int tmp = currentBrushID;
		currentBrushID = 0;
		paintBuildings( tmp );
	}

	return true;
}

int EditorInterface::SaveHeightMap()
{
	CreateDirectory(terrainPath, NULL);
	
	CFileDialog	fileDlg( 0, "tga", NULL, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = terrainPath;
	
	if (  IDOK == fileDlg.DoModal() )
	{
		const char* pFile = fileDlg.m_ofn.lpstrFile;
		
		File file;
		if ( NO_ERR != file.create( (char*)pFile ) )
		{
			EMessageBox( IDS_INVALID_FILE, IDS_CANT_SAVE, MB_OK );
			return false;
		}
		
		SetBusyMode();
		bool ret = EditorData::saveHeightMap( &file );
		UnsetBusyMode();

		return ret;
	}

	return true;

}

int EditorInterface::MissionSettings()
{
	MissionSettingsDlg dlg;
	dlg.m_MissionNameUnlocalizedText = EditorData::instance->MissionName().Data();
	dlg.m_MissionNameUseResourceString = EditorData::instance->MissionNameUseResourceString();
	dlg.m_MissionNameResourceStringID = EditorData::instance->MissionNameResourceStringID();
	dlg.m_AuthorEdit = EditorData::instance->Author().Data();
	dlg.m_BlurbUnlocalizedText = EditorData::instance->Blurb().Data();
	dlg.m_BlurbUseResourceString = EditorData::instance->BlurbUseResourceString();
	dlg.m_BlurbResourceStringID = EditorData::instance->BlurbResourceStringID();
	dlg.m_Blurb2UnlocalizedText = EditorData::instance->Blurb2().Data();
	dlg.m_Blurb2UseResourceString = EditorData::instance->Blurb2UseResourceString();
	dlg.m_Blurb2ResourceStringID = EditorData::instance->Blurb2ResourceStringID();
	dlg.m_TimeLimit = EditorData::instance->TimeLimit();
	dlg.m_DropWeightLimit = EditorData::instance->DropWeightLimit();
	dlg.m_InitialResourcePoints = EditorData::instance->InitialResourcePoints();
	dlg.m_SinglePlayerCheck = (BOOL)EditorData::instance->IsSinglePlayer();
	dlg.m_MaxTeams = EditorData::instance->MaxTeams();
	dlg.m_MaxPlayers = EditorData::instance->MaxPlayers();
	dlg.m_ScenarioTune.Empty();
	dlg.m_ScenarioTune.Format(_T("%d"), EditorData::instance->ScenarioTune());
	dlg.m_VideoFilename = EditorData::instance->VideoFilename().Data();
	dlg.m_CBills = EditorData::instance->CBills();
	dlg.m_NumRPBuildings = EditorData::instance->NumRandomRPbuildings();
	dlg.m_DownloadUrlEdit = EditorData::instance->DownloadURL().Data();
	dlg.m_MissionType = EditorData::instance->MissionType();
	dlg.m_AirStrikeCheck = EditorData::instance->AirStrikesEnabledDefault();
	dlg.m_MineLayerCheck = EditorData::instance->MineLayersEnabledDefault();
	dlg.m_ScoutCopterCheck = EditorData::instance->ScoutCoptersEnabledDefault();
	dlg.m_SensorProbeCheck = EditorData::instance->SensorProbesEnabledDefault();
	dlg.m_UnlimitedAmmoCheck = EditorData::instance->UnlimitedAmmoEnabledDefault();
	dlg.m_AllTech = EditorData::instance->AllTechEnabledDefault();
	dlg.m_RepairVehicleCheck = EditorData::instance->RepairVehicleEnabledDefault();
	dlg.m_SalvageCraftCheck = EditorData::instance->SalvageCraftEnabledDefault();
	dlg.m_ResourceBuildingCheck = EditorData::instance->ResourceBuildingsEnabledDefault();
	dlg.m_NoVariantsCheck = EditorData::instance->NoVariantsEnabledDefault();
	dlg.m_ArtilleryPieceCheck = EditorData::instance->ArtilleryPieceEnabledDefault();
	dlg.m_RPsForMechsCheck = EditorData::instance->RPsForMechsEnabledDefault();
	if ( IDOK == dlg.DoModal() )
	{
		EditorData::instance->MissionName(dlg.m_MissionNameUnlocalizedText.GetBuffer(0));
		EditorData::instance->MissionNameUseResourceString(dlg.m_MissionNameUseResourceString);
		EditorData::instance->MissionNameResourceStringID(dlg.m_MissionNameResourceStringID);
		EditorData::instance->Author(dlg.m_AuthorEdit.GetBuffer(0));
		EditorData::instance->Blurb(dlg.m_BlurbUnlocalizedText.GetBuffer(0));
		EditorData::instance->BlurbUseResourceString(dlg.m_BlurbUseResourceString);
		EditorData::instance->BlurbResourceStringID(dlg.m_BlurbResourceStringID);
		EditorData::instance->Blurb2(dlg.m_Blurb2UnlocalizedText.GetBuffer(0));
		EditorData::instance->Blurb2UseResourceString(dlg.m_Blurb2UseResourceString);
		EditorData::instance->Blurb2ResourceStringID(dlg.m_Blurb2ResourceStringID);
		EditorData::instance->TimeLimit(dlg.m_TimeLimit);
		EditorData::instance->DropWeightLimit(dlg.m_DropWeightLimit);
		EditorData::instance->InitialResourcePoints(dlg.m_InitialResourcePoints);
		EditorData::instance->IsSinglePlayer((bool)dlg.m_SinglePlayerCheck);
		EditorData::instance->MaxTeams(dlg.m_MaxTeams);
		EditorData::instance->MaxPlayers(dlg.m_MaxPlayers);
		EditorData::instance->ScenarioTune(atoi(dlg.m_ScenarioTune.GetBuffer(0)));
		EditorData::instance->VideoFilename(dlg.m_VideoFilename.GetBuffer(0));
		EditorData::instance->CBills( dlg.m_CBills);
		EditorData::instance->NumRandomRPbuildings(dlg.m_NumRPBuildings);
		EditorData::instance->DownloadURL(dlg.m_DownloadUrlEdit.GetBuffer(0));
		EditorData::instance->MissionType(dlg.m_MissionType);
		EditorData::instance->AirStrikesEnabledDefault(dlg.m_AirStrikeCheck);
		EditorData::instance->MineLayersEnabledDefault(dlg.m_MineLayerCheck);
		EditorData::instance->ScoutCoptersEnabledDefault(dlg.m_ScoutCopterCheck);
		EditorData::instance->SensorProbesEnabledDefault(dlg.m_SensorProbeCheck);
		EditorData::instance->UnlimitedAmmoEnabledDefault(dlg.m_UnlimitedAmmoCheck);
		EditorData::instance->AllTechEnabledDefault(dlg.m_AllTech);
		EditorData::instance->RepairVehicleEnabledDefault(dlg.m_RepairVehicleCheck);
		EditorData::instance->SalvageCraftEnabledDefault(dlg.m_SalvageCraftCheck);
		EditorData::instance->ResourceBuildingsEnabledDefault(dlg.m_ResourceBuildingCheck);
		EditorData::instance->NoVariantsEnabledDefault(dlg.m_NoVariantsCheck);
		EditorData::instance->ArtilleryPieceEnabledDefault(dlg.m_ArtilleryPieceCheck);
		EditorData::instance->RPsForMechsEnabledDefault(dlg.m_RPsForMechsCheck);
	}
	return true;
}

int EditorInterface::Team( int team )
{
	static CTeams originalTeams;
	if (!ObjectSelectOnlyMode()) {
		originalTeams = EditorData::instance->TeamsRef();
	}
	EditorData::instance->DoTeamDialog(team);
	if (!ObjectSelectOnlyMode()) {
		if (!(originalTeams == EditorData::instance->TeamsRef())) {
			TeamsAction *pTeamsAction = new TeamsAction(originalTeams);
			undoMgr.AddAction(pTeamsAction);
		}
		originalTeams.Clear();
	}
	return true;
}

int EditorInterface::Player( int player )
{
	PlayerSettingsDlg dlg;
	dlg.m_playerEdit = player + 1;
	dlg.m_oldDefaultTeam = EditorData::instance->PlayersRef().PlayerRef(player).DefaultTeam();
	dlg.m_numTeams = EditorData::instance->MaxTeams();
	if ((IDOK == dlg.DoModal()) && (0 <= dlg.m_newDefaultTeam))
	{
		EditorData::instance->PlayersRef().PlayerRef(player).DefaultTeam(dlg.m_newDefaultTeam);
	}
	return true;
}

static const float SQRT_2 = 1.4142135623730950488016887242097f;
/* make the horizontal scroll bar reflect the current camera position */
void EditorInterface::syncHScroll()
{
	/* this calculation was based in the code for Camera::moveRight(float amount) */
	Stuff::Vector3D direction;
	if (!eye->usePerspective)
	{
		direction.x = 1.0;
		direction.y = 0.0;
		direction.z = 0.0;
	}
	else
	{
		direction.x = -1.0;
		direction.y = 0.0;
		direction.z = 0.0;
	}
	float worldCameraRotation = eye->getCameraRotation();
	OppRotate(direction,worldCameraRotation);

	Stuff::Vector3D eyeDisplacement = eye->getPosition();

	/* maxVisual was taken from Camera::setPosition(). */
	float maxVisual = (Terrain::worldUnitsMapSide / 2) - Terrain::worldUnitsPerVertex;
	float bound = SQRT_2 * maxVisual;

	float scrollPos = ((direction * eyeDisplacement) / bound * 0.5f + 0.5f) * HSCROLLBAR_RANGE;
	SetScrollRange( SB_HORZ, 0, HSCROLLBAR_RANGE, true );
	SetScrollPos(SB_HORZ, (int)scrollPos);

	{
		/* figure out what proportion of the map is visible */
 		Stuff::Vector2DOf< long > screen;
		Stuff::Vector3D world1, world2;
		screen.y = Environment.screenHeight / 2;
		screen.x = 1;
		eye->screenToGroundPlaneApprox( screen.x, screen.y, world1 );  // cheap: span only needs x,y
		screen.x = Environment.screenWidth - 1;
		eye->screenToGroundPlaneApprox( screen.x, screen.y, world2 );
		float dx = world2.x - world1.x;
		float dy = world2.y - world1.y;
		float span = sqrt(dx * dx + dy * dy);
		/* make the size of the scroll bar proportional to the proportion of the map that's visible */
		SCROLLINFO si;
		GetScrollInfo(SB_HORZ, &si, SIF_PAGE);
		si.nPage = HSCROLLBAR_RANGE * span / ( 2.0 * bound);
		si.fMask = SIF_PAGE;
		SetScrollInfo(SB_HORZ, &si);
	}

	tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
}

/* make the vertical scroll bar reflect the current camera position */
void EditorInterface::syncVScroll()
{
	/* this calculation was based in the code for Camera::moveRight(float amount) */
	Stuff::Vector3D direction;
	if (!eye->usePerspective)
	{
		direction.x = 0.0;
		direction.y = -1.0;
		direction.z = 0.0;
	}
	else
	{
		direction.x = 0.0;
		direction.y = 1.0;
		direction.z = 0.0;
	}
	float worldCameraRotation = eye->getCameraRotation();
	OppRotate(direction,worldCameraRotation);

	Stuff::Vector3D eyeDisplacement = eye->getPosition();

	/* maxVisual was taken from Camera::setPosition(). */
	float maxVisual = (Terrain::worldUnitsMapSide / 2) - Terrain::worldUnitsPerVertex;
	float bound = SQRT_2 * maxVisual;

	float scrollPos = ((direction * eyeDisplacement) / bound * 0.5f + 0.5f) * VSCROLLBAR_RANGE;
	SetScrollRange( SB_VERT, 0, VSCROLLBAR_RANGE, false );
	SetScrollPos(SB_VERT, (int)scrollPos);

	{
		/* figure out what proportion of the map is visible */
 		Stuff::Vector2DOf< long > screen;
		Stuff::Vector3D world1, world2;
		screen.x = Environment.screenWidth / 2;
		screen.y = 1;
		eye->screenToGroundPlaneApprox( screen.x, screen.y, world1 );  // cheap: span only needs x,y
		screen.y = Environment.screenHeight - 1;
		eye->screenToGroundPlaneApprox( screen.x, screen.y, world2 );
		float dx = world2.x - world1.x;
		float dy = world2.y - world1.y;
		float span = sqrt(dx * dx + dy * dy);
		/* make the size of the scroll bar proportional to the proportion of the map that's visible */
		SCROLLINFO si;
		GetScrollInfo(SB_VERT, &si, SIF_PAGE);
		si.nPage = VSCROLLBAR_RANGE * span / ( 2.0 * bound);
		si.fMask = SIF_PAGE;
		SetScrollInfo(SB_VERT, &si);
	}

	tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
}

void EditorInterface::SetBusyMode(bool)
{
	/*
		Editor migration hard-disable:

		Busy-mode UI is called during early SDL/GameOS editor startup before
		the legacy MFC frame/tacMap/cursor state is guaranteed to exist.

		Do not touch any EditorInterface members here. Even m_AppIsBusy can be
		unsafe if this is reached through a partially constructed instance.
	*/
	return;
}


void EditorInterface::UnsetBusyMode()
{
	/*
		Editor migration hard-disable:

		Matches SetBusyMode. This must remain a pure no-op until the editor
		startup order is fully restored for the Remastered runtime.
	*/
	return;
}


int EditorInterface::Damage( bool bDamage )
{
	if ( selecting && EditorObjectMgr::instance()->hasSelection() )
	{
		DamageBrush tmp( bDamage );
		Action* pRetAction = tmp.applyToSelection( );
		if ( pRetAction )
			undoMgr.AddAction( pRetAction );
	}
	else
	{
		DamageBrush *pCurDamageBrush = dynamic_cast<DamageBrush *>(curBrush);
		if ((0 == pCurDamageBrush) || (pCurDamageBrush->damage != bDamage))
		{
			// Toolbar entry is the base ID_OTHER_DAMAGE; brushId keeps the +bDamage
			// variant so range-tests are unchanged, menuId is the toolbar base so the
			// highlight matches for both damage and repair variants.
			setActiveBrush( new DamageBrush( bDamage ), ID_OTHER_DAMAGE + bDamage, ID_OTHER_DAMAGE );
			ChangeCursor( IDC_HAMMER + !bDamage );
		}
	}

	return true;
}

int EditorInterface::LayMines()
{
	if ( selecting && ( land->hasSelection() ) )
	{
		MineBrush	brush;
		Action* pRetAction = brush.applyToSelection( );
		if ( pRetAction )
			undoMgr.AddAction( pRetAction );
	}
	else if (0 == dynamic_cast<MineBrush *>(curBrush))
	{
		setActiveBrush( new MineBrush(), ID_OTHER_LAYMINES, ID_OTHER_LAYMINES );
		ChangeCursor( IDC_HAMMER ); // need a minebrush cursor
	}
	return true;
}

int EditorInterface::SelectDetailTexture()
{
	CFileDialog fileDlg( TRUE,  "tga", NULL, OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = texturePath;

	if (  IDOK == fileDlg.DoModal() )
	{
		CString path = fileDlg.m_ofn.lpstrFile;
		path.MakeLower();
		if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
		{
			land->terrainTextures2->resetDetailTexture(path.GetBuffer(0));
			EditorData::instance->DetailTextureNeedsSaving(true);
		}
	}
	return true;
}

int EditorInterface::SelectWaterTexture()
{
	CFileDialog fileDlg( TRUE,  "tga", NULL, OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = texturePath;

	if (  IDOK == fileDlg.DoModal() )
	{
		CString path = fileDlg.m_ofn.lpstrFile;
		path.MakeLower();
		if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
		{
			land->terrainTextures2->resetWaterTexture(path.GetBuffer(0));
			EditorData::instance->WaterTextureNeedsSaving(true);
		}
	}
	return true;
}

//---------------------------------------------------------------------------
inline bool colorMapIsOKFormat (const char *fileName)
{
	DWORD localColorMapSizeCheck = land->realVerticesMapSide * 12.8;

	File tgaFile;
	long result = tgaFile.open(fileName);
	if (result == NO_ERR)
	{
		struct TGAFileHeader tgaHeader;
		tgaFile.read((MemoryPtr)&tgaHeader,sizeof(TGAFileHeader));
		if ((tgaHeader.image_type == UNC_TRUE) &&
			(tgaHeader.width == tgaHeader.height) &&
			(tgaHeader.width == localColorMapSizeCheck))
			return true;

		tgaFile.close();
	}

	return false;
}

int EditorInterface::SetBaseTexture()
{
	CFileDialog fileDlg( TRUE,  "tga", NULL, OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = texturePath;

	if (  IDOK == fileDlg.DoModal() )
	{
		CString path = fileDlg.m_ofn.lpstrFile;
		path.MakeLower();

		//Check that this is a valid colormap of EXACTLY the same size!!!
		if (colorMapIsOKFormat(path))
		{
			char name[1024];
			_splitpath(path,NULL,NULL,name,NULL);

			char *testLoc = strstr(name,".burnin");
			if (testLoc)
				testLoc[0] = 0;	//Prune off the burnin name.

			land->setColorMapName(name);

			if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
			{
				if (land->colorMapName)
					land->terrainTextures2->resetBaseTexture(land->colorMapName);
				else
					land->terrainTextures2->resetBaseTexture(land->terrainName);
			}
		}
		else
		{
			EMessageBox(IDS_INVALID_COLOR_MAP,IDS_ERROR,MB_OK);
		}
	}

	return true;
}

int EditorInterface::ReloadBaseTexture()
{
	if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
	{
		if (land->colorMapName)
			land->terrainTextures2->resetBaseTexture(land->colorMapName);
		else
			land->terrainTextures2->resetBaseTexture(land->terrainName);
	}

	return true;
}

int EditorInterface::SelectWaterDetailTexture()
{
	CFileDialog fileDlg( TRUE,  "tga", NULL, OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = texturePath;

	if (  IDOK == fileDlg.DoModal() )
	{
		CString path = fileDlg.m_ofn.lpstrFile;
		path.MakeLower();
		if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
		{
			land->terrainTextures2->resetWaterDetailTextures(path.GetBuffer(0));
			EditorData::instance->WaterDetailTextureNeedsSaving(true);
		}
	}
	return true;
}

int EditorInterface::TextureTilingFactors()
{
	TilingFactorsDialog tilingFactorsDlg;
	tilingFactorsDlg.m_TerrainDetailTilingFactor = land->terrainTextures2->getDetailTilingFactor();
	tilingFactorsDlg.m_WaterTilingFactor = land->terrainTextures2->getWaterTextureTilingFactor();
	tilingFactorsDlg.m_WaterDetailTilingFactor = land->terrainTextures2->getWaterDetailTilingFactor();
	if (IDOK == tilingFactorsDlg.DoModal())
	{
		land->terrainTextures2->setDetailTilingFactor(tilingFactorsDlg.m_TerrainDetailTilingFactor);
		land->terrainTextures2->setWaterTextureTilingFactor(tilingFactorsDlg.m_WaterTilingFactor);
		land->terrainTextures2->setWaterDetailTilingFactor(tilingFactorsDlg.m_WaterDetailTilingFactor);
	}
	return true;
}

int EditorInterface::Link( bool bLink )
{
	LinkBrush *pCurLinkBrush = dynamic_cast<LinkBrush *>(curBrush);
	if ((0 == pCurLinkBrush) || (pCurLinkBrush->bLink != bLink))
	{
		// Toolbar entry is the base ID_OTHER_LINK; brushId keeps the +bLink variant.
		setActiveBrush( new LinkBrush( bLink ), ID_OTHER_LINK + bLink, ID_OTHER_LINK );
		ChangeCursor( IDC_LINK + !bLink );
	}

	return 1;
}

int EditorInterface::DropZone( bool bVTol )
{
	int alignment = EDITOR_TEAM1;
	
	// you'll need this for multiplayer
	/*for ( int j = 0; j < 9; ++j )
	{
		if ( GetParent()->GetMenu()->GetMenuState( ID_ALIGNMENT_TEAM1 + j, MF_BYCOMMAND ) & MF_CHECKED )
		{
			if (j != 8)
				alignment = EDITOR_TEAM1 + j;
			else
				alignment = EDITOR_TEAMNONE;
		}
	}*/
	
	// No toolbar entry -> menuId = brushId. Highlight never matches it.
	setActiveBrush( new DropZoneBrush( alignment, bVTol ), ID_DROPZONES_ADD + bVTol, ID_DROPZONES_ADD + bVTol );
	ChangeCursor( IDC_DROPZONE );

	return true;

}

int EditorInterface::CampaignEditor()
{
	CCampaignDialog campaignDialog;
	campaignDialog.DoModal();
	return 1;
}

int EditorInterface::OnCreate(LPCREATESTRUCT lpCreateStruct) 
{
	if (CWnd ::OnCreate(lpCreateStruct) == -1)
		return -1;	

	if (!tacMap.m_hWnd)
	{
		tacMap.Create( IDD_TACMAP, this );
		tacMap.ShowWindow( true );
		tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
	}
	else
	{
		gosASSERT(false);
	}

	// FREEZE FIX (catch-all): drive a render tick from a WM_TIMER so the 3D view
	// keeps updating even when the normal OnIdle/WM_PAINT pump is starved AND the
	// mouse isn't moving — e.g. wheel-zoom, edge-scroll with a still cursor, or an
	// MFC modal scrollbar-drag loop (WM_TIMER is still dispatched in those loops).
	// ~120 Hz. SafeRunGameOSLogic self-guards reentrancy and renderer-not-ready.
	SetTimer(kRenderTimerId, 8, NULL);

	return 0;
}

void EditorInterface::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == kRenderTimerId)
	{
		SafeRunGameOSLogic();
		return;
	}
	CWnd::OnTimer(nIDEvent);
}

extern bool gActive;
extern bool gGotFocus;
extern Editor* editor;

// by Methuselas: the embedded GL child forwards mouse messages back here, but
// click-select and double-click settings must be resolved by the MFC Editor
// shell.  Drag-select already works through the brush path; these small
// click-state helpers only rescue non-drag left clicks so the remastered child
// window does not leave single-click and double-click dependent on stale terrain
// inverse-pick behavior.
static int s_editorLeftDownX = 0;
static int s_editorLeftDownY = 0;
static bool s_editorLeftButtonTracking = false;
static DWORD s_editorLastObjectClickTime = 0;
static int s_editorLastObjectClickX = 0;
static int s_editorLastObjectClickY = 0;

static bool EditorInterface_IsClickDistance(int x1, int y1, int x2, int y2)
{
	int cx = ::GetSystemMetrics(SM_CXDRAG);
	int cy = ::GetSystemMetrics(SM_CYDRAG);
	if (cx < 4) cx = 4;
	if (cy < 4) cy = 4;

	return abs(x2 - x1) <= cx && abs(y2 - y1) <= cy;
}

static bool EditorInterface_IsDoubleClickDistance(int x1, int y1, int x2, int y2)
{
	int cx = ::GetSystemMetrics(SM_CXDOUBLECLK);
	int cy = ::GetSystemMetrics(SM_CYDOUBLECLK);
	if (cx < 4) cx = 4;
	if (cy < 4) cy = 4;

	return abs(x2 - x1) <= cx && abs(y2 - y1) <= cy;
}

// EDITOR-OBJECTID-PICK-BRIDGE-1 (Slice 3): GPU ObjectID pick is the PRIMARY
// editor object-selection path; the legacy CPU forward-projection picker is the
// documented killswitch fallback. Default ON; set MC2_EDITOR_GPU_PICK=0 to fall
// back to CPU forward-project (e.g. if the GPU readback regresses on a vendor).
// Convention matches GameOS/gameos/gos_mech_batcher.cpp envFlagDefaultOn:
// unset -> on, exactly "0" -> off, any other value -> on.
static bool EditorInterface_GpuPickEnabled()
{
	static const bool s_enabled = []() {
		const char* v = getenv("MC2_EDITOR_GPU_PICK");
		if (v == nullptr) return true;                 // unset -> on (new default)
		if (v[0] == '0' && v[1] == '\0') return false; // exactly "0" -> off
		return true;                                   // any other value -> on
	}();
	return s_enabled;
}

// CPU forward-projection picker. Kept as the killswitch fallback for the GPU
// path and as the press-time drag-grab picker (OnLButtonDown).
static EditorObject* EditorInterface_PickObjectAtScreenPoint_CPU(int screenX, int screenY)
{
	if (!EditorObjectMgr::instance())
		return NULL;

	return EditorObjectMgr::instance()->getObjectAtScreenPosition(screenX, screenY);
}

static EditorObject* EditorInterface_PickObjectAtScreenPoint(int screenX, int screenY)
{
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if (!mgr)
		return NULL;

	// ---- PRIMARY: GPU ObjectID readback via the single EditorBridge seam ----
	// EditorBridge::pickAt() owns the screen->FBO coord transform + glReadPixels
	// + RenderWorld handle lookup. It returns Kind::StaticProp / Kind::Mech on an
	// object hit, Kind::Terrain on a ground click, Kind::Miss otherwise. We only
	// consume StaticProp / Mech here; Terrain and Miss fall through to the CPU
	// forward-projection picker so a ground click still resolves the nearest
	// object exactly as before (CPU behaviour is unchanged on a terrain hit).
	if (EditorInterface_GpuPickEnabled() && EditorBridge::isEnabled())
	{
		EditorBridge::EditorPickResult pick = EditorBridge::pickAt(screenX, screenY);

		if (pick.kind == EditorBridge::EditorPickResult::Kind::StaticProp)
		{
			// Reverse the GPU handle back to the EditorObject. handle.index()
			// equals the GpuStaticPropRegistry recipeIndex (identity mapping,
			// RenderWorld.h handleToRecipeIndex); the owning editor object's
			// appearance carries the same recipeIndex.
			const int32_t recipeIndex = static_cast<int32_t>(pick.handle.index());
			if (EditorObject* obj = mgr->findObjectByStaticRecipeIndex(recipeIndex))
			{
				// EDITOR-PICK-DUP-OID: GPU ObjectID is per-type not per-instance --
				// the GpuStaticPropRegistry's typeID->recipeIndex map keeps only the
				// LAST recipe registered for a typeID, so N same-type buildings all
				// resolve to the most-recently-placed instance. The other N-1 render
				// fine but the GPU readback hands back the wrong (duplicate) object,
				// which is elsewhere on the map -> they become un-pickable.
				//
				// Cross-check against the CPU per-instance forward-projection picker,
				// which projects every object with the SAME projection + pixel
				// tolerance (EditorObjectMgr_ProjectScreenXY_GL inside
				// getObjectAtScreenPosition) and returns the nearest one actually
				// under the cursor. If the CPU picker agrees the GPU object is at the
				// click (or there is no CPU per-instance picker available), accept the
				// GPU hit unchanged (unique-type case). Otherwise the GPU resolved a
				// same-type duplicate that does NOT project near the cursor -- discard
				// it and trust the per-instance CPU result (which may be null).
				//
				// Scoped to StaticProp hits only: the per-type OID collision is a
				// static-prop registry artifact. Mech/unit hits (Kind::Mech below)
				// carry stable per-instance handle bits and are NOT cross-checked.
				EditorObject* cpuObj = mgr->getObjectAtScreenPosition(screenX, screenY);
				if (cpuObj == NULL || cpuObj == obj)
					return obj;            // unique-type / GPU object is under cursor
				return cpuObj;             // same-type duplicate -> CPU instance wins
			}
			// GPU hit a static prop the editor has no EditorObject for (e.g.
			// terrain-baked vegetation forests with no per-instance editor object).
			// Fall through to CPU rather than returning a miss.
		}
		else if (pick.kind == EditorBridge::EditorPickResult::Kind::Mech)
		{
			// Best-effort: editor scenes rarely place live mechs, but if one is
			// present its Unit carries the RenderWorld handle (findUnitByMechHandle).
			// NOTE: mech gameObjectId is still 0 (MechRenderAdapter), but the
			// handle bits ARE the stable identity here, so this resolves cleanly.
			if (EditorObject* unit = mgr->findUnitByMechHandle(pick.handle))
				return unit;
			// Unresolved mech -> CPU fallback (documented best-effort).
		}
		// Kind::Terrain or Kind::Miss -> CPU forward-projection (ground click /
		// background): preserves the legacy "nearest projected object" behaviour.
	}

	// ---- FALLBACK: CPU forward-projection (killswitch + terrain/miss path) ----
	return EditorInterface_PickObjectAtScreenPoint_CPU(screenX, screenY);
}

// EDITOR-BRUSH-SCREENPICK-1: public seam so the brushes (eraser/damage/link) pick the
// SAME way the Select tool does -- GPU ObjectID readback primary (pixel-exact: handles a
// zoomed-in click on a prop's edge, which the CPU center+tolerance picker misses), with
// CPU forward-projection fallback. screenX/screenY must be viewport-local (the brush
// dispatch already applies EditorRttClientToViewport before paint, like the Select path).
EditorObject* EditorPickObjectAtScreen(int screenX, int screenY)
{
	return EditorInterface_PickObjectAtScreenPoint(screenX, screenY);
}

static bool EditorInterface_SelectObjectAtScreenPoint(int screenX, int screenY, bool toggle)
{
	EditorObject* pObject = EditorInterface_PickObjectAtScreenPoint(screenX, screenY);
	if (!pObject || !EditorObjectMgr::instance())
		return false;

	if (!toggle)
	{
		if (land)
			land->unselectAll();
		EditorObjectMgr::instance()->unselectAll();
		EditorObjectMgr::instance()->select(*pObject, true);
		return true;
	}

	EditorObjectMgr::instance()->select(*pObject, !pObject->isSelected());
	return true;
}

static bool EditorInterface_ShouldOpenSettingsForClick(int screenX, int screenY)
{
	DWORD now = timeGetTime();
	DWORD doubleClickTime = ::GetDoubleClickTime();
	if (doubleClickTime == 0)
		doubleClickTime = 500;

	bool result = false;
	if (s_editorLastObjectClickTime != 0 &&
		now - s_editorLastObjectClickTime <= doubleClickTime &&
		EditorInterface_IsDoubleClickDistance(s_editorLastObjectClickX, s_editorLastObjectClickY, screenX, screenY))
	{
		result = true;
		s_editorLastObjectClickTime = 0;
	}
	else
	{
		s_editorLastObjectClickTime = now;
		s_editorLastObjectClickX = screenX;
		s_editorLastObjectClickY = screenY;
	}

	return result;
}


#ifdef MC2_IMGUI
// Minimal Win32 virtual-key -> ImGuiKey map for the keys an ImGui text field needs
// (editing/navigation + the letters/digits used in Ctrl+ shortcuts). Printable text
// itself arrives via WM_CHAR -> AddInputCharacter, not through this map.
static ImGuiKey EditorInterface_VkToImGuiKey(int vk)
{
	switch (vk)
	{
	case VK_TAB:        return ImGuiKey_Tab;
	case VK_LEFT:       return ImGuiKey_LeftArrow;
	case VK_RIGHT:      return ImGuiKey_RightArrow;
	case VK_UP:         return ImGuiKey_UpArrow;
	case VK_DOWN:       return ImGuiKey_DownArrow;
	case VK_PRIOR:      return ImGuiKey_PageUp;
	case VK_NEXT:       return ImGuiKey_PageDown;
	case VK_HOME:       return ImGuiKey_Home;
	case VK_END:        return ImGuiKey_End;
	case VK_INSERT:     return ImGuiKey_Insert;
	case VK_DELETE:     return ImGuiKey_Delete;
	case VK_BACK:       return ImGuiKey_Backspace;
	case VK_SPACE:      return ImGuiKey_Space;
	case VK_RETURN:     return ImGuiKey_Enter;
	case VK_ESCAPE:     return ImGuiKey_Escape;
	case VK_CONTROL:    return ImGuiKey_LeftCtrl;
	case VK_SHIFT:      return ImGuiKey_LeftShift;
	case VK_MENU:       return ImGuiKey_LeftAlt;
	case VK_OEM_MINUS:  return ImGuiKey_Minus;
	case VK_OEM_PERIOD: return ImGuiKey_Period;
	default:
		if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
		if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
		return ImGuiKey_None;
	}
}
#endif

LRESULT EditorInterface::WindowProc(UINT message, WPARAM wParam, LPARAM lParam)
{
	//We're not ready to draw anything.
	// If you let an event through it'll probably crash!
	//
#ifdef MC2_IMGUI
	// Route the keyboard to ImGui ONLY while a text field is active (WantTextInput),
	// and CONSUME the message so editor single-key shortcuts (handleKeyDown) and the
	// GameOS input path do not also fire while the user is typing in a search/text
	// box. When no field is focused, WantTextInput is false and keys flow normally.
	if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput)
	{
		ImGuiIO& io = ImGui::GetIO();
		switch (message)
		{
		case WM_CHAR:
			if (wParam > 0 && wParam < 0x10000)
				io.AddInputCharacterUTF16((unsigned short)wParam);
			return 0;
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYUP:
		{
			const bool down = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
			ImGuiKey key = EditorInterface_VkToImGuiKey((int)wParam);
			if (key != ImGuiKey_None)
				io.AddKeyEvent(key, down);
			return 0;
		}
		default:
			break;
		}
	}
#endif

	if ( message == WM_KEYDOWN )
		handleKeyDown( wParam );

	if ( message == WM_CREATE )
	{
		gActive = true;
		gGotFocus= true;
	}

	if ( WM_MOUSEWHEEL == message)
	{
		//int i = 17;
	}
	
	int retVal  = 0;
	
	retVal = CWnd::WindowProc(message, wParam, lParam);

	if ( message == WM_KEYDOWN && wParam == VK_SCROLL )
		return 0;

	if ( (eye || message == WM_MOVE) )
	{
		switch (message)
		{
		case WM_SYSCOLORCHANGE:
		case WM_DISPLAYCHANGE:
		case WM_SETCURSOR:
		case WM_ACTIVATEAPP:
		case WM_KILLFOCUS:
		case WM_SETFOCUS:
		case WM_MOVE:
		case WM_ERASEBKGND:
		case 0x20b:
		case 0x20c:
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_CHAR:
		case WM_CLOSE:
			retVal = GameOSWinProc( m_hWnd,message,wParam,lParam );
			break;

		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
			{
				retVal = GameOSWinProc( m_hWnd,message,wParam,lParam );
			}
			break;

		case WM_SIZE:
			{
				float w = LOWORD(lParam);
				float h = HIWORD(lParam);
				windowSizeChanged = true;
				g_newWidth = w + 16/*scrollbar thickness*/;
				g_newHeight = h + 16/*scrollbar thickness*/;
				retVal = GameOSWinProc( m_hWnd,message,wParam,lParam );
				if (editor && bThisIsInitialized)
				{
					editor->update();
				}
				//EditorObjectMgr::instance()->update();
			}
			break;
		}
	}		
	
	return retVal;
	
}

afx_msg void EditorInterface::OnCommand(UINT nID) 
{
	EditorInterface::instance()->handleNewMenuMessage( nID );
}

void EditorInterface::UpdateButton( CCmdUI* pButton )
{
	pButton->Enable( true );

	if ( pButton->m_nID == ID_DROPZONES_ADD || pButton->m_nID == ID_DROPZONES_VTOL )
	{
		Stuff::Vector3D pos;
		pos.x = -1; 
		pos.y = -1;
		if ( !EditorObjectMgr::instance()->canAddDropZone( pos, 0, (pButton->m_nID - ID_DROPZONES_ADD) ? true : false ) )
			pButton->Enable( false );
	}
}


void EditorInterface::OnLButtonDown(UINT nFlags, CPoint point)
{
	// by Methuselas: MFC owns mouse capture and tool dispatch for the Editor.
	// Keep this path free of render-child input shims so the embedded GL HWND
	// stays a passive rendering surface, not a second input owner.
	SetCapture();

	// by Methuselas: remember the press point separately from brush state.
	// The selection brush owns drag-select; EditorInterface owns deciding
	// whether the completed press/release was a true click.
	s_editorLeftDownX = point.x;
	s_editorLeftDownY = point.y;
	s_editorLeftButtonTracking = true;

#ifdef MC2_IMGUI
	// Inject button-down into ImGui.  For normal GL-child clicks this is a
	// harmless duplicate of what ForwardMouseToEditor already queued; ImGui
	// deduplicates same-state events.  When MFC has capture from a prior
	// drag and this WM_LBUTTONDOWN bypassed the GL child entirely, this is
	// the ONLY injection, ensuring ImGui's state stays coherent.
	if (g_imguiInitialized)
		ImGui::GetIO().AddMouseButtonEvent(0, true);
#endif

	handleLeftButtonDown( point.x, point.y );
	SPEW( (0, "GotClick"));
}

void EditorInterface::OnLButtonUp(UINT nFlags, CPoint point)
{
	bool wasClick = s_editorLeftButtonTracking &&
		EditorInterface_IsClickDistance(s_editorLeftDownX, s_editorLeftDownY, point.x, point.y);

#ifdef MC2_IMGUI
	// Click-state trace: fires on every LButtonUp so we can see why the
	// Ctrl+Shift+LMB inspector pick is/isn't triggering.  Remove once pick works.
	{
		static int s_clickTrace = 0;
		if (++s_clickTrace <= 5) {  // cap at 5 to avoid log spam
			const int ctrl  = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
			const int shift = (::GetAsyncKeyState(VK_SHIFT)   & 0x8000) ? 1 : 0;
			std::fprintf(stderr,
				"[EDITOR_CLICK #%d] xy=(%d,%d) wasClick=%d tracking=%d ctrl=%d shift=%d imgui=%d\n",
				s_clickTrace, point.x, point.y,
				wasClick ? 1 : 0, s_editorLeftButtonTracking ? 1 : 0,
				ctrl, shift, g_imguiInitialized ? 1 : 0);
		}
	}
#endif

	handleLeftButtonUp( point.x, point.y );
	ReleaseCapture();

#ifdef MC2_IMGUI
	// Inject button-up into ImGui.  SetCapture() in OnLButtonDown routes all
	// WM_LBUTTONUP messages directly to MFC, bypassing EditorGLChildWndProc
	// and ForwardMouseToEditor.  Without this injection, ImGui's io.MouseDown[0]
	// stays true permanently after the first viewport click -- every subsequent
	// click is seen as "button already held", IsMouseClicked(0) never returns
	// true, and no widget ever activates.
	if (g_imguiInitialized)
		ImGui::GetIO().AddMouseButtonEvent(0, false);
#endif

	if (wasClick && currentBrushID == IDS_SELECT)
	{
		// Shift = add/remove (multi-select). Ctrl+click and plain click both do a
		// plain replace-select so a single click always selects the clicked object.
		bool toggle = (GetAsyncKeyState(VK_SHIFT) != 0);

		// by Methuselas: drag-select is still brush-owned.  This direct
		// click-pick path runs only after a true non-drag click and uses the
		// remastered screen-space object picker so single-click selection does
		// not depend on the old terrain inverse-project path.
		// RTT: the picker unprojects, so feed it viewport-local coords.
		int selX = (int)point.x, selY = (int)point.y;
		EditorRttClientToViewport( selX, selY );
		if (EditorInterface_SelectObjectAtScreenPoint(selX, selY, toggle))
		{
			if (EditorInterface_ShouldOpenSettingsForClick(selX, selY))
				UnitSettings();

			tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
			SafeRunGameOSLogic();
		}
	}

	s_editorLeftButtonTracking = false;

#ifdef MC2_IMGUI
	// Ctrl+Shift+LMB: run the object inspector pick.
	// ForwardMouseToEditor already consumed any click landing on an ImGui
	// window (never forwarded to MFC), so any click reaching here is a
	// confirmed viewport click -- no additional ImGui hover check needed.
	if (wasClick && g_imguiInitialized
		&& (::GetAsyncKeyState(VK_CONTROL) & 0x8000)
		&& (::GetAsyncKeyState(VK_SHIFT)   & 0x8000))
	{
		// Use tryGameplayPick for the full substrate-backed pick path
		// (coord transform + glReadPixels + handle lookup).
		// gameplay_pick.cpp has no game-object deps so it compiles cleanly
		// in the editor's link environment (gameos + renderworld + gui_runtime).
		// Convert MFC client-pixel coords to the viewport-canvas space that
		// tryGameplayPick expects.  gos_GetViewport returns vMulX/vMulY in
		// FBO-pixel units (= drawableWidth/Height for a full-screen viewport).
		// The MFC view is sized to the SDL child, but drawableWidth was set at
		// SDL window creation and may differ if the view was ever resized.
		// Normalising through the window width handles both the match and the
		// mismatch case: editorMouseX = point.x * vMulX / windowWidth.
		float vmx = 1.f, vmy = 1.f, vax = 0.f, vay = 0.f;
		gos_GetViewport(&vmx, &vmy, &vax, &vay);
		// RTT: the scene renders in the central-node sub-rect. Normalise through the
		// viewport rect (offset + dims), not the full window — else the click maps
		// full-window width onto the shrunk scene and diverges (the "2x" bug). When
		// RTT is off the rect is the full window, so this reduces to the old math.
		int pickPx = (int)point.x, pickPy = (int)point.y;
		EditorRttClientToViewport( pickPx, pickPy );
		int normW = 0, normH = 0;
		if ( g_imguiInitialized && GuiRuntime::RttEnabled()
			&& GuiRuntime::ViewportRectW() > 0 && GuiRuntime::ViewportRectH() > 0 )
		{
			normW = GuiRuntime::ViewportRectW();
			normH = GuiRuntime::ViewportRectH();
		}
		else
		{
			CRect clientRect;
			GetClientRect(&clientRect);
			normW = clientRect.Width()  > 0 ? clientRect.Width()  : 1;
			normH = clientRect.Height() > 0 ? clientRect.Height() : 1;
		}
		const int winW = normW, winH = normH;

		GameplayPickRequest req{};
		req.mouseX               = (int)((float)pickPx / winW * vmx);
		req.mouseY               = (int)((float)pickPy / winH * vmy);
		req.shiftDn              = true;
		req.leftClicked          = true;
		req.bGui                 = false;   // no HUD region in editor
		req.bLeftDouble          = false;
		req.moverSelectedThisFrame = false;

		// Guarantee the WGL context is current before glReadPixels.
		// SDL_PollEvent (inside RunGameOSLogic) fires before make_current_context;
		// MFC pump can deliver WM_LBUTTONUP in that window, leaving no context bound.
		graphics::make_current_context(EditorGameOS_GetRenderContext());

		GameplayPickResult result = tryGameplayPick(req);

		// Diagnostic: fires on every pick until a hit is seen.
		// Scans a coarse grid of the OID attachment to answer: "is ANYTHING
		// written, or is the whole buffer zero?"  Remove once pick works.
		static bool s_pickDiagDone = false;
		if (!s_pickDiagDone) {
			if (result.outcome == GameplayPickResult::Outcome::hit)
				s_pickDiagDone = true;  // stop once we see a real hit

			const char* outcomeStr[] = { "skipped", "gated", "miss", "hit" };
			const int   oi = (int)result.outcome;
			std::fprintf(stderr,
				"[EDITOR_PICK diag] outcome=%s winXY=(%d,%d) winWH=(%d,%d) "
				"vMulXY=(%.0f,%.0f) reqXY=(%d,%d) "
				"fboXY=(%d,%d) glXY=(%d,%d) dWH=(%d,%d) "
				"raw=0x%08X reason=%s\n",
				(oi >= 0 && oi <= 3) ? outcomeStr[oi] : "?",
				point.x, point.y, winW, winH,
				vmx, vmy, req.mouseX, req.mouseY,
				result.ctx.fboX, result.ctx.fboY,
				result.ctx.glX,  result.ctx.glY,
				result.ctx.drawableWidth, result.ctx.drawableHeight,
				result.lookup.rawObjectId,
				result.lookup.lookupFailReason ? result.lookup.lookupFailReason : "(none)");

			// Scan the OID attachment at a 32x32 grid across the whole FBO.
			// Reports: whether the FBO / tex exist, and first nonzero pixel found.
			// Answers definitively: "is the draw path writing object IDs at all?"
			gosPostProcess* pp_scan = getGosPostProcess();
			const GLuint scanFBO = pp_scan ? pp_scan->getSceneFBO()        : 0u;
			const GLuint scanTex = pp_scan ? pp_scan->getSceneObjectIdTex(): 0u;
			std::fprintf(stderr,
				"[EDITOR_PICK scan] pp=%p sceneFBO=%u oidTex=%u\n",
				(void*)pp_scan, scanFBO, scanTex);

			if (scanFBO && scanTex) {
				const int dw = result.ctx.drawableWidth;
				const int dh = result.ctx.drawableHeight;
				if (dw > 0 && dh > 0) {
					GLint prevReadFbo = 0;
					glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
					glBindFramebuffer(GL_READ_FRAMEBUFFER, scanFBO);
					glReadBuffer(GL_COLOR_ATTACHMENT2);

					// Sample a 32x32 grid
					const int steps = 32;
					uint32_t firstNonzeroVal = 0u;
					int firstNonzeroX = -1, firstNonzeroY = -1;
					int totalNonzero = 0;
					for (int gy = 0; gy < steps && firstNonzeroX < 0; ++gy) {
						for (int gx = 0; gx < steps; ++gx) {
							const int px = gx * (dw - 1) / (steps - 1);
							const int py = gy * (dh - 1) / (steps - 1);
							uint32_t pval = 0u;
							glReadPixels(px, py, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &pval);
							if (pval != 0u) {
								if (firstNonzeroX < 0) {
									firstNonzeroX = px;
									firstNonzeroY = py;
									firstNonzeroVal = pval;
								}
								++totalNonzero;
							}
						}
					}

					glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFbo));

					if (firstNonzeroX >= 0) {
						std::fprintf(stderr,
							"[EDITOR_PICK scan] OID attachment HAS data: "
							"first nonzero at GL(%d,%d)=0x%08X totalNonzero=%d/%d\n",
							firstNonzeroX, firstNonzeroY, firstNonzeroVal,
							totalNonzero, steps * steps);
					} else {
						std::fprintf(stderr,
							"[EDITOR_PICK scan] OID attachment ALL ZERO across %dx%d grid "
							"(dWH=%dx%d) — draw path not writing object IDs\n",
							steps, steps, dw, dh);
					}
				}
			}
		}

		if (result.outcome == GameplayPickResult::Outcome::hit ||
			result.outcome == GameplayPickResult::Outcome::miss)
		{
			// Pick ran: populate inspector with full lookup data.
			EditorInspector::setPickResult(point.x, point.y, result.lookup);
		}
		else
		{
			// Substrate off or out-of-bounds: open inspector at screen coords
			// so the user sees the window even without object ID data.
			EditorInspector::onCtrlShiftClick(point.x, point.y);
		}
	}
#endif
}

void EditorInterface::OnMouseMove(UINT nFlags, CPoint point)
{
	// by Methuselas: right-drag camera rotation still lives in handleMouseMove().
	// EditorGosRender forwards GL-child mouse messages here so this legacy
	// camera/tool path remains the single source of truth.
	handleMouseMove( point.x, point.y );

	// PERF/FREEZE FIX: the 3D viewport normally renders via OnIdle -> InvalidateRect
	// -> WM_PAINT. A continuous mouse drag (pan / edge-scroll / wheel-zoom with
	// motion) floods the queue with WM_MOUSEMOVE, which starves BOTH OnIdle (only
	// runs on an empty queue) and WM_PAINT (lowest priority). Result: the view
	// freezes for the whole duration of the drag (measured 7-19s gaps between
	// RunGameOSLogic ticks) while tacmap/coords stay live (handled here directly).
	// Pump a render frame from the flood handler itself, throttled to ~120 fps so
	// high-rate mice don't over-render. SafeRunGameOSLogic self-guards reentrancy.
	{
		static DWORD s_lastRenderTick = 0;
		DWORD nowTick = ::GetTickCount();
		if (nowTick - s_lastRenderTick >= 8)
		{
			s_lastRenderTick = nowTick;
			SafeRunGameOSLogic();
		}
	}
}

void EditorInterface::ChangeCursor( int ID )
{
	if ( hCursor )
		DestroyCursor( hCursor );

	hCursor = AfxGetApp()->LoadCursor( ID );

	if (0 >= m_AppIsBusy)
	{
		::SetCursor( hCursor );
	}
	
	curCursorID = ID;
	
}

BOOL EditorInterface::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message) 
{
	if (0 < m_AppIsBusy)
	{
		if (m_hBusyCursor) {
			::SetCursor(m_hBusyCursor);
		} else {
			::SetCursor(LoadCursor(NULL, IDC_WAIT));
		}
	}
	else if ( hCursor )
	{
		::SetCursor( hCursor );
		return true;
	}

	return CWnd::OnSetCursor( pWnd, nHitTest, message );

}

BOOL EditorInterface::PreCreateWindow(CREATESTRUCT& cs) 
{
	
      cs.lpszClass = AfxRegisterWndClass(
            CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW | CS_OWNDC, // CS_OWNDC required for OpenGL pixel format
            NULL,
            (HBRUSH) (COLOR_WINDOW + 1));         
	
	return CWnd ::PreCreateWindow(cs);
}

void EditorInterface::OnKeyUp(UINT nChar, UINT nRepCnt, UINT nFlags) 
{
	if ( nChar == KEY_SPACE )
	{
		KillCurBrush();
		curBrush = prevBrush;
		selecting = prevSelecting;
		painting = prevPainting;
		dragging = prevDragging;
		prevBrush = NULL;
		prevSelecting = false;
		prevPainting = false;
		prevDragging = false;
		ChangeCursor( oldCursor );
		syncScrollBars();
		//dragging = false;		
	}

	lastKey = -1;
	
	CWnd ::OnKeyUp(nChar, nRepCnt, nFlags);
}

void EditorInterface::OnRButtonDown(UINT nFlags, CPoint point)
{
	// by Methuselas: right-button capture must stay on the MFC EditorInterface,
	// not the embedded GL child.  The child forwards the initial down message;
	// this handler owns the drag state that drives camera rotate/tilt.
	SetCapture( );
	lastX = point.x;
	lastY = point.y;
	rightDrag = true;
	lastRightClickTime = timeGetTime();

#ifdef MC2_IMGUI
	if (g_imguiInitialized)
		ImGui::GetIO().AddMouseButtonEvent(1, true);
#endif
}

void EditorInterface::OnRButtonUp(UINT nFlags, CPoint point)
{
	rightDrag = false;
	ReleaseCapture();

#ifdef MC2_IMGUI
	// Same SetCapture bypass fix as OnLButtonUp -- inject UP so ImGui's
	// io.MouseDown[1] doesn't get stuck true after right-drag operations.
	if (g_imguiInitialized)
		ImGui::GetIO().AddMouseButtonEvent(1, false);
#endif

	if ( timeGetTime() - lastRightClickTime < 200 )
	{
		Select();
	}
	else
	{
		tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
		syncScrollBars();
	}

	// (Removed: dead commented-out MFC TrackPopupMenu right-click menu —
	// EDITOR-SIMPLIFY S0. The live right-click flow lives in OnRButtonUp/ImGui.)
}

BOOL EditorInterface::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
	// Null-guard the camera: a mouse-wheel message can arrive during editor startup
	// before `eye` (the camera) is created — the camera-zoom path below derefs
	// eye->getScaleFactor()/ZoomIn/Out and READ-violated at 0x310 on a wheel during
	// init. Nothing to rotate or zoom without a camera, so consume and bail. (Same
	// bug class as the EditorObjectMgr::instance() guard further down.)
	if ( !eye )
		return TRUE;

	//--------------------------------------------------
	// Mouse-wheel rotation. When a placement brush is active the wheel rotates
	// the placement cursor; otherwise, if objects are selected, it rotates the
	// selection. Ctrl+wheel always falls through to camera zoom (escape hatch),
	// and Shift gives fine 1-degree steps instead of the default 15.
	if ( zDelta != 0 && !(nFlags & MK_CONTROL) )
	{
		float deg = ( (nFlags & MK_SHIFT) ? 1.0f : 15.0f ) * ( (zDelta > 0) ? 1.0f : -1.0f );
		// Only a single-placement BuildingBrush rotates on the wheel. A ScatterBrush
		// is also in the object-id range but is NOT a BuildingBrush, so cast safely
		// (an unchecked cast here crashed when scattering — it has no rotation).
		if ( BuildingBrush* bb = dynamic_cast<BuildingBrush*>(curBrush) )
		{
			bb->addRotationDegrees( deg );
			tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
			return TRUE;
		}
		// Null-guard the singleton: EditorObjectMgr::instance() is NULL before a map
		// is loaded (or after a failed load). An unguarded deref here READ-violated at
		// 0x100 (this+offset) on a mouse wheel with no/partly-loaded map
		// (syncSelectedObjectPointerList <- getSelectionCount <- here). Fall through to
		// camera zoom when there is no object manager / nothing to rotate.
		else if ( EditorObjectMgr::instance() &&
		          EditorObjectMgr::instance()->getSelectionCount() > 0 )
		{
			rotateSelectedObjectsDegrees( deg );
			tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
			return TRUE;
		}
	}

	//--------------------------------------------------
	// Zoom Camera based on Mouse Wheel input.
	long mouseWheelDelta = zDelta; // 240 is the weird increment that the mouse wheel is in
	if (mouseWheelDelta)
	{
		float actualZoom = zoomInc * abs(mouseWheelDelta) * 0.0001f * eye->getScaleFactor();
		if (mouseWheelDelta < 0)
		{
			eye->ZoomOut(actualZoom);
		}
		else
		{
			eye->ZoomIn(actualZoom);
		}
	}

	//int middleClicked = nFlags & MK_MBUTTON;  
	int middleClicked = (nFlags & MK_MBUTTON) && (nFlags & MK_RBUTTON); // it's too easy to accidentally press the middle button while scrolling on some mice
	//-----------------------------------------------------------------
	// If middle mouse button is pressed, go to normal tilt, 50% zoom	
	if (middleClicked)
	{
		eye->tiltNormal();
		eye->ZoomNormal();
	}	

	//IF there is a selected object, find distance to it from camera.
	float eyeDistance = 0.0f;
	long selectionCount = EditorObjectMgr::instance()->getSelectionCount();
	if (selectionCount)
	{
		EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjectsList = EditorObjectMgr::instance()->getSelectedObjectList();
		EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = selectedObjectsList.Begin();
		const EditorObject* pInfo = (*it);
		// EDITOR-CRASH-HARDENING-1: appearance() can be NULL; guard before ->position
		// (mirror sibling readout ~line 1975).
		const ObjectAppearance* pAppearance = pInfo ? pInfo->appearance() : NULL;
		if ( pAppearance )
		{
			Stuff::Point3D eyePosition(eye->getCameraOrigin());
			Stuff::Point3D objPosition;
			objPosition.x = -pAppearance->position.x;
			objPosition.y = pAppearance->position.z;
			objPosition.z = pAppearance->position.y;

			Stuff::Vector3D Distance;
			Distance.Subtract(objPosition,eyePosition);
			eyeDistance = Distance.GetApproximateLength();
		}
	}

	// need to put this value in the appropriate place.
	char buffer[1024];
	sprintf( buffer, "%.3f", eyeDistance);
	((MainFrame*)AfxGetMainWnd())->m_wndDlgBar.GetDlgItem( IDC_OBJDISTANCEEDIT )->SetWindowText( buffer );

	// Re-render immediately so the zoomed view is visible before the next OnIdle tick.
	SafeRunGameOSLogic();
	tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
	syncScrollBars();
	return CWnd ::OnMouseWheel(nFlags, zDelta, pt);
}

// Highlight the currently-selected terrain area (translucent green) so drag-select
// on the ground gives visible feedback.
void EditorInterface::renderTerrainSelection()
{
	if ( !land || !eye || !land->hasSelection() )
		return;

	const int side = land->realVerticesMapSide;
	const float wupv = land->worldUnitsPerVertex;

	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	// Always-visible overlay: disable depth test/write (rhw=1 z=0 verts otherwise
	// fail the reverse-Z chunk-terrain depth test and vanish). Restored below.
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite,   0 );

	// PERF: window the selection-overlay scan to the visible camera vertex
	// range (same window MapData::makeLists renders). Previously this was an
	// unconditional O(realVerticesMapSide^2) double loop — on a 1k map that is
	// ~1.04M iterations + a gos_DrawQuads per selected cell EVERY frame, which
	// dominated the editor frame (~560ms, the "slow pan" hot loop). Off-screen
	// selected cells don't need drawing. Bounds mirror makeLists (mapdata.cpp).
	int jLo = 0, jHi = side - 1, iLo = 0, iHi = side - 1;
	if ( Terrain::mapData && Terrain::visibleVerticesPerSide > 0 )
	{
		const Stuff::Vector2DOf<float> tlv = Terrain::mapData->getTopLeftVertex();
		const int vvps = (int)Terrain::visibleVerticesPerSide;
		const int topX = (int)tlv.x;
		const int topY = (int)tlv.y;
		iLo = topX < 0 ? 0 : topX;
		jLo = topY < 0 ? 0 : topY;
		iHi = (topX + vvps) < (side - 1) ? (topX + vvps) : (side - 1);
		jHi = (topY + vvps) < (side - 1) ? (topY + vvps) : (side - 1);
	}

	for ( int j = jLo; j < jHi; ++j )
	{
		for ( int i = iLo; i < iHi; ++i )
		{
			if ( !land->isVertexSelected( j, i ) )
				continue;

			Stuff::Vector3D c[4];
			c[0].x = land->mapTopLeft3d.x + (float)i * wupv;       c[0].y = land->mapTopLeft3d.y - (float)j * wupv;
			c[1].x = land->mapTopLeft3d.x + (float)(i + 1) * wupv; c[1].y = c[0].y;
			c[2].x = c[1].x;                                       c[2].y = land->mapTopLeft3d.y - (float)(j + 1) * wupv;
			c[3].x = c[0].x;                                       c[3].y = c[2].y;

			gos_VERTEX q[4];
			memset( q, 0, sizeof( q ) );
			for ( int k = 0; k < 4; ++k )
			{
				c[k].z = land->getTerrainElevation( c[k] );
				Stuff::Vector4D s;
				eye->projectZ( c[k], s );
				q[k].x = s.x; q[k].y = s.y; q[k].rhw = 1.0f; q[k].argb = 0x5000ff00;
			}
			gos_DrawQuads( q, 4 );
		}
	}

	gos_SetRenderState( gos_State_ZCompare, 1 );
	gos_SetRenderState( gos_State_ZWrite,   1 );
}

void EditorInterface::setSculptBrush( int mode )
{
	// Synthetic-negative brushId preserved (not IDS_SELECT, not the object range).
	// No toolbar entry; sculpt highlight is driven separately off curBrush's mode
	// (~:5289), so menuId = brushId is just a stable non-matching value.
	const int brushId = -100 - mode;
	setActiveBrush( new HeightBrush( (HeightBrush::Mode)mode, m_sculptRadius, m_sculptStrength ), brushId, brushId );
}

void EditorInterface::setStampBrush( int type )
{
	// Synthetic-negative brushId preserved. No toolbar entry; menuId = brushId is a
	// stable non-matching value.
	const int brushId = -200 - type;
	setActiveBrush( new StampBrush( (StampBrush::Type)type, m_stampRadius, m_stampStrength ), brushId, brushId );
}

// TERRAIN-MATERIAL-PAINT-1 (Slice 1): revive the dormant editor/TerrainBrush.h in the
// modern Tools panel. TerrainBrush takes only a TerrainType (mclib/dmapdata.h) and is
// single-vertex (no radius/strength) -- Slice 2 adds a radius loop. Install via the same
// single-source setActiveBrush path setSculptBrush/setStampBrush use so the active-tool
// (currentBrushID/currentBrushMenuID) state stays correct. Synthetic-negative brushId
// (distinct from the sculpt -100.. and stamp -200.. ranges).
void EditorInterface::setPaintMaterialBrush( int terrainType )
{
	const int brushId = -300 - terrainType;
	TerrainBrush* tb = new TerrainBrush( terrainType );
	tb->setRadius( m_paintMaterialSize );   // Slice 0: radius-fill (mirrors sculpt)
	setActiveBrush( tb, brushId, brushId );
	m_paintMaterialType = terrainType;   // drives the active-button highlight
}

#ifdef MC2_IMGUI
// Visibility for the Phase 1d wrapper panels (toggled from the Tools palette).
static bool s_placePanelOpen   = false;
static bool s_missionToolsOpen = false;

// Global ImGui UI scale (drives io.FontGlobalScale so every panel scales at once).
// Initialised once from the editor window's DPI so HiDPI displays are readable by
// default; the user can override it from the Tools palette. Persists for the run.
static float s_uiScale     = 1.0f;
static bool  s_uiScaleInit = false;

// Floating tool palette (Photoshop-style). Buttons re-post the same WM_COMMAND
// the menu items use, so they share the existing tool-switch handlers. The active
// tool is highlighted. Drawn each frame from EditorGameOS.cpp's ImGui block.
void EditorInterface::renderToolbarImGui()
{
	struct ToolDef { const char* label; int cmdId; };
	static const ToolDef tools[] = {
		{ "Select",      ID_OTHER_SELECT },
		{ "Flatten (F)", ID_OTHER_FLATTEN },
		{ "Erase (E)",   ID_OTHER_ERASE },
		{ "Mine",        ID_OTHER_LAYMINES },
		{ "Link (L)",    ID_OTHER_LINK },
		{ "Damage (D)",  ID_OTHER_DAMAGE },
	};

	ImGuiIO& io = ImGui::GetIO();

	// One-time DPI-aware default: scale the whole UI by the editor window's DPI
	// (96 dpi = 1.0x). Keeps panels readable on HiDPI displays out of the box.
	if (!s_uiScaleInit)
	{
		s_uiScaleInit = true;
		UINT dpi = 96;
		if (m_hWnd && ::IsWindow(m_hWnd))
			dpi = ::GetDpiForWindow(m_hWnd);
		float guess = (dpi > 0 ? (float)dpi / 96.0f : 1.0f);
		// Bias up a touch (these panels read small even at 100%) and clamp.
		guess *= 1.25f;
		s_uiScale = (guess < 1.0f) ? 1.0f : (guess > 3.0f ? 3.0f : guess);
	}
	// Apply the master scale before any window is drawn this frame.
	io.FontGlobalScale = s_uiScale;

	// --- Collapsible panels -------------------------------------------------------
	// The full-window scene (RTT off, the exact-picking path) means the docked panel
	// column OVERLAYS the map's right edge. Let the user collapse it to reveal the
	// whole map. Toggle with backtick (`) or the always-visible button (top-right).
	// The status HUD and the toggle button stay visible when collapsed.
	static bool s_panelsVisible = true;
	{
		static bool s_togPrev = false;
		const bool tog = (::GetAsyncKeyState(VK_OEM_3) & 0x8000) != 0;   // backtick `
		if (tog && !s_togPrev && !io.WantTextInput)
			s_panelsVisible = !s_panelsVisible;
		s_togPrev = tog;
	}

	// Status HUD (top-left) — always visible, even when panels are collapsed.
	{
		ImGui::SetNextWindowPos( ImVec2( 8.f, 8.f ), ImGuiCond_Always );
		ImGui::SetNextWindowBgAlpha( 0.35f );
		extern const char* EditorStartupWarning();
		extern void EditorClearStartupWarning();
		const bool hudHasWarning = ( EditorStartupWarning() != nullptr );
		ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
		// Accept input only while the dismissable warning banner is showing (its "x"
		// button must be clickable); otherwise the HUD is a passthrough overlay.
		if ( !hudHasWarning )
			hudFlags |= ImGuiWindowFlags_NoInputs;
		if ( ImGui::Begin( "##statushud", nullptr, hudFlags ) )
		{
			const char* mod = ModPicker::ActiveMod();
			ImGui::Text( "Mod: %s", ( mod && mod[0] ) ? mod : "None (stock)" );
			if ( EditorModProject::IsActive() )
				ImGui::Text( "Project: %s", EditorModProject::Id() );
			if ( land && Terrain::realVerticesMapSide > 1 )
				ImGui::Text( "Terrain: %ld x %ld", Terrain::realVerticesMapSide, Terrain::realVerticesMapSide );
			else
				ImGui::TextDisabled( "Terrain: (none)" );
			int selCount = EditorObjectMgr::instance() ? EditorObjectMgr::instance()->getSelectionCount() : 0;
			ImGui::Text( "Selected: %d", selCount );
			ImGui::Text( "Foliage: %d", FoliageRender::Count() );
			ImGui::Text( "%.0f fps", ImGui::GetIO().Framerate );

			// Non-blocking startup warning (e.g. empty-install / no assets). Yellow,
			// wrapped, with a dismiss "x" that clears it for the session. Replaces the
			// old blocking ::MessageBoxA the user had to OK on every launch.
			if ( const char* warn = EditorStartupWarning() )
			{
				ImGui::Separator();
				ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.0f, 0.78f, 0.20f, 1.0f ) );
				if ( ImGui::SmallButton( "x##startupwarn" ) )
					EditorClearStartupWarning();
				ImGui::SameLine();
				ImGui::PushTextWrapPos( ImGui::GetCursorPosX() + 320.f );
				ImGui::TextUnformatted( warn );
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
		}
		ImGui::End();
	}

	// Always-visible collapse toggle (top-right overlay; never docked).
	{
		ImGui::SetNextWindowPos( ImVec2( io.DisplaySize.x - 160.f * s_uiScale, 8.f ), ImGuiCond_Always );
		ImGui::SetNextWindowBgAlpha( 0.35f );
		ImGuiWindowFlags tf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
		if ( ImGui::Begin( "##panelToggle", nullptr, tf ) )
		{
			if ( ImGui::Button( s_panelsVisible ? "Hide Panels (`)" : "Show Panels (`)" ) )
				s_panelsVisible = !s_panelsVisible;
		}
		ImGui::End();
	}

	if ( !s_panelsVisible )
		return;   // collapsed: skip the Tools window + all docked panels (HUD + toggle stay)

	const float toolbarW = 195.0f * s_uiScale;
	// SetNextWindowPos pulls a window OUT of the DockBuilder assignment (floats it), so
	// skip it under autodock -- the Tools panel then docks into the right column.
	if (!GuiRuntime::AutoDockActive())
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - toolbarW - 16.0f, 16.0f), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(toolbarW, 0.f), ImGuiCond_Once);
	ImGui::Begin("Tools", nullptr, 0);

	// UI scale control — affects every editor panel (FontGlobalScale). +/- and a
	// slider; the value persists for the session.
	ImGui::TextUnformatted("UI Scale");
	if (ImGui::SmallButton("-")) s_uiScale -= 0.1f;
	ImGui::SameLine();
	if (ImGui::SmallButton("+")) s_uiScale += 0.1f;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1.f);
	ImGui::SliderFloat("##uiscale", &s_uiScale, 0.8f, 3.0f, "%.1fx");
	if (s_uiScale < 0.8f) s_uiScale = 0.8f;
	if (s_uiScale > 3.0f) s_uiScale = 3.0f;
	ImGui::Separator();

	// Pre-load mod selector: mount a mod's content BEFORE loading a mission (default
	// None = stock). Above the load/generate actions so it's set first.
	ModPicker::Draw();
	ImGui::Separator();

	// Mod Project — bind the session to a mods/<id> folder (Open/New/Close). When active,
	// File save dialogs default to <root>\data\missions and the mod is mounted for assets.
	if (ImGui::Button("Mod Project", ImVec2(-1.f, 0.f)))
		EditorModProject::Toggle();
	EditorModProject::Draw();
	if (EditorModProject::IsActive())
		ImGui::TextDisabled("Project: %s", EditorModProject::Id());
	ImGui::Separator();

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
	if (ImGui::Button("Generate Map", ImVec2(-1.f, 0.f)))
		MapGeneratorDialog::Open();
	ImGui::PopStyleColor();

	// Draw the Map Generator dialog (no-op when closed).
	MapGeneratorDialog::Draw();

	// Foliage Detail — add/tweak trees, rocks, bushes on the generated map.
	// IN PROGRESS: greyed out until real prop-instance foliage rendering lands
	// (the preview only produced flat placeholder cards). The panel itself also
	// shows an "(in progress)" banner + disabled controls if reached.
	ImGui::BeginDisabled(true);
	ImGui::Button("Foliage Detail  (in progress)", ImVec2(-1.f, 0.f));
	ImGui::EndDisabled();
	MapGeneratorDialog::DrawFoliagePanel();

	// Beauty Sidecar Preview (EDITOR-SIDECAR-PREVIEW-1) — apply/restore an offline
	// terrain beautify delta on the live terrain.
	BeautySidecarPreview::MaybeAutoApply();
	BeautySidecarPreview::DrawImGui();

	// Mission Save Checklist — shows why .pak save is ready/blocked + warnings.
	if (ImGui::Button("Mission Checklist", ImVec2(-1.f, 0.f)))
		MissionValidator::Open();
	MissionValidator::Draw();

	// Async task monitor (terrain gen progress/cancel/log). Own window; no-op empty.
	EditorTaskRunner::RenderImGui();

	// Debug overlay control panel (chunk/superchunk grid toggles + stats).
	EditorDebugOverlay::RenderImGui();
	// (Status HUD is drawn at the top of this function so it stays visible when the
	// panel column is collapsed.)

	// Scene Outliner Lite — read-only list of placed objects, click-to-select.
	if (ImGui::Button("Scene Outliner", ImVec2(-1.f, 0.f)))
		SceneOutliner::Toggle();
	SceneOutliner::Draw();

	// Inspector Lite — read-only details of the current selection.
	if (ImGui::Button("Inspector", ImVec2(-1.f, 0.f)))
		InspectorPanel::Toggle();
	InspectorPanel::Draw();

	// Objectives Overview — read-only objectives explainer (per-team WHEN/THEN cards).
	if (ImGui::Button("Objectives", ImVec2(-1.f, 0.f)))
		ObjectivesSummaryPanel::Toggle();
	ObjectivesSummaryPanel::Draw();

	// Campaign Overview — read-only campaign explainer (load a .fit on demand).
	if (ImGui::Button("Campaign", ImVec2(-1.f, 0.f)))
		CampaignSummaryPanel::Toggle();
	CampaignSummaryPanel::Draw();

	// AI / Brain / Orders — selected unit's AI brain + scaffolded orders/stance.
	if (ImGui::Button("AI / Brain / Orders", ImVec2(-1.f, 0.f)))
		UnitBrainPanel::Toggle();
	UnitBrainPanel::Draw();

	// Telemetry — read-only live editor state (fps / counts / undo / dirty / map).
	if (ImGui::Button("Telemetry", ImVec2(-1.f, 0.f)))
		TelemetryPanel::Toggle();
	TelemetryPanel::Draw();

	// Asset Browser Lite — searchable object catalog; click to start placing.
	if (ImGui::Button("Asset Browser", ImVec2(-1.f, 0.f)))
		AssetBrowser::Toggle();
	AssetBrowser::Draw();

	// Gameplay Debugger — read-only runtime state for the selection. Editor has
	// no live sim (ObjectManager is null), so it shows static editor data + a
	// "not simulating" notice; wired now for when a sim path exists.
	if (ImGui::Button("Gameplay Debugger", ImVec2(-1.f, 0.f)))
		GameplayDebugger::Toggle();
	GameplayDebugger::Draw();

	// Playtest Results — read-only summary of the most recent playtest's archived
	// stdout (status chip, child exit, SMOKE verdict, PERF p50/p99, mover counts,
	// warning/fatal markers). Pure post-hoc log reader; no engine telemetry.
	if (ImGui::Button("Playtest Results", ImVec2(-1.f, 0.f)))
		EditorPlaytestResults::Toggle();
	EditorPlaytestResults::Draw();

	// Undo History — display-only list of undo actions with the current cursor.
	if (ImGui::Button("Undo History", ImVec2(-1.f, 0.f)))
		UndoHistoryPanel::Toggle();
	UndoHistoryPanel::Draw();

	// Command Palette — searchable list of editor commands (also Ctrl+P).
	if (ImGui::Button("Command Palette", ImVec2(-1.f, 0.f)))
		CommandPalette::Toggle();
	CommandPalette::Draw();

	// === Authoring mode selector (contextual tool settings) =================
	// EDITOR-CONTEXTUAL-TOOLS-1: one active mode; only its controls render below,
	// replacing the old flat wall where every tool's settings stacked at once.
	// s_toolMode tracks the active brush (brush-pointer change = re-derive) so a
	// brush switch from the legacy menu or a hotkey moves the selector too; the
	// non-brush modes (Relief/Mission/Place-help) are pure UI and therefore stick.
	ImGui::Separator();
	enum ToolMode { MODE_SELECT = 0, MODE_SCULPT, MODE_STAMP, MODE_PAINT, MODE_RELIEF, MODE_PLACE, MODE_MISSION };
	static int   s_toolMode = MODE_SELECT;
	static void* s_lastBrushForMode = (void*)-1;
	if (curBrush != s_lastBrushForMode)
	{
		s_lastBrushForMode = curBrush;
		if      (dynamic_cast<HeightBrush*>(curBrush))   s_toolMode = MODE_SCULPT;
		else if (dynamic_cast<StampBrush*>(curBrush))    s_toolMode = MODE_STAMP;
		else if (dynamic_cast<TerrainBrush*>(curBrush))  s_toolMode = MODE_PAINT;
		else if (dynamic_cast<BuildingBrush*>(curBrush) ||
		         dynamic_cast<ScatterBrush*>(curBrush))  s_toolMode = MODE_PLACE;
		else                                             s_toolMode = MODE_SELECT;
	}
	{
		struct ModeDef { const char* label; int mode; };
		static const ModeDef modes[] = {
			{ "Select",  MODE_SELECT  }, { "Sculpt",  MODE_SCULPT },
			{ "Stamp",   MODE_STAMP   }, { "Paint",   MODE_PAINT  },
			{ "Relief",  MODE_RELIEF  }, { "Place",   MODE_PLACE  },
			{ "Mission", MODE_MISSION },
		};
		const int nModes = (int)(sizeof(modes) / sizeof(modes[0]));
		const float colW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		for (int i = 0; i < nModes; ++i)
		{
			const bool on = (s_toolMode == modes[i].mode);
			if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
			if (ImGui::Button(modes[i].label, ImVec2(colW, 0.f)))
			{
				s_toolMode = modes[i].mode;
				// Arm the matching brush on entering a brush-backed mode so it is
				// usable immediately (no second click in the sub-controls).
				if      (modes[i].mode == MODE_SCULPT && !dynamic_cast<HeightBrush*>(curBrush))  setSculptBrush(0);
				else if (modes[i].mode == MODE_STAMP  && !dynamic_cast<StampBrush*>(curBrush))   setStampBrush(0);
				else if (modes[i].mode == MODE_PAINT  && !dynamic_cast<TerrainBrush*>(curBrush)) setPaintMaterialBrush(m_paintMaterialType);
			}
			if (on) ImGui::PopStyleColor();
			if ((i % 2) == 0 && i + 1 < nModes) ImGui::SameLine();
		}
	}
	ImGui::Separator();

	// Place Tool / Mission Tools own their own windows and must submit every frame
	// regardless of the active mode, or an already-open window would vanish.
	renderPlacePanelImGui();
	renderMissionToolsImGui();

	if (s_toolMode == MODE_PLACE)
	{
		// Place Tool — scatter mode + params + active brush (wraps existing brushes).
		if (ImGui::Button("Place Tool", ImVec2(-1.f, 0.f)))
			s_placePanelOpen = !s_placePanelOpen;
		ImGui::TextWrapped("Pick an object from the Objects menu to start placing. The "
			"Objects palette (sibling swap / recent) appears on the left while a "
			"placement brush is active.");
	}

	if (s_toolMode == MODE_MISSION)
	{
		// Mission Tools — Test Mission (launch game) + Build Mod Package.
		if (ImGui::Button("Mission Tools", ImVec2(-1.f, 0.f)))
			s_missionToolsOpen = !s_missionToolsOpen;

		// Playtest in Game: one-click save -> launch mc2.exe -> capture log. Stop
		// while running; status (state / exit code / last line) below the button.
		{
			const bool running = EditorPlaytest::IsRunning();
			if (running)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.25f, 0.25f, 1.0f));
				if (ImGui::Button("Stop Playtest", ImVec2(-1.f, 0.f)))
					EditorPlaytest::Stop();
				ImGui::PopStyleColor();
			}
			else
			{
				const bool can = EditorPlaytest::CanPlaytest();
				if (!can) ImGui::BeginDisabled();
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
				if (ImGui::Button("Playtest", ImVec2(-1.f, 0.f)))
					EditorPlaytest::Start();
				ImGui::PopStyleColor();
				if (!can) ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !can)
					ImGui::SetTooltip("Load + save a mission first.");
			}
			ImGui::TextWrapped("%s", EditorPlaytest::StatusLine());
		}
	}

	if (s_toolMode == MODE_SELECT)
	for (int i = 0; i < (int)(sizeof(tools) / sizeof(tools[0])); ++i)
	{
		const bool active = (currentBrushMenuID == tools[i].cmdId);
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
		if (ImGui::Button(tools[i].label, ImVec2(-1.f, 0.f)))
			handleNewMenuMessage(tools[i].cmdId);  // direct call (reliable; these are brush switches, no dialogs)
		if (active)
			ImGui::PopStyleColor();
	}

	if (s_toolMode == MODE_SCULPT)
	{
		ImGui::Text("Sculpt Terrain");
		HeightBrush* hb = dynamic_cast<HeightBrush*>(curBrush);
		struct SM { const char* label; int mode; };
		static const SM sm[] = { { "Raise", 0 }, { "Lower", 1 }, { "Smooth", 2 } };
		for (int i = 0; i < 3; ++i)
		{
			const bool sactive = hb && (int)hb->getMode() == sm[i].mode;
			if (sactive)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.20f, 0.70f, 1.0f));
			if (ImGui::Button(sm[i].label, ImVec2(-1.f, 0.f)))
				setSculptBrush(sm[i].mode);
			if (sactive)
				ImGui::PopStyleColor();
		}
		if (ImGui::SliderFloat("Size", &m_sculptRadius, 64.0f, 3000.0f, "%.0f") && hb)
			hb->setRadius(m_sculptRadius);
		if (ImGui::SliderFloat("Strength", &m_sculptStrength, 1.0f, 300.0f, "%.0f") && hb)
			hb->setStrength(m_sculptStrength);
	}

	if (s_toolMode == MODE_STAMP)
	{
		ImGui::Text("Terrain Stamp");
		StampBrush* st = dynamic_cast<StampBrush*>(curBrush);
		struct SD { const char* label; int type; };
		static const SD sd[] = { { "Pad", 0 }, { "Crater", 1 }, { "Hill", 2 } };
		for (int i = 0; i < 3; ++i)
		{
			const bool sactive = st && (int)st->getType() == sd[i].type;
			if (sactive)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.70f, 0.85f, 1.0f));
			if (ImGui::Button(sd[i].label, ImVec2(-1.f, 0.f)))
				setStampBrush(sd[i].type);
			if (sactive)
				ImGui::PopStyleColor();
		}
		if (ImGui::SliderFloat("StampSize", &m_stampRadius, 64.0f, 3000.0f, "%.0f") && st)
			st->setRadius(m_stampRadius);
		if (ImGui::SliderFloat("StampAmt", &m_stampStrength, 1.0f, 500.0f, "%.0f") && st)
			st->setStrength(m_stampStrength);
	}

	if (s_toolMode == MODE_PAINT)
	{
		ImGui::Text("Paint Material");
		// TERRAIN-MATERIAL-PAINT-1 (Slice 1): surface the dormant TerrainBrush.
		// Each button installs a TerrainBrush painting one TerrainType (mclib/dmapdata.h)
		// via setPaintMaterialBrush() -> setActiveBrush() (same path as Sculpt/Stamp).
		// The 4 buttons map to the 4 visual PBR buckets the renderer blends
		// (terrain_mat_layers.hglsl): Rock->Mountain(6), Grass(9), Dirt(4), Concrete(10).
		TerrainBrush* tb = dynamic_cast<TerrainBrush*>(curBrush);
		struct PM { const char* label; int type; };
		static const PM pm[] = {
			{ "Rock",     MC_MOUNTAIN_TYPE },
			{ "Grass",    MC_GRASS_TYPE    },
			{ "Dirt",     MC_DIRT_TYPE     },
			{ "Concrete", MC_CONCRETE_TYPE },
		};
		for (int i = 0; i < 4; ++i)
		{
			// Highlight only while a TerrainBrush is the active tool AND it matches the
			// last material we installed (TerrainBrush exposes no type getter).
			const bool pactive = tb && (m_paintMaterialType == pm[i].type);
			if (pactive)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.55f, 0.20f, 1.0f));
			if (ImGui::Button(pm[i].label, ImVec2(-1.f, 0.f)))
				setPaintMaterialBrush(pm[i].type);
			if (pactive)
				ImGui::PopStyleColor();
		}
		// Slice 0: TerrainBrush now fills all vertices within this world-space radius
		// (mirrors the sculpt brush). Update the live brush as the slider moves.
		if (ImGui::SliderFloat("PaintSize", &m_paintMaterialSize, 64.0f, 3000.0f, "%.0f") && tb)
			tb->setRadius(m_paintMaterialSize);
		// LOD-chunk colormap caveat: only concrete is type-gated in-shader today.
		ImGui::TextWrapped("Concrete paints live; grass/dirt/rock may need a reload to show (LOD-chunk colormap).");
	}

	if (s_toolMode == MODE_RELIEF)
	{
		ImGui::Text("Terrain Relief");
		// Amplify/flatten the whole heightmap about its mean (exaggerate hills +
		// deepen basins, or smooth toward flat). Repeatable per click.
		if (ImGui::Button("Amplify x1.5", ImVec2(-1.f, 0.f)))
			EditorData::amplifyTerrain(1.5f);
		if (ImGui::Button("Flatten x0.67", ImVec2(-1.f, 0.f)))
			EditorData::amplifyTerrain(0.6667f);

		// Live water-height slider (same effect as the Water dialog: sets the
		// water elevation/plane; recalcWater reflows which cells are submerged).
		// Raise it into the terrain range to flood basins with a real water surface.
		if (land && Terrain::mapData)
		{
			m_waterHeight = Terrain::mapData->waterDepth;   // sync display to current
			if (ImGui::SliderFloat("Water Ht", &m_waterHeight, -50.0f, 800.0f, "%.0f"))
			{
				Terrain::waterElevation     = m_waterHeight;
				Terrain::mapData->waterDepth = m_waterHeight;
				land->recalcWater();
			}
			// On release, rebuild the water recipe set so the new level re-evaluates
			// which cells are water against the current (possibly sculpted) terrain
			// -- this is what makes water divide around ridges/pathways.
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				WaterStream::Reset();
				WaterStream::Build();
			}
		}
	}

	ImGui::End();

	renderObjectCompanionPanel();
	renderObjectInfoPanel();
}

// Companion panel for object placement: when a BuildingBrush is active, show the
// current TEAM, the active object's same-group siblings (one-click swap), and an
// MRU "Recent" strip. Lets the user bounce between related objects (wall <-> gate)
// without re-walking the Objects menu. Shares selectBuildingObject() with the menu.
void EditorInterface::renderObjectCompanionPanel()
{
	// Panel is active for either object-placement brush (single place or scatter).
	int group = -1, activeIdx = -1;
	if ( BuildingBrush* bb = dynamic_cast<BuildingBrush*>(curBrush) )
	{
		group = bb->getGroup();
		activeIdx = bb->getIndexInGroup();
	}
	else if ( ScatterBrush* sb = dynamic_cast<ScatterBrush*>(curBrush) )
	{
		group = sb->getGroup();
		activeIdx = sb->getIndexInGroup();
	}
	else
		return;

	EditorObjectMgr* pMgr = EditorObjectMgr::instance();
	if (group < 0 || group >= pMgr->getBuildingGroupCount())
		return;

	// Same-group object palette. Pop it up on the LEFT edge of the screen so it does
	// not collide with the right-column dock, and scale its geometry by the editor UI
	// scale (s_uiScale). Font size is inherited from io.FontGlobalScale (which is set
	// to s_uiScale) like every other panel -- the old SetWindowFontScale(1.3f) here
	// double-scaled it and made this palette look oversized / "funny" vs the rest of
	// the editor, and ignored DPI. ImGuiCond_Always on the position keeps it reliably
	// on the left each time it pops (it is a transient per-brush context palette).
	const float w = 240.0f * s_uiScale;
	ImGui::SetNextWindowPos(ImVec2(16.0f * s_uiScale, 360.0f * s_uiScale), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(w, 0.f), ImGuiCond_Once);
	ImGui::Begin("Objects", nullptr, ImGuiWindowFlags_NoScrollbar);

	// Clicking a button must not delete curBrush (this bb) or mutate the recent
	// ring mid-render, so defer the actual swap until after all widgets are drawn.
	int pendGroup = -1, pendIndex = -1;

	// TEAM indicator (display only in v1).
	const int align = currentAlignmentFromMenu();
	if (align == EDITOR_TEAMNONE)
		ImGui::Text("TEAM: Neutral");
	else
		ImGui::Text("TEAM: %d", align - EDITOR_TEAM1 + 1);

	// Scatter mode: paint many jittered copies in a radius (generalised forest
	// brush). Toggling re-creates the current brush in the new mode (deferred).
	bool scatterToggled = false;
	if (ImGui::Checkbox("Scatter mode", &m_scatterMode))
		scatterToggled = true;
	if (m_scatterMode)
	{
		ImGui::SliderFloat("Radius",  &ScatterBrush::s_radius,     64.0f, 3000.0f, "%.0f");
		ImGui::SliderInt  ("Density", &ScatterBrush::s_density,    1,     60);
		ImGui::SliderFloat("Spacing", &ScatterBrush::s_minSpacing, 0.0f,  512.0f, "%.0f");
		ImGui::SliderFloat("ScaleJit",&ScatterBrush::s_scaleJitter,0.0f,  0.9f,   "%.2f");
		ImGui::SliderFloat("MaxSlope",&ScatterBrush::s_maxSlopeRise,0.0f, 256.0f, "%.0f");
		ImGui::Checkbox   ("Rand rot", &ScatterBrush::s_randomRotation);
	}
	ImGui::Separator();

	// Same-group siblings.
	ImGui::Text("%s", pMgr->getGroupName(group));
	const int count = pMgr->getNumberBuildingsInGroup(group);
	if (count > 0)
	{
		const char* names[512] = { 0 };
		int n = 0;
		const int cap = (count <= 512) ? count : 512;
		pMgr->getBuildingNamesInGroup(group, names, n);
		if (n > cap) n = cap;
		for (int i = 0; i < n; ++i)
		{
			const bool active = (i == activeIdx);
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
			ImGui::PushID(i);
			if (ImGui::Button(names[i] ? names[i] : "?", ImVec2(-1.f, 0.f)))
			{
				pendGroup = group;
				pendIndex = i;
			}
			ImGui::PopID();
			if (active)
				ImGui::PopStyleColor();
		}
	}

	// Ecosystem Components section -------------------------------------------
	// When the selected building is part of a functional system (turrets,
	// sensors, spotlights) show all related building types grouped by role so
	// the user can quickly place every component of that system.
	{
		using BT = EditorObjectMgr::BuildingType;

		// Three ecosystems: each entry lists the trigger types, the role
		// names, and which specialType maps to which role index.
		struct EcoRole { const char* label; BT type; };
		struct Ecosystem
		{
			const char* header;
			EcoRole     roles[4];
			int         roleCount;
		};
		static const Ecosystem k_ecosystems[] =
		{
			{ "Turret Components",    { {"Controls",   BT::TURRET_CONTROL},
			                            {"Generators", BT::TURRET_GENERATOR},
			                            {"Turrets",    BT::EDITOR_TURRET} }, 3 },
			{ "Sensor Components",   { {"Controls",   BT::SENSOR_CONTROL},
			                            {"Sensors",    BT::SENSOR_TOWER}  }, 2 },
			{ "Spotlight Components",{ {"Controls",   BT::SPOTLIGHT_CONTROL},
			                            {"Spotlights", BT::SPOTLIGHT}     }, 2 },
		};
		static const int k_numEco = (int)(sizeof(k_ecosystems) / sizeof(k_ecosystems[0]));

		// Determine current building's specialType.
		const BT curType = (group >= 0 && activeIdx >= 0)
		    ? pMgr->getBuildingSpecialType(group, activeIdx)
		    : BT::UNSPECIAL;

		// Find which ecosystem (if any) this type belongs to.
		const Ecosystem* eco = nullptr;
		for (int e = 0; e < k_numEco && !eco; ++e)
			for (int r = 0; r < k_ecosystems[e].roleCount && !eco; ++r)
				if (k_ecosystems[e].roles[r].type == curType)
					eco = &k_ecosystems[e];

		if (eco)
		{
			ImGui::Separator();
			if (ImGui::CollapsingHeader(eco->header, ImGuiTreeNodeFlags_DefaultOpen))
			{
				const int totalGroups = pMgr->getBuildingGroupCount();
				for (int r = 0; r < eco->roleCount; ++r)
				{
					const BT roleType = eco->roles[r].type;
					// Collect all buildings of this role type across all groups.
					bool headerPrinted = false;
					int  ecoID = 2000 + r * 256; // unique widget ID base
					for (int g = 0; g < totalGroups; ++g)
					{
						const int cnt = pMgr->getNumberBuildingsInGroup(g);
						for (int i = 0; i < cnt; ++i)
						{
							if (pMgr->getBuildingSpecialType(g, i) != roleType)
								continue;
							if (!headerPrinted)
							{
								ImGui::TextDisabled("%s", eco->roles[r].label);
								headerPrinted = true;
							}
							const bool isActive = (g == group && i == activeIdx);
							if (isActive)
								ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
							ImGui::PushID(ecoID++);
							if (ImGui::Button(pMgr->getBuildingName(g, i), ImVec2(-1.f, 0.f)))
							{
								pendGroup = g;
								pendIndex = i;
							}
							ImGui::PopID();
							if (isActive)
								ImGui::PopStyleColor();
						}
					}
				}
			}
		}
	}
	// End Ecosystem Components ------------------------------------------------

	// Recent (MRU across groups). Validate each entry against the current palette.
	const ObjectRecentRing& ring = objectRecentRing();
	if (!ring.items().empty())
	{
		ImGui::Separator();
		ImGui::Text("Recent");
		int rid = 0;
		for (const RecentObject& r : ring.items())
		{
			if (r.group < 0 || r.group >= pMgr->getBuildingGroupCount())
				continue;
			const int rc = pMgr->getNumberBuildingsInGroup(r.group);
			if (r.indexInGroup < 0 || r.indexInGroup >= rc)
				continue;

			const char* rnames[512] = { 0 };
			int rn = 0;
			pMgr->getBuildingNamesInGroup(r.group, rnames, rn);
			const char* nm = (r.indexInGroup < rn && rnames[r.indexInGroup]) ? rnames[r.indexInGroup] : "?";

			char label[160];
			sprintf(label, "%s: %s", pMgr->getGroupName(r.group), nm);
			ImGui::PushID(1000 + rid);
			if (ImGui::Button(label, ImVec2(-1.f, 0.f)))
			{
				pendGroup = r.group;
				pendIndex = r.indexInGroup;
			}
			ImGui::PopID();
			++rid;
		}
	}

	// Toggling scatter mode re-creates the current object's brush in the new mode.
	if (scatterToggled && pendGroup < 0)
	{
		pendGroup = group;
		pendIndex = activeIdx;
	}

	ImGui::End();

	// Apply the deferred selection now that no widget code holds the brush / iterates the ring.
	if (pendGroup >= 0)
		selectBuildingObject(pendGroup, pendIndex);
}

// ---------------------------------------------------------------------------
// Object info panel (left-pinned overlay): shows the FIRST selected object's
// identity/position and the common manipulation actions (locate, rotate,
// inspector, delete). Overlay (NOT a dockspace split) on purpose -- a left dock
// would shift the scene viewport origin and break mouse picking; an overlay
// window floats over the viewport without reflowing it. Selection retrieval
// mirrors InspectorPanel's null-safe path (EditorObjectMgr::instance() +
// getSelectedObjectList()); every deref is guarded since the editor god-class
// crashes easily on a half-built selection.
// ---------------------------------------------------------------------------
void EditorInterface::renderObjectInfoPanel()
{
	// Left "current object" DOCK COLUMN. Pinned flush-left, full viewport height so
	// it reads as a docked left column (not a floating popup). It is NOT a real
	// dockspace split: a true left dock would shrink the scene from the left and
	// shift its origin, drifting object picking (gated on the projZ->GL slice, see
	// PROJZ-GL-UNIFICATION-RECON-1). Pinned-overlay instead — it covers only the
	// scene's left strip; picking in the rest of the scene stays correct. Always
	// pos/size + NoSavedSettings so a stale imgui.ini can never float or hide it.
	ImGuiIO& objIo = ImGui::GetIO();
	const float w    = 270.0f * s_uiScale;
	const float topY = 26.0f * s_uiScale;   // clear the main menu bar
	ImGui::SetNextWindowPos(ImVec2(0.0f, topY), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(w, objIo.DisplaySize.y - topY), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.92f);
	ImGui::Begin("Object", nullptr,
		ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing);

	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	int sel = mgr ? mgr->getSelectionCount() : 0;
	if (sel == 0)
	{
		ImGui::TextDisabled("No selection");
		ImGui::TextWrapped("Select mode: click an object to inspect/manipulate it.");
		ImGui::End();
		return;
	}

	// First selected object via the same null-safe list walk InspectorPanel uses.
	EditorObject* obj = nullptr;
	{
		EditorObjectMgr::EDITOR_OBJECT_LIST list = mgr->getSelectedObjectList();
		for (EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = list.Begin(); !it.IsDone(); it++)
		{
			if (*it)
			{
				obj = (*it);
				break;
			}
		}
	}

	// getPosition() derefs appearance() unconditionally, so the appearance guard
	// here also makes the position read below safe.
	if (!obj || !obj->appearance())
	{
		ImGui::TextDisabled("Selection not ready");
		ImGui::End();
		return;
	}

	const char* name = obj->getDisplayName();
	ImGui::Text("%s", (name && name[0]) ? name : "(unnamed)");
	ImGui::Text("ID %ld   %s", obj->getID(), InspectorPanel::CategoryToken(obj));
	const Stuff::Vector3D& p = obj->getPosition();
	ImGui::Text("Pos  %.0f, %.0f, %.0f", p.x, p.y, p.z);
	if (sel > 1)
		ImGui::TextDisabled("(+%d more selected)", sel - 1);

	ImGui::Separator();

	if (ImGui::Button("Locate (center camera)", ImVec2(-1.f, 0.f)))
	{
		if (eye)
			eye->setPosition(obj->getPosition(), false);
	}

	if (ImGui::Button("Rotate -45", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 4, 0.f)))
		rotateSelectedObjects(-1);
	ImGui::SameLine();
	if (ImGui::Button("Rotate +45", ImVec2(-1.f, 0.f)))
		rotateSelectedObjects(1);

	if (ImGui::Button("Open Inspector (full detail)", ImVec2(-1.f, 0.f)))
		InspectorPanel::Open();

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.25f, 0.25f, 1.0f));
	if (ImGui::Button("Delete", ImVec2(-1.f, 0.f)))
		mgr->deleteSelectedObjects();
	ImGui::PopStyleColor();

	ImGui::End();
}

// ---------------------------------------------------------------------------
// Place Tool panel (Phase 1d): a single always-available control surface for the
// EXISTING object-placement brushes. Shows the active brush, toggles scatter
// mode, and exposes the scatter parameters (the same ScatterBrush statics the
// companion panel edits). No new placement system -- placement still happens via
// BuildingBrush/ScatterBrush -> Action undo.
// ---------------------------------------------------------------------------
void EditorInterface::renderPlacePanelImGui()
{
	if (!s_placePanelOpen)
		return;

	ImGui::SetNextWindowSize(ImVec2(260.f, 0.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Place Tool", &s_placePanelOpen))
	{
		ImGui::End();
		return;
	}

	EditorObjectMgr* pMgr = EditorObjectMgr::instance();

	// Active placement brush (if any) -> show what will be placed.
	int group = -1, idx = -1;
	if (BuildingBrush* bb = dynamic_cast<BuildingBrush*>(curBrush))
	{
		group = bb->getGroup();
		idx   = bb->getIndexInGroup();
	}
	else if (ScatterBrush* sb = dynamic_cast<ScatterBrush*>(curBrush))
	{
		group = sb->getGroup();
		idx   = sb->getIndexInGroup();
	}

	if (pMgr && group >= 0 && group < pMgr->getBuildingGroupCount()
	    && idx >= 0 && idx < pMgr->getNumberBuildingsInGroup(group))
	{
		ImGui::Text("Placing: %s / %s",
		            pMgr->getGroupName(group), pMgr->getBuildingName(group, idx));
	}
	else
	{
		ImGui::TextDisabled("No object selected.");
		ImGui::TextDisabled("Pick one in the Asset Browser.");
	}

	// Team indicator (display only in v1; alignment is driven by the menu).
	const int align = currentAlignmentFromMenu();
	if (align == EDITOR_TEAMNONE)
		ImGui::Text("Team: Neutral");
	else
		ImGui::Text("Team: %d", align - EDITOR_TEAM1 + 1);

	ImGui::Separator();

	// Scatter mode toggle. Re-create the active brush in the new mode so the
	// change takes effect immediately (selectBuildingObject reads m_scatterMode).
	if (ImGui::Checkbox("Scatter mode", &m_scatterMode))
	{
		if (group >= 0 && idx >= 0)
			selectBuildingObject(group, idx);
	}

	if (m_scatterMode)
	{
		ImGui::SliderFloat("Radius",   &ScatterBrush::s_radius,       64.0f, 3000.0f, "%.0f");
		ImGui::SliderInt  ("Density",  &ScatterBrush::s_density,      1,     60);
		ImGui::SliderFloat("Spacing",  &ScatterBrush::s_minSpacing,   0.0f,  512.0f,  "%.0f");
		ImGui::SliderFloat("ScaleJit", &ScatterBrush::s_scaleJitter,  0.0f,  0.9f,    "%.2f");
		ImGui::SliderFloat("MaxSlope", &ScatterBrush::s_maxSlopeRise, 0.0f,  256.0f,  "%.0f");
		ImGui::Checkbox   ("Rand rot", &ScatterBrush::s_randomRotation);
	}

	ImGui::Separator();
	if (ImGui::Button("Clear brush", ImVec2(-1.f, 0.f)))
		KillCurBrush();

	ImGui::End();
}

// ---------------------------------------------------------------------------
// Mission Tools panel (Phase 1d): the smallest useful Test Mission + Build Mod
// Package flows, wrapping existing pieces (EditorTaskRunner for the launch,
// getMapName() for the current .pak, plain file copies for the package). No new
// mission/launch/package SYSTEM -- just buttons over what already exists.
// Editable path fields keep it safe to use on any machine.
// ---------------------------------------------------------------------------
void EditorInterface::renderMissionToolsImGui()
{
	if (!s_missionToolsOpen)
		return;

	ImGui::SetNextWindowSize(ImVec2(440.f, 0.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Mission Tools", &s_missionToolsOpen))
	{
		ImGui::End();
		return;
	}

	const char* missionPath = EditorData::instance ? EditorData::instance->getMapName() : 0;
	const bool  haveMission = (missionPath && missionPath[0]);

	ImGui::Text("Current mission: %s", haveMission ? missionPath : "(unsaved -- save first)");
	ImGui::Separator();

	// --- Test Mission: launch the game on the current .pak --------------------
	ImGui::TextUnformatted("Test Mission");
	static char s_gameExe[512] = "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe";
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("##gameexe", s_gameExe, sizeof(s_gameExe));

	// GAP 1 -- active-mod propagation. ModPicker::Activate() set MC2_ACTIVE_MOD in
	// THIS (editor) process via _putenv_s. EditorTaskRunner launches the game with
	// CreateProcessA(lpEnvironment=NULL), so the child INHERITS the editor's full
	// environment -- MC2_ACTIVE_MOD included. No explicit env block needed; we just
	// surface the active mod so the modder knows which overrides the playtest uses.
	const char* activeMod = ModPicker::ActiveMod();          // "" == stock
	const bool  haveMod    = (activeMod && activeMod[0]);
	const char* modLabel   = haveMod ? activeMod : "stock (no mod)";
	ImGui::Text("Active mod: %s", modLabel);

	// GAP 2 -- stale-build advisory (governance sec 6). Compare game exe mtime to
	// the editor exe's own mtime. If the game exe is OLDER, the modder likely
	// rebuilt the editor but forgot to rebuild/deploy the game -> stale playtest.
	// Non-blocking yellow warning only.
	auto fileWriteTime = [](const char* path, FILETIME* out) -> bool {
		WIN32_FILE_ATTRIBUTE_DATA fad{};
		if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
			return false;
		*out = fad.ftLastWriteTime;
		return true;
	};
	bool     gameExeStale = false;
	FILETIME gameExeFT{};
	bool     haveGameFT = fileWriteTime(s_gameExe, &gameExeFT);
	{
		char editorExe[MAX_PATH] = {0};
		FILETIME editorFT{};
		if (haveGameFT && GetModuleFileNameA(NULL, editorExe, MAX_PATH) > 0 &&
		    fileWriteTime(editorExe, &editorFT))
		{
			// gameExe older than editor exe -> CompareFileTime < 0.
			gameExeStale = (CompareFileTime(&gameExeFT, &editorFT) < 0);
		}
	}
	if (!haveGameFT)
		ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
		                   "WARNING: game exe not found at the path above.");
	else if (gameExeStale)
		ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f),
		                   "WARNING: game exe is older than the editor;\n"
		                   "you may be playtesting a stale build.");

	if (!haveMission) ImGui::BeginDisabled();
	char launchLabel[160];
	snprintf(launchLabel, sizeof(launchLabel),
	         "Launch Game on this Mission [mod: %s]", modLabel);
	if (ImGui::Button(launchLabel, ImVec2(-1.f, 0.f)))
	{
		// Game loads a mission directly from a -mission <pak> argument (same flag
		// the editor itself accepts). Launch async via the existing task runner so
		// the editor UI never blocks; progress/exit show in the Task Monitor.
		// Child inherits MC2_ACTIVE_MOD from the editor env (see GAP 1 above).
		EditorTaskRunner::TaskSpec spec;
		spec.name = "Test Mission";
		char cmd[1100];
		snprintf(cmd, sizeof(cmd), "\"%s\" -mission \"%s\"", s_gameExe, missionPath);
		spec.commandLine = cmd;
		// cwd = the game exe's directory (best effort; "" inherits if unknown).
		std::string exeDir(s_gameExe);
		size_t slash = exeDir.find_last_of("/\\");
		spec.workingDirectory = (slash != std::string::npos) ? exeDir.substr(0, slash) : std::string();

		// GAP 3 -- playtest archive (ruling C3). When the launch completes, write a
		// small record to mods/<id-or-"stock">/.modproject/playtest/<ts>/playtest.json.
		// Snapshot every input the callback needs NOW (the lambda runs later, on the
		// main thread, when the game process exits).
		std::string gameExeSnap   = s_gameExe;
		std::string missionSnap    = missionPath;
		std::string modSnap        = haveMod ? activeMod : "stock";
		bool        staleSnap      = gameExeStale;
		// Format the game exe mtime as FILETIME ticks (stable, opaque) for the record.
		unsigned long long gameMtimeTicks = 0;
		if (haveGameFT)
			gameMtimeTicks = ((unsigned long long)gameExeFT.dwHighDateTime << 32) |
			                 (unsigned long long)gameExeFT.dwLowDateTime;

		auto archive = [gameExeSnap, missionSnap, modSnap, staleSnap, gameMtimeTicks]
		               (const EditorTaskRunner::TaskResult& r)
		{
			// Timestamp: tool artifact, not a deterministic capture -> system time OK.
			SYSTEMTIME st; GetLocalTime(&st);
			char ts[32];
			snprintf(ts, sizeof(ts), "%04u%02u%02u-%02u%02u%02u",
			         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

			// mods/<id>/.modproject/playtest/<ts>/  (create each level; ignore EEXIST).
			char dir[1024];
			snprintf(dir, sizeof(dir), "mods");                                      CreateDirectoryA(dir, NULL);
			snprintf(dir, sizeof(dir), "mods/%s", modSnap.c_str());                  CreateDirectoryA(dir, NULL);
			snprintf(dir, sizeof(dir), "mods/%s/.modproject", modSnap.c_str());      CreateDirectoryA(dir, NULL);
			snprintf(dir, sizeof(dir), "mods/%s/.modproject/playtest", modSnap.c_str()); CreateDirectoryA(dir, NULL);
			snprintf(dir, sizeof(dir), "mods/%s/.modproject/playtest/%s", modSnap.c_str(), ts);
			CreateDirectoryA(dir, NULL);

			// Persist the captured stdout/stderr tail next to the json.
			char logPath[1100];
			snprintf(logPath, sizeof(logPath), "%s/playtest.log", dir);
			if (FILE* lf = fopen(logPath, "wb"))
			{
				fwrite(r.log.data(), 1, r.log.size(), lf);
				fclose(lf);
			}

			char jsonPath[1100];
			snprintf(jsonPath, sizeof(jsonPath), "%s/playtest.json", dir);
			if (FILE* jf = fopen(jsonPath, "wb"))
			{
				// Minimal hand-written JSON (no json dep in the editor). Paths use
				// forward slashes already; no escaping needed for these fields.
				fprintf(jf,
				    "{\n"
				    "  \"timestamp\": \"%s\",\n"
				    "  \"missionPak\": \"%s\",\n"
				    "  \"gameExe\": \"%s\",\n"
				    "  \"gameExeMtimeTicks\": %llu,\n"
				    "  \"gameExeStale\": %s,\n"
				    "  \"activeMod\": \"%s\",\n"
				    "  \"exitCode\": %d,\n"
				    "  \"logPath\": \"%s\"\n"
				    "}\n",
				    ts, missionSnap.c_str(), gameExeSnap.c_str(),
				    gameMtimeTicks, staleSnap ? "true" : "false",
				    modSnap.c_str(), r.exitCode, logPath);
				fclose(jf);
			}
		};
		spec.onSuccessMainThread = archive;
		spec.onFailureMainThread = archive;   // archive crashes/nonzero exits too.

		EditorTaskRunner::StartTask(spec);
	}
	if (!haveMission) ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !haveMission)
		ImGui::SetTooltip("Save the mission to a .pak first (File > Save As).");

	ImGui::Separator();

	// --- Build Mod Package: copy the mission + sidecars into mods/<name>/ -----
	ImGui::TextUnformatted("Build Mod Package");
	static char s_modName[128]  = "my_mission";
	static char s_modsRoot[512] = "mods";
	static char s_pkgStatus[256] = "";
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("Mod name##modname", s_modName, sizeof(s_modName));
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("Mods root##modsroot", s_modsRoot, sizeof(s_modsRoot));

	if (!haveMission) ImGui::BeginDisabled();
	if (ImGui::Button("Build Package", ImVec2(-1.f, 0.f)))
	{
		// Derive <dir>/<base>.pak, then copy <base>.{pak,fit,burnin.jpg,foliage.json}
		// that exist into <modsRoot>/<modName>/missions/. Additive + reversible
		// (delete the folder to undo). Never deletes or overwrites outside dest.
		std::string src(missionPath);
		size_t slash = src.find_last_of("/\\");
		std::string srcDir  = (slash != std::string::npos) ? src.substr(0, slash) : std::string(".");
		std::string fileNm  = (slash != std::string::npos) ? src.substr(slash + 1) : src;
		size_t dot = fileNm.find_last_of('.');
		std::string base = (dot != std::string::npos) ? fileNm.substr(0, dot) : fileNm;

		char destDir[900];
		snprintf(destDir, sizeof(destDir), "%s/%s/missions", s_modsRoot, s_modName);

		// Create mods/<name>/missions/ (each level; ignore "already exists").
		char lvl[900];
		snprintf(lvl, sizeof(lvl), "%s", s_modsRoot);                         CreateDirectoryA(lvl, NULL);
		snprintf(lvl, sizeof(lvl), "%s/%s", s_modsRoot, s_modName);           CreateDirectoryA(lvl, NULL);
		CreateDirectoryA(destDir, NULL);

		const char* exts[] = { ".pak", ".fit", ".burnin.jpg", ".foliage.json" };
		int copied = 0;
		for (int i = 0; i < (int)(sizeof(exts) / sizeof(exts[0])); ++i)
		{
			char srcF[900], dstF[1000];
			snprintf(srcF, sizeof(srcF), "%s/%s%s", srcDir.c_str(), base.c_str(), exts[i]);
			snprintf(dstF, sizeof(dstF), "%s/%s%s", destDir, base.c_str(), exts[i]);
			// CopyFileA(bFailIfExists=FALSE) overwrites only inside our dest dir.
			if (CopyFileA(srcF, dstF, FALSE))
				++copied;
		}
		snprintf(s_pkgStatus, sizeof(s_pkgStatus),
		         "Copied %d file(s) to %s", copied, destDir);
	}
	if (!haveMission) ImGui::EndDisabled();

	if (s_pkgStatus[0])
		ImGui::TextWrapped("%s", s_pkgStatus);

	ImGui::End();
}

#else
void EditorInterface::renderToolbarImGui() {}
void EditorInterface::renderObjectCompanionPanel() {}
void EditorInterface::renderPlacePanelImGui() {}
void EditorInterface::renderMissionToolsImGui() {}
#endif

void EditorInterface::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar) 
{
	
	if (pScrollBar != NULL && pScrollBar->SendChildNotifyLastMsg())
		return;     // eat it

	// ignore scroll bar msgs from other controls
	if (pScrollBar != GetScrollBarCtrl(SB_HORZ))
		return;

//	OnScroll(MAKEWORD(nSBCode, -1), nPos);

	SCROLLINFO sInfo;
	sInfo.cbSize = sizeof ( SCROLLINFO ); 
	sInfo.fMask = SIF_POS | SIF_PAGE;
	sInfo.nMin = 0; 
	sInfo.nMax = 0; 
	sInfo.nPage = 0; 
	sInfo.nPos = 0; 
	sInfo.nTrackPos = 0; 
	
	CPoint pt;

	GetScrollInfo( SB_HORZ, &sInfo );
	pt.x = sInfo.nPos;

	switch ( nSBCode )
	{
		
		case SB_LINELEFT:
			pt.x +=  (-0.01 * HSCROLLBAR_RANGE);
			pt.y = 0;
			break;
		case SB_LINERIGHT:
			pt.x += (0.01 * HSCROLLBAR_RANGE);
			pt.y = 0;
			break;	
		case SB_PAGELEFT:
			pt.x -= sInfo.nPage;
			break;
		case SB_PAGERIGHT:
			pt.x += sInfo.nPage;
			break;
		case SB_THUMBPOSITION:
			pt.x = nPos;
			break;
		case SB_THUMBTRACK:
			pt.x = nPos;
			break;


		default:
			break;
	}

	
	/* this calculation was based in the code for Camera::moveRight(float amount) */
	Stuff::Vector3D direction;
	if (!eye->usePerspective)
	{
		direction.x = 1.0;
		direction.y = 0.0;
		direction.z = 0.0;
	}
	else
	{
		direction.x = -1.0;
		direction.y = 0.0;
		direction.z = 0.0;
	}
	float worldCameraRotation = eye->getCameraRotation();
	OppRotate(direction,worldCameraRotation);

	Stuff::Vector3D eyeDisplacement = eye->getPosition();

	/* maxVisual was taken from Camera::setPosition(). */
	float maxVisual = (Terrain::worldUnitsMapSide / 2) - Terrain::worldUnitsPerVertex;
	float bound = SQRT_2 * maxVisual;
	float amount = bound * 2.0f * (((float)pt.x) / HSCROLLBAR_RANGE - 0.5f);

	float amountMore = amount - direction * eyeDisplacement;
	
	eye->moveRight(amountMore);

	sInfo.nPos = pt.x;
	sInfo.fMask = SIF_POS; 
	
	this->SetScrollPos( SB_HORZ, pt.x );
	
	CWnd ::OnHScroll(nSBCode, nPos, pScrollBar);

	SafeRunGameOSLogic();
	tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
	syncScrollBars();
}


void EditorInterface::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar) 
{
	SCROLLINFO sInfo;
	sInfo.cbSize = sizeof ( SCROLLINFO ); 
	sInfo.fMask = SIF_POS | SIF_PAGE;
	sInfo.nMin = 0; 
	sInfo.nMax = 0; 
	sInfo.nPage = 0; 
	sInfo.nPos = 0; 
	sInfo.nTrackPos = 0; 
	
	CPoint pt;

	GetScrollInfo( SB_VERT, &sInfo );
	pt.y = sInfo.nPos;
		
	switch ( nSBCode )
	{
		
		case SB_LINELEFT:
			pt.x =  0;
			pt.y += (-0.01 * VSCROLLBAR_RANGE);
			break;
		case SB_LINERIGHT:
			pt.x = 0;
			pt.y += (0.01 * VSCROLLBAR_RANGE);
			break;	
		case SB_PAGELEFT:
			pt.y -= sInfo.nPage;
			break;	
		case SB_PAGERIGHT:
			pt.y += sInfo.nPage;
			break;
		case SB_THUMBPOSITION:
			pt.y = nPos;
			break;
		case SB_THUMBTRACK:
			pt.y = nPos;
			break;


		default:
			break;
	}

	/* this calculation was based in the code for Camera::moveRight(float amount) */
	Stuff::Vector3D direction;
	if (!eye->usePerspective)
	{
		direction.x = 0.0;
		direction.y = -1.0;
		direction.z = 0.0;
	}
	else
	{
		direction.x = 0.0;
		direction.y = 1.0;
		direction.z = 0.0;
	}
	float worldCameraRotation = eye->getCameraRotation();
	OppRotate(direction,worldCameraRotation);

	Stuff::Vector3D eyeDisplacement = eye->getPosition();

	/* maxVisual was taken from Camera::setPosition(). */
	float maxVisual = (Terrain::worldUnitsMapSide / 2) - Terrain::worldUnitsPerVertex;
	float bound = SQRT_2 * maxVisual;
	float amount = bound * 2.0f * ((float)pt.y / VSCROLLBAR_RANGE - 0.5f);

	float amountMore = amount - direction * eyeDisplacement;
	eye->moveDown(amountMore);

	SetScrollPos( SB_VERT, pt.y );
	
	CWnd ::OnVScroll(nSBCode, nPos, pScrollBar);

	SafeRunGameOSLogic();
	tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
	syncScrollBars();
}

void EditorInterface::OnSysKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags) 
{
	handleKeyDown( nChar );	
	CWnd ::OnSysKeyDown(nChar, nRepCnt, nFlags);
}

int EditorInterface::UnitSettings()
{
	EList<Unit*, Unit* > list;

	// ok, find the rest of the selected units
	EditorObjectMgr::instance()->getSelectedUnits( list );

	if ( list.Count() )
	{
		// dialog for units (mechs and vehicles)
		UnitSettingsDlg dlg( list, undoMgr );
		if ( IDOK == dlg.DoModal() )
		{
		}
	}

	EditorObjectMgr::EDITOR_OBJECT_LIST list2 = EditorObjectMgr::instance()->getSelectedObjectList();
	if (list2.Count() > list.Count())
	{
		EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator iter;
		for ( iter = list2.End(); !iter.IsDone(); iter-- )
		{
			if ((0 != dynamic_cast<Unit *>(*iter)) || (0 != dynamic_cast<::DropZone *>(*iter)))
			{
				list2.Delete(iter);
			}
		}

		if ( list2.Count() )
		{
			// dialog for buildings (not units, not dropzones)
			BuildingSettingsDlg dlg( list2, undoMgr );
			if ( IDOK == dlg.DoModal() )
			{
			}
		}
	}

	return true;
	
}

void EditorInterface::initTacMap()
{
	// Why don't we just load it from the frickin' file?
	// We go through all the damned trouble to save it every time!
	// This takes a LONG time.
	// -fs
	BYTE* pData = NULL;
	long size = 0;
	
	FullPathFileName mPath;
	bool bFile = false;
	if ( EditorData::instance->getMapName() )
	{
		mPath.init(missionPath,(char *)EditorData::instance->getMapName(),".pak");
		bFile = true;
	}
	
	if ( bFile && fileExists(mPath))
	{
		PacketFile file;
		file.open(mPath);
		
		EditorData::instance->loadTacMap(&file, pData, size, 128);
		
		file.close();
	}
	else
	{
		EditorData::instance->makeTacMap(pData, size, 128); 
	}
	
	tacMap.SetData( pData, size );
	free( pData );
	pData = NULL;
	tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
}

BOOL EditorInterface::PreTranslateMessage(MSG* pMsg) 
{
	if (m_hAccelTable) 
	{
        if (::TranslateAccelerator(m_hWnd, m_hAccelTable, pMsg))
            return(TRUE);
    }     
	
	return CWnd ::PreTranslateMessage(pMsg);
}

void EditorInterface::OnLButtonDblClk(UINT nFlags, CPoint point) 
{
	// by Methuselas: double-click settings must select the clicked object first.
	// The old handler inverse-projected to terrain and then re-projected through
	// getObjectAtPosition(), which can miss remastered mech appearances even when
	// drag-select works.  Use the same screen-space picker as single-click.
	// RTT: viewport-local coords for the dbl-click picker (it unprojects).
	int dblX = (int)point.x, dblY = (int)point.y;
	EditorRttClientToViewport( dblX, dblY );
	EditorObject* pObject = EditorInterface_PickObjectAtScreenPoint(dblX, dblY);
	if (NULL != pObject)
	{
		bool toggle = false;
		EditorInterface_SelectObjectAtScreenPoint(dblX, dblY, toggle);
		s_editorLastObjectClickTime = 0;
		UnitSettings();
	}
	CWnd ::OnLButtonDblClk(nFlags, point);
}

void EditorInterface::OnPaint() 
{
	CPaintDC dc(this); // device context for painting
	static unsigned long s_paintCount = 0;
	++s_paintCount;
	if (s_paintCount <= 5 || (s_paintCount % 120) == 0)
	{
}

	static bool bFirstLoad = true;
	if (bFirstLoad) {
		if (!bThisIsInitialized) {
			/*paint splash screen*/
			if (!m_hSplashBitMap) {
				m_hSplashBitMap = (HBITMAP)LoadImage(NULL, "esplash.bmp", IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
			}
			if (m_hSplashBitMap) {
				CRect rcClient;
				GetClientRect(rcClient);

				BITMAP bm_struct;
				CBitmap* pbmOld = NULL;
				CDC dcMem;
				dcMem.CreateCompatibleDC(&dc);
				{
					CBitmap *pSplashBitmap = CBitmap::FromHandle(m_hSplashBitMap);
					pSplashBitmap->GetBitmap(&bm_struct);
					pbmOld = dcMem.SelectObject(pSplashBitmap);
				}

				int gbmLeft = rcClient.right / 2 - bm_struct.bmWidth / 2;
				int gbmTop = rcClient.bottom / 2 - bm_struct.bmHeight / 2;
				int gbmRight = gbmLeft + bm_struct.bmWidth;
				int gbmBottom = gbmTop + bm_struct.bmHeight;

				CRect rcTmp;
				rcTmp = rcClient;
				rcTmp.bottom = gbmTop;
				dc.FillSolidRect(&rcTmp, RGB(0, 0, 0));
				rcTmp = rcClient;
				rcTmp.top = gbmBottom;
				dc.FillSolidRect(&rcTmp, RGB(0, 0, 0));
				rcTmp = rcClient;
				rcTmp.right = gbmLeft;
				dc.FillSolidRect(&rcTmp, RGB(0, 0, 0));
				rcTmp = rcClient;
				rcTmp.left = gbmRight;
				dc.FillSolidRect(&rcTmp, RGB(0, 0, 0));

				dc.BitBlt(rcClient.right / 2 - bm_struct.bmWidth / 2,
						rcClient.bottom / 2 - bm_struct.bmHeight / 2,
						bm_struct.bmWidth, bm_struct.bmHeight,
						&dcMem, 0, 0, SRCCOPY);

				dcMem.SelectObject(pbmOld);
				dcMem.DeleteDC();
			}
		} else {
			bFirstLoad = false;
		}
	}

	if (ProcessingError || !bThisIsInitialized || bIsLoading)
	{
		return;
	}
	SafeRunGameOSLogic();

	/* This hack is here because syncScrollBars() depends on the function
	Camera::inverseProject(...) which, for some reason, doesn't return the correct value until
	four frames have been drawn.*/
	if (4 == turn)
	{
		syncScrollBars();
	}
	// Do not call CWnd ::OnPaint() for painting messages
}

void EditorInterface::OnViewRefreshtacmap() 
{
	tacMap.UpdateMap();
}

bool EditorInterface::SafeRunGameOSLogic()
{
	static unsigned long s_safeRunCount = 0;
	// Allow rendering when the ImGui map generator dialog is open even if no
	// terrain is loaded yet — the dialog needs GL frames to draw itself.
#ifdef MC2_IMGUI
	const bool needRender = bThisIsInitialized &&
	                        ((NULL != land) || MapGeneratorDialog::IsOpen());
#else
	const bool needRender = bThisIsInitialized && (NULL != land);
#endif
	if (needRender)
	{
		if (!m_bInRunGameOSLogicCall)
		{
			++s_safeRunCount;
			if (s_safeRunCount <= 5 || (s_safeRunCount % 120) == 0)
			{
}
			m_bInRunGameOSLogicCall = true;
			RunGameOSLogic();
			m_bInRunGameOSLogicCall = false;
			return true;
		}
	}
	return false;
}

void EditorInterface::rotateSelectedObjects( int direction )
{
	ModifyBuildingAction *pAction = new ModifyBuildingAction;
	EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjects;
	selectedObjects = EditorObjectMgr::instance()->getSelectedObjectList();
	EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator iter = selectedObjects.Begin();
	while (!iter.IsDone())
	{
		// EDITOR-CRASH-HARDENING-1: appearance() can be NULL; skip (sibling applyObjectTransform:6401).
		if ( !(*iter) || !(*iter)->appearance() )
		{
			iter++;
			continue;
		}
		pAction->addBuildingInfo(*(*iter));
		int id = (*iter)->getID();
		int fitID = EditorObjectMgr::instance()->getFitID(id);
		if ((EditorObjectMgr::WALL == (*iter)->getSpecialType()) || (33/*repair bay*/ == fitID))
		{
			(*iter)->appearance()->rotation += direction * 90;
		}
		else
		{
			(*iter)->appearance()->rotation += direction * 45;
		}
		(*iter)->appearance()->invalidateStaticRegistration();
		(*iter)->appearance()->update();
		(*iter)->appearance()->registerStatic();
		iter++;
	}

	undoMgr.AddAction(pAction);
	pAction = NULL;
}

// Frame the camera on the current selection. Recenters the camera's ground
// anchor on the centroid of the selected objects' XY positions. Read-only:
// touches no object, pushes no undo action, does not mark the mission dirty.
// No-op when nothing is selected. setPosition() clamps the target to the map
// and derives the camera Z from terrain elevation (swoopy z-glide) — the same
// path every other editor camera move uses. (setGoalPosition() alone is inert
// here: updateGoalPosition() resets it to the current pos unless goalPosTime is
// primed, which the editor never does.)
void EditorInterface::frameSelectedObjects()
{
	if ( !eye )
		return;

	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if ( !mgr )
		return;

	EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjects = mgr->getSelectedObjectList();
	Stuff::Vector3D centroid;
	centroid.x = centroid.y = centroid.z = 0.0f;
	int count = 0;
	for ( EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator iter = selectedObjects.Begin();
	      !iter.IsDone(); iter++ )
	{
		if ( !(*iter) )
			continue;
		const Stuff::Vector3D& p = (*iter)->getPosition();
		centroid.x += p.x;
		centroid.y += p.y;
		centroid.z += p.z;
		count++;
	}

	if ( count == 0 )
		return;  // no selection -> no-op

	float inv = 1.0f / (float)count;
	centroid.x *= inv;
	centroid.y *= inv;
	centroid.z *= inv;

	eye->setPosition( centroid, true );  // xy clamped to map, z terrain-locked
}

// Continuous-angle rotation of the current selection, used by the mouse wheel.
// Mirrors rotateSelectedObjects() but applies an arbitrary degree delta instead
// of the discrete 45/90 step, so the wheel can spin objects smoothly.
void EditorInterface::rotateSelectedObjectsDegrees( float deg )
{
	ModifyBuildingAction *pAction = new ModifyBuildingAction;
	EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjects;
	selectedObjects = EditorObjectMgr::instance()->getSelectedObjectList();
	EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator iter = selectedObjects.Begin();
	while (!iter.IsDone())
	{
		// EDITOR-CRASH-HARDENING-1: appearance() can be NULL; skip (sibling applyObjectTransform:6401).
		if ( !(*iter) || !(*iter)->appearance() )
		{
			iter++;
			continue;
		}
		pAction->addBuildingInfo(*(*iter));
		(*iter)->appearance()->rotation += deg;
		// Recompute transform + re-bake the static GPU recipe so static props
		// visibly rotate (mechs no-op the invalidate; see drag-move note).
		(*iter)->appearance()->invalidateStaticRegistration();
		(*iter)->appearance()->update();
		(*iter)->appearance()->registerStatic();
		iter++;
	}

	undoMgr.AddAction(pAction);
	pAction = NULL;
}

// Editable Inspector v1 transform. Reuses ModifyBuildingAction exactly like the
// drag-move (handleLeftButtonDown/Move) and rotateSelectedObjects paths: capture
// the old appearance snapshot, mutate the live appearance, re-bake the static GPU
// recipe, then note the new position so the action's location-based undo can find
// the object at its new pose. No new transform/undo system.
bool EditorInterface::applyObjectTransform( EditorObject* obj, float worldX, float worldY, float yawDegrees )
{
	if ( !obj || !obj->appearance() || !land )
		return false;

	// Forest-member trees are placed/owned by the forest; moving one individually
	// would desync forest bookkeeping. Out of scope for v1.
	if ( obj->getForestID() != -1 )
		return false;

	ModifyBuildingAction* pAction = new ModifyBuildingAction;
	pAction->addBuildingInfo( *obj );   // captures OLD snapshot + OLD position

	Stuff::Vector3D newPos;
	newPos.x = worldX;
	newPos.y = worldY;
	newPos.z = obj->appearance()->position.z;

	// Cell/link bookkeeping (best-effort, mirrors drag-move which ignores the
	// return), then override with the precise free XY and terrain-locked Z.
	int row = 0, col = 0;
	land->worldToCell( newPos, row, col );
	EditorObjectMgr::instance()->moveBuilding( obj, row, col );

	ObjectAppearance* pApp = obj->appearance();
	newPos.z = land->getTerrainElevation( newPos );
	pApp->position = newPos;
	pApp->rotation = yawDegrees;
	pApp->invalidateStaticRegistration();
	pApp->update();
	pApp->registerStatic();

	// buildingIDs must hold the object's CURRENT position so the action's
	// getObjectAtLocation-based undo/redo can locate it (it was captured at the
	// OLD position by addBuildingInfo).
	pAction->updateNotedObjectPositions();

	undoMgr.AddAction( pAction );

	if ( EditorData::instance )
		EditorData::instance->MissionNeedsSaving( true );

	return true;
}

bool EditorInterface::applyObjectAlignment( EditorObject* obj, int newTeam )
{
	if ( !obj || !obj->appearance() )
		return false;

	// Forest-member trees are owned by the forest; skip (mirrors applyObjectTransform).
	if ( obj->getForestID() != -1 )
		return false;

	// Valid team range is [-1,7] (EditorObject::setAlignment / its gosASSERT).
	if ( newTeam < -1 || newTeam > 7 )
		return false;

	// No-op if unchanged -- never push an empty undo entry.
	if ( obj->getAlignment() == newTeam )
		return false;

	ModifyBuildingAction* pAction = new ModifyBuildingAction;
	pAction->addBuildingInfo( *obj );   // snapshot captures the OLD teamId

	obj->setAlignment( newTeam );

	// Buildings are STATIC props whose team colour is baked into the GPU recipe,
	// so re-bake them. UNITS are dynamic movers -- the working UnitSettingsDlg
	// path just calls setAlignment with NO static re-bake; doing the static
	// invalidate/registerStatic on a unit corrupts it (it vanishes when
	// deselected). So skip the re-bake for units.
	if ( !dynamic_cast<Unit*>( obj ) )
	{
		ObjectAppearance* pApp = obj->appearance();
		pApp->invalidateStaticRegistration();
		pApp->update();
		pApp->registerStatic();
	}

	// Position is unchanged, but keep the action's location bookkeeping consistent
	// (undo locates the object by position).
	pAction->updateNotedObjectPositions();

	undoMgr.AddAction( pAction );

	if ( EditorData::instance )
		EditorData::instance->MissionNeedsSaving( true );

	return true;
}

int EditorInterface::runInspectorEditSmoke()
{
	if ( !land || !EditorObjectMgr::instance() )
		return -1;

	// Place a throwaway drop zone near map center (no catalog group/index needed).
	const int mid = ( land->realVerticesMapSide - 1 ) / 2;
	Stuff::Vector3D p;
	land->cellToWorld( mid, mid, p );
	p.z = land->getTerrainElevation( p );

	EditorObject* obj = EditorObjectMgr::instance()->addDropZone( p, 0, false );
	if ( !obj || !obj->appearance() )
		return -1;

	// Exercise the real selection path too.
	EditorObjectMgr::instance()->unselectAll();
	EditorObjectMgr::instance()->select( *obj, true );

	const float ox = obj->getPosition().x;
	const float oy = obj->getPosition().y;

	bool applied = applyObjectTransform( obj, ox + 64.0f, oy + 64.0f, 90.0f );
	const float nx = obj->getPosition().x;
	const float ny = obj->getPosition().y;
	const bool moved = applied && ( fabsf( nx - ox ) > 1.0f || fabsf( ny - oy ) > 1.0f );

	int result = moved ? 1 : 0;

	// Undo through the SAME manager the transform pushed to (member undoMgr).
	if ( undoMgr.HaveUndo() )
	{
		undoMgr.Undo();
		const float ux = obj->getPosition().x;
		const float uy = obj->getPosition().y;
		if ( fabsf( ux - ox ) < 1.0f && fabsf( uy - oy ) < 1.0f )
			result |= 2;
	}

	// Team edit: change alignment, assert it took, then undo and assert restored.
	const int oTeam = obj->getAlignment();
	const int nTeam = ( oTeam == 1 ) ? 2 : 1;   // a different valid team
	if ( applyObjectAlignment( obj, nTeam ) && obj->getAlignment() == nTeam )
	{
		result |= 4;
		if ( undoMgr.HaveUndo() )
		{
			undoMgr.Undo();
			if ( obj->getAlignment() == oTeam )
				result |= 8;
		}
	}

	return result;
}

// -smoke-place-oob: reproduce (and now guard) the off-map placement crash.
// Activates a real BuildingBrush and drives its update() at off-map screen
// points (top edge = horizon -> projects off any finite map; corners + extreme
// out-of-viewport coords cover the OOB cell range). Before the BuildingBrush
// worldToCell clamp, the first off-map point read the heightmap out of bounds
// in MapData::terrainElevation (0xC0000005). Returns 1 if all updates survived,
// -1 if setup failed (no terrain / catalog / camera).
int EditorInterface::runPlaceOobSmoke()
{
	if ( !land )
		return -1;

	// terrainElevation() guards only the UPPER cell bound; a position WEST/NORTH
	// of the map yields a negative mesh offset and reads blocks[<0] out of bounds
	// (0xC0000005). The interactive crash was exactly this. Drive far-off-map
	// world positions through the production guard BuildingBrush::snapToTerrainCell
	// (the same call BuildingBrush::update makes). Without the clamp these crash;
	// with it they snap to an in-range edge cell.
	const float wupv = Terrain::worldUnitsPerVertex;
	const float farW = land->mapTopLeft3d.x - 100000.0f * wupv; // far west  -> cc << 0
	const float farN = land->mapTopLeft3d.y + 100000.0f * wupv; // far north -> cr << 0
	const float farE = land->mapTopLeft3d.x + 100000.0f * wupv; // far east  -> cc >> max
	const float farS = land->mapTopLeft3d.y - 100000.0f * wupv; // far south -> cr >> max

	const float px[] = { farW, land->mapTopLeft3d.x, farW, farE, farW };
	const float py[] = { land->mapTopLeft3d.y, farN, farN, farS, farS };

	// Postcondition check (deterministic — the raw OOB read only *faults* on an
	// unmapped page, which is heap-dependent and unreliable to trigger headless).
	// After snapToTerrainCell, the result MUST land on a valid in-grid cell. Run
	// it back through worldToCell and assert the indices are in range. Without the
	// clamp the snapped position is still off-map -> out-of-range -> returns 0.
	const int maxCell = (int)( ( Terrain::realVerticesMapSide - 1 ) * 3 ) - 1;
	if ( maxCell < 0 )
		return -1;

	for ( int i = 0; i < (int)( sizeof( px ) / sizeof( px[0] ) ); ++i )
	{
		Stuff::Vector3D p;
		p.x = px[i];
		p.y = py[i];
		p.z = 0.0f;
		BuildingBrush::snapToTerrainCell( land, p );  // production guard under test

		int cr = 0, cc = 0;
		land->worldToCell( p, cr, cc );
		if ( cr < 0 || cc < 0 || cr > maxCell || cc > maxCell )
			return 0;  // guard FAILED: snapped result is still off the cell grid
	}

	return 1; // every off-map input snapped back onto the valid cell grid
}

static void UpdateMissionPlayerPlayer(int player, CCmdUI* pCmdUI)
{
	if (EditorData::instance->MaxPlayers() < player) {
		pCmdUI->Enable(FALSE);
	} else {
		pCmdUI->Enable(TRUE);
	}
}

void EditorInterface::OnUpdateMissionPlayerPlayer3(CCmdUI* pCmdUI) 
{
	UpdateMissionPlayerPlayer(3, pCmdUI);
}

void EditorInterface::OnUpdateMissionPlayerPlayer4(CCmdUI* pCmdUI) 
{
	UpdateMissionPlayerPlayer(4, pCmdUI);
}

void EditorInterface::OnUpdateMissionPlayerPlayer5(CCmdUI* pCmdUI) 
{
	UpdateMissionPlayerPlayer(5, pCmdUI);
}

void EditorInterface::OnUpdateMissionPlayerPlayer6(CCmdUI* pCmdUI) 
{
	UpdateMissionPlayerPlayer(6, pCmdUI);
}

void EditorInterface::OnUpdateMissionPlayerPlayer7(CCmdUI* pCmdUI) 
{
	UpdateMissionPlayerPlayer(7, pCmdUI);
}

void EditorInterface::OnUpdateMissionPlayerPlayer8(CCmdUI* pCmdUI) 
{
	UpdateMissionPlayerPlayer(8, pCmdUI);
}

static void UpdateMissionTeamTeam(int team, CCmdUI* pCmdUI)
{
	if (EditorData::instance->MaxTeams() < team) {
		pCmdUI->Enable(FALSE);
	} else {
		pCmdUI->Enable(TRUE);
	}
}

void EditorInterface::OnUpdateMissionTeamTeam3(CCmdUI* pCmdUI) 
{
	UpdateMissionTeamTeam(3, pCmdUI);
}

void EditorInterface::OnUpdateMissionTeamTeam4(CCmdUI* pCmdUI) 
{
	UpdateMissionTeamTeam(4, pCmdUI);
}

void EditorInterface::OnUpdateMissionTeamTeam5(CCmdUI* pCmdUI) 
{
	UpdateMissionTeamTeam(5, pCmdUI);
}

void EditorInterface::OnUpdateMissionTeamTeam6(CCmdUI* pCmdUI) 
{
	UpdateMissionTeamTeam(6, pCmdUI);
}

void EditorInterface::OnUpdateMissionTeamTeam7(CCmdUI* pCmdUI) 
{
	UpdateMissionTeamTeam(7, pCmdUI);
}

void EditorInterface::OnUpdateMissionTeamTeam8(CCmdUI* pCmdUI) 
{
	UpdateMissionTeamTeam(8, pCmdUI);
}

void EditorInterface::OnDestroy()
{
	CWnd ::OnDestroy();

	if (tacMap.m_hWnd)
	{
		tacMap.DestroyWindow();
	}
	else
	{
		// EDITOR-EXIT-CRASH-1 (#4): gosASSERT is a dead no-op in RelWithDebInfo
		// (see mclib review), so gosASSERT(false) here silently did nothing —
		// a null tacMap.m_hWnd at WM_DESTROY was neither logged nor handled.
		// Make the signal live: log it (crashbundle ring + optional file) and
		// continue: tacMap.DestroyWindow() would be a no-op on a null HWND
		// anyway, so there's nothing unsafe left to do here.
		EditorShutdownTrace("[EDITOR_SHUTDOWN] phase=OnDestroy_null_tacmap_hwnd");
	}
}

#pragma warning( default:4244 )

//-------------------------------------------------------------------------------------------------
// Phase 5 PCG foliage commands. Generate runs the Python generator's foliage pass
// on the current generated-map recipe and loads the resulting sidecar; the rest
// are pure overlay-state toggles. None touch terrain/save/load.
//-------------------------------------------------------------------------------------------------
void EditorInterface::OnFoliageGenerate()
{
	extern bool g_cliSuppressModals;   // smoke/headless: skip the modal feedback below
	const char* recipe = "terrain_gen_out\\genmap_recipe.json";
	if ( GetFileAttributes( recipe ) == INVALID_FILE_ATTRIBUTES )
	{
		if (!g_cliSuppressModals) AfxMessageBox( "No generated-map recipe found.\nUse the Map Generator first, then Generate Foliage." );
		return;
	}
	char cmd[1024];
	sprintf( cmd,
		"py -3 tools\\terrain_gen\\terrain_gen.py \"%s\" --out terrain_gen_out "
		"--generate-foliage --foliage-rules tools\\terrain_gen\\recipes\\foliage_rules_example.json",
		recipe );
	int rc = system( cmd );
	if ( rc != 0 )
	{
		if (!g_cliSuppressModals) AfxMessageBox( "Foliage generation failed.\nNeeds Python 3 + tools\\terrain_gen on the editor's working dir." );
		return;
	}
	if ( FoliageRender::Load( "terrain_gen_out\\genmap.foliage.json" ) )
	{
		char msg[128];
		sprintf( msg, "Foliage generated: %d instances.", FoliageRender::Count() );
		if (!g_cliSuppressModals) AfxMessageBox( msg );
	}
	else
	{
		if (!g_cliSuppressModals) AfxMessageBox( "Foliage generated but no instances were placed (check rules / map)." );
	}
}

void EditorInterface::OnFoliageRegenSel()
{
	// v1: selected-superchunk regeneration is not yet wired; regenerate the full
	// foliage set deterministically (same per-superchunk seeds -> stable result).
	OnFoliageGenerate();
}

void EditorInterface::OnFoliageClear()
{
	FoliageRender::Clear();
}

void EditorInterface::OnFoliageToggle()
{
	FoliageRender::Toggle();
}

void EditorInterface::OnForestTool()
{
	if (EditorInterface::instance()->ObjectSelectOnlyMode())
	{
		return;
	}

	ForestDlg dlg;

	long count = 0;
	float xAvg = 0;
	float yAvg = 0;
	

	// figure out selection
	for ( int j = 0; j < land->realVerticesMapSide; ++j )
	{
		for ( int i = 0; i < land->realVerticesMapSide; ++i )
		{
			if ( land->isVertexSelected( j, i ) )
			{
				Stuff::Vector3D pos;
				land->tileCellToWorld( j, i, 0, 0, pos );

				xAvg += pos.x;
				yAvg += pos.y;
				count++;
			}
		}
	}

	// The forest tool scatters trees over the SELECTED terrain vertices. With no
	// selection there is nothing to plant (and the centre below divides by zero) --
	// tell the user instead of silently doing nothing.
	if ( count == 0 )
	{
		MessageBox(
			_T("Select a terrain area first (drag-select with the Area Select tool), then choose Other > Forests."),
			_T("Forest"), MB_OK | MB_ICONINFORMATION );
		return;
	}

	float centerX = xAvg/count;
	float centerY = yAvg/count;

	float dist = 0;
	
	for ( int j = 0; j < land->realVerticesMapSide; ++j )
	{
		for ( int i = 0; i < land->realVerticesMapSide; ++i )
		{
			if ( land->isVertexSelected( j, i ) )
			{
				Stuff::Vector3D pos;
				land->tileCellToWorld( j, i, 0, 0, pos );

				float deltaX = pos.x - centerX;
				float deltaY = pos.y - centerY;

				float tmpDist = deltaX * deltaX + deltaY * deltaY;
				if ( tmpDist > dist )
					dist = tmpDist;
			}
		}
	}

	dlg.m_xLoc = centerX;
	dlg.m_yLoc = centerY;
	dist += 128 * 128; // always make as big as one tile
	dlg.m_radius = (float)sqrt( dist );

	if ( IDOK == dlg.DoModal() )
	{
		long forestID = EditorObjectMgr::instance()->createForest( dlg.forest );
		// Register an undo action. createForest assigns the real ID; fetch the
		// live Forest back so ForestAction snapshots it with that ID.
		const long kMaxForests = 256;
		Forest* forestPtrs[kMaxForests];
		long count = kMaxForests;
		EditorObjectMgr::instance()->getForests( forestPtrs, count );
		for ( long i = 0; i < count; ++i )
		{
			if ( forestPtrs[i] && forestPtrs[i]->getID() == forestID )
			{
				ActionUndoMgr::instance->AddAction( new ForestAction( *forestPtrs[i] ) );
				break;
			}
		}
	}

}

void EditorInterface::OnOtherEditforests() 
{
	if (EditorInterface::instance()->ObjectSelectOnlyMode())
	{
		return;
	}

	EditForestDlg dlg;
	dlg.DoModal();
}

bool TeamsAction::undo() {
	CTeams swap = EditorData::instance->TeamsRef();
	EditorData::instance->TeamsRef() = PreviousTeams();
	PreviousTeams(swap);
	return true;
}

CTeams TeamsAction::PreviousTeams() {
	m_previousTeams.RestoreObjectPointerReferencesFromNotedPositions();
	return m_previousTeams;
}

void TeamsAction::PreviousTeams(const CTeams &teams) {
	m_previousTeams = teams;
	/*Some of the objective conditions have pointers to objects. These pointers may become
	invalid in the case that an object is deleted then restored (via undo/redo), so we store the
	positions of the objects referred to so that we can use them later to retrieve valid pointers
	to the objects. Btw, we do know that the object positions will be valid when we need to infer
	the pointers from them. */
	m_previousTeams.NoteThePositionsOfObjectsReferenced();
}

void EditorInterface::OnViewOrthographiccamera() 
{
	if (GetParent()->GetMenu()->GetMenuState( ID_VIEW_ORTHOGRAPHICCAMERA, MF_BYCOMMAND ) & MF_CHECKED) {
		GetParent()->GetMenu()->CheckMenuItem( ID_VIEW_ORTHOGRAPHICCAMERA, MF_BYCOMMAND | MF_UNCHECKED );
		eye->usePerspective = true;
	} else {
		GetParent()->GetMenu()->CheckMenuItem( ID_VIEW_ORTHOGRAPHICCAMERA, MF_BYCOMMAND | MF_CHECKED );
		eye->usePerspective = false;
	}
	eye->rotateRight(180.0/*degrees*/);
	SafeRunGameOSLogic();
	syncScrollBars();
}

void EditorInterface::OnViewShowpassabilitymap()
{
	bool isChecked = (GetParent()->GetMenu()->GetMenuState(ID_VIEW_SHOWPASSABILITYMAP, MF_BYCOMMAND) & MF_CHECKED) != 0;

	if (isChecked) {
		// Toggle off — clear whichever overlay is active.
		GetParent()->GetMenu()->CheckMenuItem(ID_VIEW_SHOWPASSABILITYMAP, MF_BYCOMMAND | MF_UNCHECKED);
		drawTerrainGrid      = false;
		drawEditorPassability = false;
		EditorNavLayer::Get().Clear();
		return;
	}

	// Toggle on — prefer legacy MOVE overlay (small maps); fall back to EditorNav (large maps).
	if (!GameMap) {
		std::string err;
		EditorData::RebuildMoveFromCurrentTerrain(&err);
		// GameMap is now set if MOVE built successfully; null if map too large.
	}

	if (GameMap) {
		// Legacy MOVE overlay available.
		GetParent()->GetMenu()->CheckMenuItem(ID_VIEW_SHOWPASSABILITYMAP, MF_BYCOMMAND | MF_CHECKED);
		drawTerrainGrid      = true;
		drawEditorPassability = false;
		return;
	}

	// MOVE unavailable (map exceeds MAX_MAP_CELL_WIDTH or build failed) — use EditorNav overlay.
	if (!EditorNavLayer::Get().BuildFromTerrain()) {
		AfxMessageBox("Cannot build terrain passability overlay.");
		GetParent()->GetMenu()->CheckMenuItem(ID_VIEW_SHOWPASSABILITYMAP, MF_BYCOMMAND | MF_UNCHECKED);
		return;
	}

	GetParent()->GetMenu()->CheckMenuItem(ID_VIEW_SHOWPASSABILITYMAP, MF_BYCOMMAND | MF_CHECKED);
	drawEditorPassability = true;
}

void EditorInterface::OnMButtonUp(UINT nFlags, CPoint point) 
{
	if (ThisIsInitialized() && eye)
	{
		eye->allNormal();
	}
	
	CWnd ::OnMButtonUp(nFlags, point);
}
