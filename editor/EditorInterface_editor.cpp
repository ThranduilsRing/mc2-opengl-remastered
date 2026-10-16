// Editor: the Editor object (init, destroy, resave-all, render, update), the message map, the New menu and mission open/save prompts.
// Declared in the class headers; shared names in EditorInterface_internal.h.
#include "EditorInterface_internal.h"



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
					CFileDialog fileDlg( 1,  "pak", nullptr, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR, szPAKFilter );
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
						land->init( 0, (PacketFile*)nullptr, EDITOR_VISIBLE_VERTICES, crap, 100  );
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

	PlaySound("SystemDefault",nullptr,SND_ASYNC);
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
	eye = nullptr;
	EditorShutdownTrace("[EDITOR_SHUTDOWN] phase=Editor_destroy_exit");
}

void Editor::resaveAll (void)
{
	if ( !eye )
		eye = new EditorCamera;

	CoInitialize(nullptr);
	// Asset exports use the editor's version identity.
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
		
		for (auto iter = campaignData.m_GroupList.cbegin(); iter != campaignData.m_GroupList.cend(); ++count, ++iter)
		{
			CGroupData groupData = (*iter);

			strcpy(buf, campaignFiles[i]);
			int campaignLen = (int)strlen(buf);
			buf[campaignLen-4] = '_';
			buf[campaignLen-3] = 0;
			strcat(buf, "group");
			char groupId[4] = {0};
			sprintf(groupId, "%02d", count+1);
			strcat(buf, groupId);

			campaignAssetPtr->AddRelationship("Group", buf);
			
			IProviderAssetPtr groupAssetPtr = armProvider->OpenAsset(buf, AssetType_Virtual, ProviderType_Primary);

			groupAssetPtr->AddProperty("Type", "Mission Group");
			

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
				// Briefing video filenames already include their extension.
				groupAssetPtr->AddRelationship("PreVideo", buf);			
			}

			if (groupData.m_VideoFile != "")
			{
				strcpy(buf, "Data\\Movies\\");
				strcat(buf, groupData.m_VideoFile.GetBuffer());
				strcat(buf, ".bik");
				groupAssetPtr->AddRelationship("Video", buf);			
			}

			for (auto iter2 = groupData.m_MissionList.cbegin(); iter2 != groupData.m_MissionList.cend(); ++iter2)
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

	char findString[512];
	sprintf(findString,"%s*.pak",missionPath);

	WIN32_FIND_DATA	findResult;
	HANDLE searchHandle = FindFirstFile(findString,&findResult);
	do
	{
		if ((findResult.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			char baseName[1024];
			_splitpath(findResult.cFileName,nullptr,nullptr,baseName,nullptr);
			
			FullPathFileName pakName;
			pakName.init(missionPath,baseName,".pak");
			
			FullPathFileName fitName;
			fitName.init(missionPath,baseName,".fit");
			
			if (fileExists(pakName))
			{
				SetFileAttributes(pakName,FILE_ATTRIBUTE_NORMAL);
				SetFileAttributes(fitName,FILE_ATTRIBUTE_NORMAL);
				long result = EditorData::initTerrainFromPCV(pakName);
				if (result)
					data.save(pakName);

				printf( "Resaved Map %s", (const char*)pakName );

			}
		}
	} while (FindNextFile(searchHandle,&findResult) != 0);

	FindClose(searchHandle);

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
	curBrush = nullptr;
	selecting = true;
	realRotation = 0.0;
	currentBrushID = IDS_SELECT;
	currentBrushMenuID = ID_OTHER_SELECT;

	m_pDragObject = nullptr;
	m_dragObjMoved = false;
	m_pDragAction = nullptr;
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
	m_paintMaterialSize = 400.0f;   // Material-paint brush radius.
	m_waterHeight = 0.0f;
	m_pendGenerateMission = false;

	smoothRadius = 2;
	dragging = false;
	prevBrush = nullptr;
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
	menus = nullptr;
	menusAllocatedCount = 0;
	m_hAccelTable = LoadAccelerators(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDR_ACCELERATOR1));

	m_hSplashBitMap = nullptr;
	m_hBusyCursor = nullptr;
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
		m_hBusyCursor = nullptr;
	}
	if (m_hSplashBitMap) {
		DeleteObject(m_hSplashBitMap);
		m_hSplashBitMap = nullptr;
	}
	s_instance = nullptr;
}

void EditorInterface::terminate()
{
	if (bThisIsInitialized)
	{
		bThisIsInitialized = false;
		EditorShutdownTrace("[EDITOR_SHUTDOWN] phase=terminate_enter menus=%p menusAllocatedCount=%d", menus, menusAllocatedCount);

		if ( curBrush )
			delete curBrush; curBrush = nullptr;

		// menus is NULL whenever addBuildingsToNewMenu()
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
				delete menus[i]; menus[i] = nullptr;
			}

			free( menus );
			menus = nullptr;
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

	int groupCount = pMgr->getBuildingGroupCount( );

	const char** pNames = ( const char** )malloc( sizeof( const char *) * groupCount );
	pMgr->getBuildingGroupNames( pNames, groupCount );

	menus = (CMenu**) malloc( sizeof( CMenu* ) * (groupCount + 2) );
	// record exactly how many entries were allocated
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

void EditorInterface::handleNewMenuMessage( long specificMessage )
{
	if ( !eye || !eye->active )
		return;

	if (EditorInterface::instance()->ObjectSelectOnlyMode())
	{
		PlaySound("SystemDefault",nullptr,SND_ASYNC);
		return;
	}

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
			case ID_MISSION_REFRESH_DEFINITIONS: { refreshMissionDefinitionsFromCatalog(); } break;
			case ID_MISSION_AUTHOR_CAMERA_ZOOM: { authorMissionCameraZoom(); } break;
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
		PlaySound("SystemDefault",nullptr,SND_ASYNC);
	}
	return true;
}

int EditorInterface::FileOpen()
{
	int res = PromptAndSaveIfNecessary();
	if (IDCANCEL == res) {
		return false;
	}

	CFileDialog fileDlg( 1,  "pak", nullptr, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR, szPAKFilter );
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
		PlaySound("SystemDefault",nullptr,SND_ASYNC);
		UnsetBusyMode();
	}

	return true;

}


void EditorInterface::OpenMissionByPath( const char* pakPath )
{
	if ( !pakPath || !pakPath[0] )
		return;
	SetBusyMode();
	EditorData::initTerrainFromPCV( pakPath );
	tacMap.UpdateMap();
	syncScrollBars();
	PlaySound("SystemDefault",nullptr,SND_ASYNC);
	UnsetBusyMode();
}
