//------------------------------------------------------------------------------
// BeautySidecarPreview
//
// Loads a terrain "beauty sidecar" (offline per-vertex elevation delta produced
// by tools/terrain_beautify/mission_sidecar.py) and applies it to the LIVE editor
// terrain so the artist can preview a beautified mission, then restore the stock
// terrain. Reuses the HeightBrush edit path (setVertexHeight + calcLight +
// refreshTerrainAfterEdit) so the GPU-direct terrain re-bakes correctly.
//
// Sidecar on disk:  <missionName>.beauty/height_delta.r32  (float32 side*side,
// row-major, world-unit deltas — same layout the Python tool writes).
//
// Non-destructive: snapshots the original elevations on first Apply and Restore
// swaps them back. Never writes any file.
//------------------------------------------------------------------------------
#ifndef BEAUTY_SIDECAR_PREVIEW_H
#define BEAUTY_SIDECAR_PREVIEW_H

namespace BeautySidecarPreview
{
	// Load <mission>.beauty/height_delta.r32 and apply to the live terrain.
	// Returns true on success; status string updated either way.
	bool Apply();

	// Swap the original (pre-apply) elevations back. No-op if not applied.
	void Restore();

	bool        IsApplied();
	int         ChangedCount();
	const char* Status();

	// ImGui section for the editor "Tools" panel (Apply / Restore + status).
	void DrawImGui();

	// One-shot env-gated auto-apply (MC2_EDITOR_BEAUTY_AUTOAPPLY) so the headless
	// editor smoke exercises the path. Safe to call every frame.
	void MaybeAutoApply();

	// --- B7b delta heatmap preview ---------------------------------------------
	// Load the sidecar delta WITHOUT mutating terrain (for the heatmap overlay).
	bool LoadDeltaForPreview();
	// Loaded delta (row-major side*side world-unit deltas) for the overlay to draw.
	bool         HasDelta();
	int          DeltaSide();
	float        DeltaMaxAbs();
	const float* DeltaData();   // size DeltaSide()^2, or null
	// Heatmap toggle (driven by the Tools-panel checkbox, read by EditorDebugOverlay).
	bool ShowHeatmap();

	// --- B7c protected-zone overlay --------------------------------------------
	// protected.r8: per-cell level (2=structural hard, 1=water, 0=editable).
	bool                 HasProtected();
	int                  ProtectedSide();
	const unsigned char* ProtectedData();   // size ProtectedSide()^2, or null
	bool                 ShowProtected();
}

#endif // BEAUTY_SIDECAR_PREVIEW_H
