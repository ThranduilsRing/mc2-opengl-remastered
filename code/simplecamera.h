#ifndef SIMPLECAMERA_H
#define SIMPLECAMERA_H
/*************************************************************************************************\
SimpleCamera.h		: Interface for the camera that renders a single object
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "mc2_czstring.h"
#include"camera.h"

#include <memory>
#include <string>

class ObjectAppearance;

///////////////////////////////

class SimpleCamera : public Camera
{
public:
	SimpleCamera();
	~SimpleCamera();

	void setMech( mc2::czstring fileName, long base = 0xffff7e00, long highlight = 0xffff7e00, long h2 = 0xffbcbcbc );
	void setComponent( mc2::czstring fileName );
	void setBuilding( mc2::czstring fileName );
	void setVehicle( mc2::czstring vehicle, long base = 0xffff7e00, long highlight = 0xffff7e00, long h2 = 0xffbcbcbc );
	ObjectAppearance* getObjectAppearance() const { return pObject.get(); }

	void init( float left, float right, float top, float bottom );

	void setObject( mc2::czstring fileName, long type, long base = 0xffff7e00, long highlight = 0xffff7e00, long h2 = 0xffbcbcbc );

	void render() override;
	virtual void render(long xOffset, long yOffset);
	long update() override;
    void drawAnimationPanel();

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

	bool hasObject() const { return pObject != nullptr; }
	bool fitting() const { return framed_ && fitIters_ > 0; }
	// Mech-lab framing: the model fills fillFrac of the rect height with its feet near the bottom, turned yaw
	// degrees and seen from pitch degrees, lit by key + fill + rim. Distance and offset converge over a few
	// frames from the drawn height and full-turn horizontal radius. The camera keeps its own
	// distance: Camera::AltitudeTight is static and a second camera on the
	// same screen (the component picture) rewrites it.
	void setFramedPose( float fillFrac, float yaw, float pitch )
	{ fillFrac_ = fillFrac; framedPitch_ = pitch; rotation = swayBase_ = fitYaw_ = yaw; swayPhase_ = 0.f; framed_ = true; framedFov_ = 40.f; fudgeX = fudgeY = 0.f; fitIters_ = 40; haveDrawn_ = false; haveGround_ = false; turntableLastT_ = 0.0; maxHalf_ = turnAccum_ = 0.f; widens_ = 0; }
	// Mech-lab idle turntable (about 10 deg/s on a framed pose); paused while the player drags a component.
	void setTurntablePaused( bool paused ) { turntablePaused_ = paused; }
	void setScale( float newScale );
	void setRotation( float rotation );
	void zoomIn( float howMuch ); // scale for things that can't 

	void rotateBy(float degrees) { swayBase_ += degrees; rotation += degrees; if (rotation > 360.f) rotation -= 360.f; if (rotation < -360.f) rotation += 360.f; }
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
        oldCam = nullptr;
        bContextNotSet = true;
    }

	float		bounds[4];


private:

	std::unique_ptr<ObjectAppearance>	pObject;	// B7o.5: owned; released only through releaseObject()
	std::string modelCredit_;
	void releaseObject();
	Camera*				oldCam;
	float				rotation;
	float				rotationIncrement;
	float				fudgeX;
	float				fudgeY;
	bool				bIsComponent;
	bool				bIsInMission;
	float				shapeScale;
	bool				drawOffscreen_ = false;   // PREVIEW-FBO-FIXED-800x600-1
	float				fillFrac_ = 0.f, framedPitch_ = 0.f, framedAlt_ = 650.f, framedFov_ = 40.f;
	float				turnRadius_ = 0.f, turnHalfHeight_ = 0.f;
	float				maxHalf_ = 0.f, turnAccum_ = 0.f;
	float				swayBase_ = 0.f, swayPhase_ = 0.f, fitYaw_ = 0.f;   // idle turntable swings +-30 deg about the framed yaw
	int					widens_ = 0;
	std::string			fitChassis_;
	float				drawn_[4] = {};	// last PreviewDraw screen extents (framed only)
	int					fitIters_ = 0;
	bool				fitSweepYawHeld_ = false;	// the last sweep yaw is still on the model: reveal at the base yaw
	bool				fitPresented_ = false;	// the GPU lab posted the backdrop-only fit frame
	bool				framed_ = false, haveDrawn_ = false;
	bool				turntablePaused_ = false;
	double				turntableLastT_ = 0.0;	// steady-clock seconds of the previous turntable step (0 = none)
	float				groundC_[2] = {}, groundH_ = 0.f;	// smoothed mech feet centre / height (panel logical px) for the contact shadow
	bool				haveGround_ = false;

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
