// Editor: left-button/mouse-move handling, object picking and the WM_ mouse handlers.
// Declared in the class headers; shared names in EditorInterface_internal.h.
#include "EditorInterface_internal.h"
#include "Gates/gate.h"
#include "../RenderCore/axis_convert.h"



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
		// Measurement confirmed the FORWARD projection
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

	((MainFrame*)AfxGetMainWnd())->m_wndDlgBar.GetDlgItem( IDC_COORDINATES_EDIT )->SetWindowText( buffer2 );

	char buffer3[256];
	sprintf( buffer3, "%.3f", vector.z );

	((MainFrame*)AfxGetMainWnd())->m_wndDlgBar.GetDlgItem( IDC_ALTITUDE_EDIT )->SetWindowText( buffer3 );

	vector -= lastClickPos;
	float distance = vector.GetLength();

	char buffer[256];
	sprintf( buffer, "%.3f", distance );

	((MainFrame*)AfxGetMainWnd())->m_wndDlgBar.GetDlgItem( IDC_DISTANCE_EDIT )->SetWindowText( buffer );

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
			mc2ToRender(objPosition, pAppearance->position);
	
			Stuff::Vector3D Distance;
			Distance.Subtract(objPosition,eyePosition);
			eyeDistance = Distance.GetApproximateLength();
		}
	}	

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

extern bool gActive;
extern bool gGotFocus;
extern Editor* editor;

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
	static const bool s_enabled = mc2::gate::get<"MC2_EDITOR_GPU_PICK">();   // Lead0Kill row, default on
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

EditorObject* EditorInterface_PickObjectAtScreenPoint(int screenX, int screenY)
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

bool EditorInterface_SelectObjectAtScreenPoint(int screenX, int screenY, bool toggle)
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
	if (middleClicked)
	{
		eye->tiltNormal();
		eye->ZoomNormal();
	}	

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
