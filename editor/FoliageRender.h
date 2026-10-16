//-------------------------------------------------------------------------------------------------
// FoliageRender.h -- editor-side PCG foliage billboard preview.
//
// Loads the generator's {name}.foliage.json sidecar and
// draws each instance as a screen-space, ground-anchored billboard over the
// terrain. Self-contained: owns its own instance list + sprite-texture cache and
// is driven by 4 editor menu commands (Generate / Clear / Toggle / Regenerate).
//
// It NEVER touches terrain/heightmap/colormap state, so the generate->save->load
// terrain contract is unaffected -- foliage is a pure visual overlay.
//-------------------------------------------------------------------------------------------------
#ifndef FOLIAGE_RENDER_H
#define FOLIAGE_RENDER_H

class Camera;

namespace FoliageRender
{
	// Replace the instance list from a foliage.json sidecar. Returns false if the
	// file is missing/unparseable (instances left unchanged on parse failure).
	bool Load( const char* jsonPath );

	// Drop all instances (Clear Foliage command).
	void Clear();

	// Flip preview visibility (Toggle Foliage command).
	void Toggle();
	bool Visible();

	// Current instance count (for status/trace).
	int  Count();

	// Per-frame draw. Call AFTER terrain is rendered, with the active editor
	// camera (projectForScreenXY is a Camera base method). No-op when hidden/empty.
	void Render( Camera* eye );
}

#endif // FOLIAGE_RENDER_H
