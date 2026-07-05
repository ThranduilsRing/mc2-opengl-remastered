#ifndef SIMPLECAMERA_H
#define SIMPLECAMERA_H
/*************************************************************************************************\
SimpleCamera.h		: Interface for the camera that renders a single object
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"camera.h"

class ObjectAppearance;

///////////////////////////////

class SimpleCamera : public Camera
{
public:
	SimpleCamera();
	~SimpleCamera();

	void setMech( const char* fileName, long base = 0xffff7e00, long highlight = 0xffff7e00, long h2 = 0xffbcbcbc );
	void setComponent( const char* fileName );
	void setBuilding( const char* fileName );
	void setVehicle( const char* vehicle, long base = 0xffff7e00, long highlight = 0xffff7e00, long h2 = 0xffbcbcbc );
	ObjectAppearance* getObjectAppearance() const { return pObject; }

	void init( float left, float right, float top, float bottom );

	void setObject( const char* fileName, long type, long base = 0xffff7e00, long highlight = 0xffff7e00, long h2 = 0xffbcbcbc );

	virtual void render();
	virtual void render(long xOffset, long yOffset);
	virtual long update();

	// PREVIEW-FBO-FIXED-800x600-1: opt a defs/ImGui page's use of this camera into
	// rendering the mech preview into a fixed 800x600 offscreen texture instead
	// of directly to the real screen (see SimpleCamera::render()). ONLY set this
	// for screens that actually composite the result via drawPreviewToPanel()
	// below (a real ImGui panel is present, e.g. Mechlopedia's mcl_en_mechs.fit
	// Gui3DView) -- screens still on the fully-legacy path (no defs/ImGui page,
	// e.g. Mech Bay as of this pass) must leave this false/default so their
	// existing direct-to-screen draw is unchanged.
	void setPreviewOffscreen( bool v ) { drawOffscreen_ = v; }

	// After a render() with setPreviewOffscreen(true) active, composite just
	// this camera's bounds[] sub-rect of the fixed 800x600 offscreen texture
	// into the real-resolution panel at (panelX,panelY,panelW,panelH) (real
	// screen pixels). No-op if setPreviewOffscreen(true) wasn't set.
	void drawPreviewToPanel( float panelX, float panelY, float panelW, float panelH ) const;

	void setScale( float newScale );
	void setRotation( float rotation );
	void zoomIn( float howMuch ); // scale for things that can't 

	void setInMission (void)
	{
		bIsInMission = true;
	}

	void setColors( long base = 0xffff7e00, long highlight = 0xffff7e00, long h2 = 0xffbcbcbc );

    void pushContext() {
		gosASSERT(bContextNotSet);
        oldCam = eye;
        eye = this;
        bContextNotSet = false;
    }
    void popContext() {
		gosASSERT(false == bContextNotSet);
        eye = oldCam;
        oldCam = NULL;
        bContextNotSet = true;
    }

	float		bounds[4];


private:

	ObjectAppearance*	pObject;
	Camera*				oldCam;
	float				rotation;
	float				rotationIncrement;
	float				fudgeX;
	float				fudgeY;
	bool				bIsComponent;
	bool				bIsInMission;
	float				shapeScale;
	bool				drawOffscreen_ = false;   // PREVIEW-FBO-FIXED-800x600-1

    bool                bContextNotSet;

	// Orthographic preview framing: fitScale_ is the ortho size (newScaleFactor)
	// converged each frame to fill the panel from the real projected vertex
	// bounds; fitFrames_ counts down the convergence window after a setXxx.
	float				fitScale_;
	int					fitFrames_;

	void resetPreviewFit()
	{
		fitScale_  = 0.5f;
		fitFrames_ = 30;
	}
};


#endif
