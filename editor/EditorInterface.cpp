/***************************************************************
* FILENAME: EditorInterface.cpp
* DESCRIPTION: Implements the MFC Editor interface, input routing, and UI shell.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 05/16/2026
* MODIFICATION: by Methuselas
* CHANGES: Remove temporary Editor init/dialog trace calls after port validation.
****************************************************************/

#include "EditorInterface_internal.h"
#include "../RenderCore/axis_convert.h"
#include "Gates/gate.h"


// ---------------------------------------------------------------------------
// EDITOR-EXIT-CRASH-1 shutdown breadcrumbs.
// Mirrors EditorData.cpp's EditorDataTrace: always feeds the crashbundle ring
// buffer (so the last ~64KB of activity lands in crashes/<ts>/last_trace.txt
// on any CTD, including one during exit), and additionally logs to
// editor-startup.log when MC2_EDITOR_TRACE is set for interactive debugging.
// ---------------------------------------------------------------------------
void EditorShutdownTrace(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	crashbundle_append(buf);

	if (!mc2::gate::get<"MC2_EDITOR_TRACE">())
		return;

	FILE* f = fopen("editor-startup.log", "a");
	if (!f)
		return;
	fputs(buf, f);
	fputc('\n', f);
	fclose(f);
}

IProviderEngine * armProvider;

CameraPtr eye = NULL;
long lightState = 0;
bool s_bSensorMapEnabled = false;
EditorInterface* EditorInterface::s_instance = NULL;

void EditorInterface::handleKeyDown( int Key )
{
	if ( !eye || !eye->active  )
		return;

	if ( DebuggerActive )
		return;

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
			mc2ToRender(objPosition, pAppearance->position);

			Stuff::Vector3D Distance;
			Distance.Subtract(objPosition,eyePosition);
			eyeDistance = Distance.GetApproximateLength();
		}
	}

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
		gos_Free(curDirStr); curDirStr = 0;

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
		static bool   s_probeEnabled = mc2::gate::get<"MC2_TERRAIN_PROBE">();
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

		if ( painting && !dragging )
		{
			GetCursorPos( &pt );
			ScreenToClient( &pt );
			int PosX = (int)pt.x;
			int PosY = (int)pt.y;

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

#pragma warning( default:4244 )
