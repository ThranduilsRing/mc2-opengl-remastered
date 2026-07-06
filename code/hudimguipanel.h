#ifndef HUDIMGUIPANEL_H
#define HUDIMGUIPANEL_H
/*************************************************************************************************\
hudimguipanel.h : thin helper for porting legacy in-mission HUD panels to ImGui
                  (IN-MISSION-HUD-IMGUI-SCOPE-1, slice B pattern pieces).

Coordinate / exemption-bracket contract (the load-bearing part):

  Legacy in-mission HUD chrome is authored in Environment.screenWidth/Height
  ("HUD space") and remapped by flushHUDBatch into the centered 16:9 canvas
  (gos_ComputeHudCanvasBox) unless the draw is canvas-exempt. The exemption
  bracket is 3-state (gos_SetHudCanvasExemptMode, gameos.hpp):
    -1 (default) follow scaleExempt (cursor / modal dialogs / world overlays
       stay full-surface), 0 force canvas remap, 1 force exemption.

  An ImGui HUD panel never draws through the HUD batch, so the bracket does
  not apply to it directly. Instead this helper applies the SAME canvas
  transform FORWARD (authored HUD coords -> display pixels -> ImGui logical
  coords), which is exactly what flushHUDBatch does to non-exempt chrome.
  That means a panel ported with beginPanel() behaves like bracket mode 0
  (canvas-remapped chrome). Mouse input needs NO inverse: ImGui hit-tests in
  display space natively, and the panel is positioned in display space.
  Panels that must stay full-surface (bracket 1 semantics — e.g. modal
  dialogs) should NOT use beginPanel's canvas mapping; that case is out of
  scope for slice B (the pause window is canvas-remapped legacy chrome, see
  pausewindow.cpp:100).

  NOTE: the bottom-band s_hud_scale shrink (MC2_HUD_SCALE) is NOT modeled
  here — it only affects draws whose centroid is below 60% screen height.
  The pause window sits above that band (its own hit-test comment calls the
  shrink inverse "a no-op up here"). A future port of bottom-band chrome
  (command bar, tac map) must add that transform before reusing this helper.

Failure contract (pin-to-legacy): if ImGui is unavailable or Begin fails,
beginPanel returns false and pins ALL HudImGui panels to the legacy path for
the rest of the session; callers must render their legacy path whenever
beginPanel returns false.
\*************************************************************************************************/

namespace HudImGui
{
	// Presence-gate check, cached after first call per name (max 8 names).
	// ON = set and not "0".
	bool envGateOn( const char* name );

	// True once any beginPanel has failed; all later beginPanel calls return
	// false immediately (session-wide legacy fallback).
	bool pinnedToLegacy();

	// Map an authored HUD-space point (Environment.screenWidth/Height space,
	// non-exempt chrome) to ImGui logical display coords via the HUD canvas
	// box. Identity-stretch when no canvas is active (16:9 window).
	void hudToDisplay( float hx, float hy, float& outX, float& outY );

	// Begin an ImGui window pinned over the given HUD-space rect (borderless,
	// no move/resize/collapse). Returns false -> caller renders legacy this
	// frame (and the session is pinned to legacy). Must be paired with
	// endPanel() ONLY when it returned true.
	// fontScaleOut (optional): suggested ImGui font scale so ~13px base text
	// matches the panel's HUD pixel density.
	bool beginPanel( const char* id, float hudLeft, float hudTop,
					 float hudRight, float hudBottom, float* fontScaleOut = 0 );
	void endPanel();
}

#endif // HUDIMGUIPANEL_H
