// Editor: window-message handlers (commands, scroll, paint, update UI, destroy) and the teams undo action.
// Declared in the class headers; shared names in EditorInterface_internal.h.
#include "EditorInterface_internal.h"


afx_msg void EditorInterface::OnCommand(UINT nID) 
{
	EditorInterface::instance()->handleNewMenuMessage( nID );
}

void EditorInterface::UpdateButton( CCmdUI* pButton )
{
	pButton->Enable( true );

	if ( pButton->m_nID == ID_DROPZONES_ADD || pButton->m_nID == ID_DROPZONES_VTOL )
	{
		glm::vec3 pos;
		pos.x = -1; 
		pos.y = -1;
		if ( !EditorObjectMgr::instance()->canAddDropZone( pos, 0, (pButton->m_nID - ID_DROPZONES_ADD) ? true : false ) )
			pButton->Enable( false );
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
			::SetCursor(LoadCursor(nullptr, IDC_WAIT));
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
            nullptr,
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
		prevBrush = nullptr;
		prevSelecting = false;
		prevPainting = false;
		prevDragging = false;
		ChangeCursor( oldCursor );
		syncScrollBars();
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
		// A short click on an object opens its context menu; elsewhere it returns to Select.
		if ( !openObjectContextMenu( point.x, point.y ) )
			Select();
	}
	else
	{
		tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
		syncScrollBars();
	}

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
	glm::vec3 direction;
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

	glm::vec3 eyeDisplacement = eye->getPosition();

	/* maxVisual was taken from Camera::setPosition(). */
	float maxVisual = (Terrain::worldUnitsMapSide / 2) - Terrain::worldUnitsPerVertex;
	float bound = SQRT_2 * maxVisual;
	float amount = bound * 2.0f * ((float)pt.y / VSCROLLBAR_RANGE - 0.5f);

	float amountMore = amount - glm::dot(direction, eyeDisplacement);
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
	EditorObjectMgr::SelectedUnits list;

	EditorObjectMgr::instance()->getSelectedUnits( list );

	if ( static_cast<int>(list.size()) )
	{
		UnitSettingsDlg dlg( list, undoMgr );
		if ( IDOK == dlg.DoModal() )
		{
		}
	}

	EditorObjectMgr::EDITOR_OBJECT_LIST list2 = EditorObjectMgr::instance()->getSelectedObjectList();
	if (static_cast<int>(list2.size()) > static_cast<int>(list.size()))
	{
		for (size_t index = list2.size(); index > 0; )
		{
			--index;
			if (dynamic_cast<Unit*>(list2[index]) || dynamic_cast<::DropZone*>(list2[index]))
				list2.erase(list2.begin() + index);
		}

		if ( static_cast<int>(list2.size()) )
		{
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
	BYTE* pData = nullptr;
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
	pData = nullptr;
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
	EditorObject* pObject = EditorInterface_PickObjectAtScreenPoint(dblX, dblY);
	if (nullptr != pObject)
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
			if (!m_hSplashBitMap) {
				m_hSplashBitMap = (HBITMAP)LoadImage(nullptr, "esplash.bmp", IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
			}
			if (m_hSplashBitMap) {
				CRect rcClient;
				GetClientRect(rcClient);

				BITMAP bm_struct;
				CBitmap* pbmOld = nullptr;
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
	                        ((nullptr != land) || MapGeneratorDialog::IsOpen());
#else
	const bool needRender = bThisIsInitialized && (nullptr != land);
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
		// gosASSERT is a dead no-op in RelWithDebInfo
		// (see mclib review), so gosASSERT(false) here silently did nothing —
		// a null tacMap.m_hWnd at WM_DESTROY was neither logged nor handled.
		// Make the signal live: log it (crashbundle ring + optional file) and
		// continue: tacMap.DestroyWindow() would be a no-op on a null HWND
		// anyway, so there's nothing unsafe left to do here.
		EditorShutdownTrace("[EDITOR_SHUTDOWN] phase=OnDestroy_null_tacmap_hwnd");
	}
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
