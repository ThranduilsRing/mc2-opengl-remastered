#ifndef UNIT_BRAIN_PANEL_H
#define UNIT_BRAIN_PANEL_H
/***************************************************************
* FILENAME: UnitBrainPanel.h
* DESCRIPTION: Editor "AI / Brain / Orders" inspector panel.
*   Visualizes WHAT the currently-selected unit does: its AI brain script,
*   pilot, lance/squad, and an Orders & Stance section with the route tool
*   (order type, stance, waypoints). Authored routes save as the unit's
*   OrderType/Waypoint keys, which the game admits through
*   Mission::admitAuthoredRoutes.
*   Mirrors the InspectorPanel pattern (own ImGui window, selection-driven,
*   safe with no map / no selection). Does NOT touch save/load or PacketFile.
* DATE: 2026-06-24
****************************************************************/

class UnitBrainPanel
{
public:
	// Panel visibility (own ImGui window; safe with no map / no selection).
	static void Open();
	static void Close();
	static void Toggle();
	static bool IsOpen();
	static bool SmokeProbe();

	// Draw the floating panel each frame from renderToolbarImGui().
	// No-op cheaply when closed.
	static void Draw();

	// Waypoint placement mode. While active, EditorInterface's left-click handler
	// appends the clicked map point to the target unit (by editor id, so a changed
	// selection or deleted unit is handled). Set by the panel's "Add Waypoints"
	// button; auto-ends when the panel closes or the target unit deselects.
	static bool WaypointPlaceActive();
	static long WaypointPlaceTargetId();
	static void BeginWaypointPlace( long unitId );
	static void EndWaypointPlace();
	// Append a clicked map point to the placement-target unit (undoable). Called
	// from EditorInterface::handleLeftButtonDown. Returns true if the click was
	// consumed (placement mode active + target valid).
	static bool HandlePlacementClick( float worldX, float worldY, float worldZ );

	// Route tool gestures while placement mode is active (all edits go through
	// EditorCommands::setRoute, one undo step per gesture):
	//   press on a waypoint        -> select it; dragging moves it
	//   Ctrl + press on a waypoint -> delete that waypoint
	//   Alt + press                -> insert a waypoint into the nearest segment
	//   Shift + press and drag     -> draw a stroke (a point every stroke spacing)
	//   plain press elsewhere      -> append a waypoint
	// Delete (Erase) removes the selected waypoint. Returns true when consumed.
	static bool RoutePress( float worldX, float worldY, float worldZ, bool ctrl, bool alt, bool shift );
	static bool RouteGestureActive();
	static void RouteDrag( float worldX, float worldY, float worldZ );
	static bool RouteRelease();
	static bool RouteDeleteSelected();
	// Selected waypoint index of the placement target (-1 if none), for the overlay.
	static int SelectedWaypoint( long unitId );
};

#endif // UNIT_BRAIN_PANEL_H
