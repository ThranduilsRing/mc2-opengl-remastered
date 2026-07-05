#ifndef EDITORINTERFACE_H
#define EDITORINTERFACE_H
//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma warning( disable : 4786 )


#ifndef MCLIB_H
#include "mclib.h"
#endif

#ifndef ACTION_H
#include "Action.h"
class EditorObject;
class ModifyBuildingAction;
#endif

#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif

#ifndef EDITORDATA_H
#include "EditorData.h"
#endif

#ifndef EDITORTACMAP_H
#include "EditorTacMap.h"
#endif

#include "Objective.h"

#include "stdafx.h"

#ifdef MC2_IMGUI
#include "MapGeneratorDialog.h"
#endif

// forward declarations
class DlgFileOpen;
class Brush;
class MainMenu;
// class Menu; // wlib Menu removed - Editor uses MFC CMenu

// global resource handle
extern HSTRRES gameResourceHandle;
extern bool DebuggerActive;

// ARM
namespace Microsoft
{
	namespace Xna
	{
		namespace Arm
		{
			struct IProviderEngine;
			struct IProviderAsset;
		}
	}
}

extern Microsoft::Xna::Arm::IProviderEngine * armProvider;


//--------------------------------------------------------------------------------------
//
// Mech Commander 2 -- Copyright (c) 1998 FASA Interactive
//
// the EditorInterface class handles all the messages for the editor
//
// the Editor class merely holds everything needed to make this go.
//
// 
//--------------------------------------------------------------------------------------

// this class handles and routes all messages...
class EditorInterface : public CWnd
{
private:
	
	

public:

	static EditorInterface* instance(){ return s_instance; }
	// EDITOR-CRASH-HARDENING-1: called from the object-delete broadcast
	// (EditorObjectMgr::deleteBuilding/deleteSelectedObjects) so a delete that
	// happens while a drag pointer is held cannot leave m_pDragObject dangling
	// (UAF: set ~:1632, derefed ~:1764/1855/1864). Mirrors the selection-cache
	// scrub (selectedObjects.RemoveIfThere) on the same delete path.
	void notifyObjectDeleted( const EditorObject* pObj );
	// Open a mission by explicit .pak path (same load path as FileOpen's success branch,
	// minus the file dialog). Used by the Mod Project "Import Mission" flow.
	void OpenMissionByPath(const char* pakPath);
	void renderToolbarImGui();
	void renderObjectCompanionPanel();
	void renderObjectInfoPanel();

	// Modder-friendly wrapper panels (Phase 1d). Each surfaces EXISTING editor
	// state/flows through ImGui; no new placement/launch/package systems.
	void renderPlacePanelImGui();    // scatter mode + params + active brush + clear
	void renderMissionToolsImGui();  // Test Mission (launch game) + Build Mod Package
	void setSculptBrush( int mode );
	void setStampBrush( int type );
	// TERRAIN-MATERIAL-PAINT-1 (Slice 1): surface the dormant TerrainBrush. Installs
	// a TerrainBrush painting the given TerrainType via the single-source setActiveBrush
	// path (same as setSculptBrush/setStampBrush). 'terrainType' is a TerrainType enum
	// value (mclib/dmapdata.h); brushId/menuId use a synthetic-negative range.
	void setPaintMaterialBrush( int terrainType );
	void renderTerrainSelection();

	// Editable Inspector v1: set one object's world XY + absolute yaw through the
	// existing ModifyBuildingAction undo path (same mechanism as drag-move /
	// rotateSelectedObjects). Z stays terrain-locked. Returns false (no-op) for
	// null / no-appearance / forest-member objects. Pushes ONE undoable action
	// and marks the mission dirty.
	bool applyObjectTransform( EditorObject* obj, float worldX, float worldY, float yawDegrees );

	// Inspector edit: change an object's team/alignment (newTeam in [-1,7]; -1 =
	// Neutral). Pushes ONE undoable ModifyBuildingAction (its snapshot already
	// captures teamId) + marks the mission dirty + re-bakes the static recipe so
	// the team colour updates. Returns false (no-op) for null/no-appearance/
	// forest-member objects, an out-of-range team, or an unchanged team.
	bool applyObjectAlignment( EditorObject* obj, int newTeam );

	// Smoke-only (-smoke-inspector-edit): place a throwaway drop zone, transform it
	// via applyObjectTransform AND change its team via applyObjectAlignment, then
	// undo through the same undo manager. Returns a bitmask: bit0 = transform moved
	// the object, bit1 = undo restored it, bit2 = team change applied, bit3 = undo
	// restored the team. Returns -1 if setup failed (no terrain / object-mgr).
	int runInspectorEditSmoke();

	// Smoke-only (-smoke-place-oob): activate a BuildingBrush and drive its
	// update() at off-map screen points to exercise the worldToCell OOB clamp.
	// Returns 1 if all updates survived, -1 if setup failed. Read-only.
	int runPlaceOobSmoke();

	// Frame/focus the camera on the current selection: recenters the camera's
	// ground anchor on the centroid of the selected objects' XY positions.
	// Read-only — no object mutation, no undo entry, no mission-dirty. No-op
	// when nothing is selected. Driven by the Scene Outliner (double-click) and
	// the 'F' hotkey. Z stays terrain-locked (derived by Camera::setPosition).
	static void frameSelectedObjects();

	// Object placement: shared by the Objects menu and the companion panel.
	bool selectBuildingObject( int group, int indexInGroup );
	int  currentAlignmentFromMenu();
	int  objectMessageId( int group, int indexInGroup );
	static const class ObjectRecentRing& objectRecentRing();
	
	EditorInterface();
	~EditorInterface();

	void handleNewMenuMessage( long specificMessage );

	void init( const char* fileName );

	void terminate();

	void ChangeCursor( int ID );

	int MissionSettings();

	int Team( int team );
	int Player( int player );
	/* When in "ObjectSelectOnlyMode", the interface is put in selection mode and all
	features are disabled except those pertaining to selection of objects. This mode is
	engaged from the objectives dialog. */
	bool ObjectSelectOnlyMode() { return bObjectSelectOnlyMode; }
	void ObjectSelectOnlyMode(bool val) { bObjectSelectOnlyMode = val; }
	CObjectivesEditState objectivesEditState;	/* persistent storage for the objective(s) dialog */
	void SelectionMode() { Select(); }
	
	int RefractalizeTerrain( long threshold );

	virtual void handleLeftButtonDown( int PosX, int PosY ); // mouse button down
	virtual void handleLeftButtonDbl( int PosX, int PosY ){} // mouse button dbl click
	virtual void handleLeftButtonUp( int PosX, int PosY ); // pop ups etc need this
	virtual void handleKeyDown( int Key );
	virtual void handleMouseMove( int PosX, int PosY );
	void updateCameraInput();   // per-frame camera scroll/zoom/rotate from keys + edge-scroll

	void update (void);

	virtual void render();
	void initTacMap();
	void updateTacMap() { tacMap.UpdateMap(); }

	void syncHScroll();
	void syncVScroll();
	void syncScrollBars() { syncHScroll(); syncVScroll(); }

	long Width(){  RECT tmp; GetWindowRect( &tmp ); return tmp.right - tmp.left; }
	long Height() { RECT tmp; GetWindowRect( &tmp ); return tmp.bottom - tmp.top; }

	void SetBusyMode(bool bRedrawWindow = true);
	void UnsetBusyMode();

	bool SafeRunGameOSLogic();

	bool ThisIsInitialized() { return this->bThisIsInitialized; }

	afx_msg void UpdateButton( CCmdUI* button );

	int Quit();
	int Save();
	int SaveAs();
	int QuickSave();
	int PromptAndSaveIfNecessary();

	ActionUndoMgr				undoMgr;

		//{{AFX_VIRTUAL(EditorInterface)
	public:
	virtual BOOL PreTranslateMessage(MSG* pMsg);
	protected:
	virtual LRESULT WindowProc(UINT message, WPARAM wParam, LPARAM lParam);
	virtual BOOL PreCreateWindow(CREATESTRUCT& cs);
	//}}AFX_VIRTUAL

	protected:
		//{{AFX_MSG(EditorInterface)
		afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
	afx_msg void OnKeyUp(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg void OnRButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
	afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
	afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnSysKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
	afx_msg void OnPaint();
	afx_msg void OnViewRefreshtacmap();
	afx_msg void OnUpdateMissionPlayerPlayer3(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionPlayerPlayer4(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionPlayerPlayer5(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionPlayerPlayer6(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionPlayerPlayer7(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionPlayerPlayer8(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionTeamTeam3(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionTeamTeam4(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionTeamTeam5(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionTeamTeam6(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionTeamTeam7(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMissionTeamTeam8(CCmdUI* pCmdUI);
	afx_msg void OnDestroy();
	afx_msg void OnForestTool();
	afx_msg void OnOtherEditforests();
	afx_msg void OnFoliageGenerate();
	afx_msg void OnFoliageRegenSel();
	afx_msg void OnFoliageClear();
	afx_msg void OnFoliageToggle();
	afx_msg void OnViewOrthographiccamera();
	afx_msg void OnViewShowpassabilitymap();
	afx_msg void OnMButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnTimer(UINT_PTR nIDEvent);   // render-pump timer (freeze fix)
	//}}AFX_MSG
		DECLARE_MESSAGE_MAP()

	enum { kRenderTimerId = 0xED20 };          // WM_TIMER id for the render pump

	afx_msg void OnCommand(UINT nID);
	

private:

	// Message handlers
	int Undo();
	int Redo();	
	int FileOpen();
	int New();
	int NewGeneratedMission();
	int PaintDirtRoad();
	int PaintRocks();
	int PaintPaved();
	int PaintTwoLaneDirtRoad();
	int PaintDamagedRoad();
	int PaintRunway();
	int PaintBridge();
	int PaintDamagedBridge();
	int Erase();
	int Select();
	int Flatten();
	int Fog();
	int PurgeTransitions();
	int ShowTransitions();
	int Light();
	int AssignElevation();

	int paintBuildings( int message );
	int PaintTerrain( int type );
	int PaintOverlay( int type, int message );

	int NewHeightMap();
	int SaveCameras();
	int SelectSlopes();
	int SelectAltitude();
	int SelectTerrainType();
	int Waves();
	int SaveHeightMap();

	int DragSmooth();
	int DragRough();

	int SmoothRadius();
	int Alignment( int specific );
	int Damage( bool bDamage );
	int Link( bool bLink );
	int LayMines();
	int SelectDetailTexture();
	int SelectWaterTexture();
	int SelectWaterDetailTexture();
	int TextureTilingFactors();
	int ReloadBaseTexture();
	int SetBaseTexture();
	int DropZone( bool bVTol );
	int UnitSettings( );
	
	int SetSky (long skyId);
	
	int CampaignEditor();

	// helpers
	void KillCurBrush();

	// EDITOR-SIMPLIFY-S1: single source of truth for the active tool. Tears down the
	// old brush (KillCurBrush) and sets curBrush/currentBrushID/currentBrushMenuID in
	// one place. Call sites keep their own ChangeCursor() and any site-specific logic.
	// menuId convention: pass the toolbar tools[].cmdId for tools that have a toolbar
	// entry (so the highlight matches); for tools with no toolbar entry, pass brushId
	// (a stable sentinel — the highlight check never matches a non-toolbar id).
	void setActiveBrush( Brush* newBrush, int brushId, int menuId );

	void addBuildingsToNewMenu();

	void rotateSelectedObjects( int direction );
	void rotateSelectedObjectsDegrees( float deg );

	//-------------------------------------------
	// Data to control scroll, rotation and zoom
	float						baseFrameLength;
	
	float						zoomInc;
	float						rotationInc;
	float						scrollInc;
	
	float						screenScrollLeft;
	float						screenScrollRight;
	float						screenScrollUp;
	float						screenScrollDown;
	
	float						realRotation;
	float						degPerSecRot;

	Brush*						curBrush;
	Brush*						prevBrush;
	bool						prevSelecting;
	bool						prevPainting;
	bool						prevDragging;
	int							oldCursor;
	bool						painting;
	bool						selecting;
	bool						dragging;
	bool						highlighted;

	int							currentBrushID;
	int							currentBrushMenuID;

	// Object drag-move (IDS_SELECT): grab on press, drag to move, finalize on release.
	EditorObject*				m_pDragObject;
	bool						m_dragObjMoved;
	ModifyBuildingAction*		m_pDragAction;
	int							m_dragStartX;
	int							m_dragStartY;
	Stuff::Vector3D				m_dragStartWorld;
	Stuff::Vector3D				m_dragObjStartPos;
	// last cursor screen pos during a drag; the per-frame screen->world delta uses
	// a LOCALLY-sampled map (inverseProject snaps to the 128-unit grid, and a single
	// grab-time map overshoots as world-units-per-pixel changes across the screen).
	int							m_dragLastScreenX, m_dragLastScreenY;
	float						m_sculptRadius;
	float						m_sculptStrength;
	bool						m_scatterMode;     // object brush scatters in a radius vs single place
	float						m_stampRadius;
	float						m_stampStrength;
	int							m_paintMaterialType;   // active Paint-Material TerrainType (for button highlight)
	float						m_paintMaterialSize;   // Paint-Material brush Size (reserved; TerrainBrush is single-vertex in Slice 1)
	float						m_waterHeight;     // live water-elevation slider value
	bool						m_pendGenerateMission; // deferred: run generator outside the ImGui pass

	MainMenu					*m_pMainMenu;

	int							smoothRadius;
	bool						bSmooth;

	static EditorInterface*		s_instance;
	Stuff::Vector3D				lastClickPos;

	HCURSOR						hCursor;
	int							curCursorID;
	int							lastX;
	int							lastY;
	long						lastKey;
	bool 						bObjectSelectOnlyMode;
	CMenu**						menus;
	// EDITOR-EXIT-CRASH-1 (#2): number of entries actually allocated into
	// menus[] by addBuildingsToNewMenu() (groupCount+2 at alloc time). menus
	// is NULL until that call runs (it's skipped when land==NULL at init()
	// time, e.g. the map-generator path). terminate() must free exactly this
	// many entries, and only when menus != NULL — NOT re-query
	// getBuildingGroupCount() at teardown time, which walks OOB/garbage when
	// menus was never allocated or the group count has since changed.
	int							menusAllocatedCount;

	EditorTacMap				tacMap;
	HACCEL						m_hAccelTable;

	bool						rightDrag;
	unsigned long				lastRightClickTime;

	HBITMAP m_hSplashBitMap;
	HCURSOR m_hBusyCursor;
	int m_AppIsBusy;
	bool m_bInRunGameOSLogicCall;
	bool bThisIsInitialized;
};



class Editor // simply holds everything else
{
public:

	EditorObjectMgr					objectMgr;
	EditorData						data;

	~Editor(){ destroy(); }

	void destroy (void);

	void init( char* loader );
	
	void render();
	
	void update();

	void resaveAll();		//Used by autoBuild to automagically resave all maps with correct data.	
};


class TeamsAction : public Action
{
public:
	TeamsAction() : Action() {}
	TeamsAction(const CTeams &teams) : Action() { PreviousTeams(teams); }
	virtual ~TeamsAction() {}
	virtual bool redo() { return undo(); }
	virtual bool undo();
	CTeams PreviousTeams();
	void PreviousTeams(const CTeams &teams);

private:
	CTeams m_previousTeams;
};

#endif
