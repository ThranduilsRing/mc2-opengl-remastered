#ifndef UNIT_BRAIN_PANEL_H
#define UNIT_BRAIN_PANEL_H
/***************************************************************
* FILENAME: UnitBrainPanel.h
* DESCRIPTION: Editor "AI / Brain / Orders" inspector panel.
*   Shows the selected unit's AI brain, pilot and squad. Edits patrol routes
*   and stance through the editor undo manager and mission Part persistence.
* DATE: 2026-06-24
****************************************************************/

class Camera;
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
	static bool HandleRoutePress(Camera* eye, int screenX, int screenY);
	static bool RouteDragging();
	static void HandleRouteMove(Camera* eye, int screenX, int screenY);
	static void MoveRoutePoint(float worldX, float worldY, float worldZ);
	static void FinishRouteDrag();
	static void DeleteRoutePoint();
};

#endif // UNIT_BRAIN_PANEL_H
