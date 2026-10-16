//-------------------------------------------------------------------------------------------------
// EditorDebugOverlay.h -- reusable editor debug/overlay layer.
//
// Draws diagnostic world overlays (chunk grid, superchunk grid, water plane bounds)
// on top of the already-rendered terrain, plus an ImGui control/stats panel.  Built
// to diagnose the mid-zoom water-row artifact: with the grid drawn ON the water plane
// you can read directly whether missing water rows align to chunk rows, superchunk
// rows, or neither.
//
// Pure visual: touches NO terrain/save/mission state.  Follows the FoliageRender
// convention -- project world points to screen with the active Camera and submit
// screen-space gos primitives -- and restores the render state it changes.
//
//   chunk      = 20 terrain cells (1 cell = 1 vertex step = worldUnitsPerVertex)
//   superchunk = N chunks (default 3) = 60 cells
//-------------------------------------------------------------------------------------------------
#ifndef EDITOR_DEBUG_OVERLAY_H
#define EDITOR_DEBUG_OVERLAY_H

class Camera;

namespace EditorDebugOverlay
{
	// Draw enabled world overlays.  Call inside the editor render frame (same place
	// FoliageRender::Render is called), AFTER terrain is drawn.  No-op if nothing is
	// enabled or no terrain is loaded.  Safe with a null camera.
	void RenderWorldOverlay( Camera* eye );

	// Draw the selected unit's patrol/move path (team-color dashed line + dots +
	// direction arrows). No-op unless the AI/Brain/Orders panel is open. Call in
	// the same render frame as RenderWorldOverlay.
	void RenderPatrolPaths( Camera* eye );

	// Run the terrain height/water probe once and emit the machine-readable
	// "TERRAIN_PROBE ..." line to the trace log. Headless-safe (no ImGui): lets a
	// -gen-map smoke run capture same-session before/after evidence. No-op if no
	// terrain is loaded.
	void RunProbeOnce();

#ifdef MC2_IMGUI
	// Draw the "Debug Overlays" ImGui panel (toggles + tunables + stats).
	void RenderImGui();
#endif
}

#endif // EDITOR_DEBUG_OVERLAY_H
