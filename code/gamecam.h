//---------------------------------------------------------------------------
//
// GameCam.h -- File contains the Game camera class definitions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef GAMECAM_H
#define GAMECAM_H
//---------------------------------------------------------------------------
// Include Files

#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

//---------------------------------------------------------------------------
class GameCamera : public Camera
{
	//Data Members
	//-------------
	protected:
		GameObjectPtr			targetObject;
		std::unique_ptr<Appearance>			compass;	// B7o.5: owned; reset in destroy()
		bool					drawCompass;
		
		float					lastShadowLightPitch;

		// render() phases (gamecam.cpp).
		void renderViewUniforms();
		void renderVanishProbe();
		void renderTerrainSceneState();
		void renderTacticalArcOverlay();
		void renderLetterbox();
		void renderFadeQuad();
		void refreshCompass();
		
	public:
	
		void init (void) override
		{
			Camera::init();
			targetObject = nullptr;
			compass = nullptr;
			lastShadowLightPitch = 0.0f;

			DWORD szData = sizeof(drawCompass);
			gos_LoadDataFromRegistry("CompassStatus", &drawCompass, &szData);

			//Check if data not in registry yet.  Goober!
			if (!szData)
				drawCompass = true;
		}
			
		~GameCamera (void)
		{
			destroy();
		}

		void destroy (void) override;

		void render (void) override;
		long activate (void) override;
		
		long update (void) override;
		void prepareCompass();
		void moveLeft(float amount) override
		{
			targetObject = nullptr;
			Camera::moveLeft(amount);
		}
		
		void moveRight(float amount) override
		{
			targetObject = nullptr;
			Camera::moveRight(amount);
		}
		
		void moveUp(float amount) override
		{
			targetObject = nullptr;
			Camera::moveUp(amount);
		}
		
		void moveDown(float amount) override
		{
			targetObject = nullptr;
			Camera::moveDown(amount);
		}
			
		virtual void setTarget (GameObjectPtr target)
		{
			targetObject = target;
			if( target )
			{
				//float newRotation = targetObject->getRotation();
				//setCameraRotation(newRotation,newRotation);
			}
		}

		GameObject* getTarget()
		{
			return targetObject;
		}

		// VK-FIRST-BOOKMARK-POSE-1 (ported for CAMERA-PARK-1): also drop the
		// object-tracking target so GameCamera::update()'s per-frame snap
		// (setPosition(targetObject->getPosition())) cannot re-capture the
		// camera after a parked pose. Base clears lookTargetObject + all
		// goals/velocity.
		void freezeScriptedCameraForCapture (void) override
		{
			Camera::freezeScriptedCameraForCapture();
			targetObject = nullptr;
		}

		void allTight (void) override
		{
			long i=0;
			GameObjectPtr mvt = nullptr;
			while (i<ObjectManager->getNumGoodMovers())
			{
				mvt = ObjectManager->getGoodMover(i);
				if (!mvt->isDestroyed() && !mvt->isDisabled())
					break;
				i++;
			}

			tiltNormal();
			setTarget(mvt);
			ZoomTight();
		}

		void toggleCompass()
		{
			drawCompass ^= true;

			//Save current Compass state to registry.
			// Saves us having to put it into the prefs files.
			DWORD szData = sizeof(drawCompass);
			gos_SaveDataToRegistry( "CompassStatus",  &drawCompass,  szData );
		}
};		

//---------------------------------------------------------------------------
extern CameraPtr eye;		//Global Instance of the current camera

//---------------------------------------------------------------------------
#endif

//---------------------------------------------------------------------------
//
// Edit log
//
//---------------------------------------------------------------------------
