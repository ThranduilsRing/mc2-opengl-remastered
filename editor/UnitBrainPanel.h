#ifndef UNIT_BRAIN_PANEL_H
#define UNIT_BRAIN_PANEL_H
/***************************************************************
* FILENAME: UnitBrainPanel.h
* DESCRIPTION: Editor "AI / Brain / Orders" inspector panel.
*   Visualizes WHAT the currently-selected unit does: its AI brain script,
*   pilot, lance/squad, and a scaffolded Orders & Stance section (patrol
*   waypoints, attitude/ROE). Read-only display of what EXISTS today; the
*   order/waypoint/stance EDITING controls are present as "this is where this
*   goes" scaffolding, disabled until editor-side order persistence lands and
*   the in-flight TECHSCRIPT brain dispatch (MC2_BRAIN_DISPATCH*, default-OFF)
*   defines the runtime order delivery + precedence.
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
};

#endif // UNIT_BRAIN_PANEL_H
