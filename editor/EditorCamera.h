#include "Gates/gate.h"
#ifndef EDITORCAMERA_H
#define EDITORCAMERA_H
/*************************************************************************************************\
EditorCamera.h			: Interface for the EditorCamera component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef CAMERA_H
#include "Camera.h"
#endif

#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif

#ifndef OBJSTATUS_H
#include "objstatus.h"
#endif

#include "../EditorBridge/EditorRenderBridge.h"  // GPU batcher/registry lifecycle (firewall)
#include <GL/glew.h>                              // GLint/GL_VIEWPORT/glGetIntegerv for the
                                                  // throttled [PICKW] viewport diagnostics.
#include "../GameOS/gameos/view_uniforms_gl.h"  // setCurrentView + uploadViewUniforms
#include "EditorGpuTimer.h"                      // MC2_EDITOR_GPU_TIMERS per-pass GPU timing
#include "AuthoredMissionState.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Throwaway diagnostic helper: throttled trace to editor-startup.log,
// gated on MC2_EDITOR_TRACE. Header is included by exactly one TU
// (EditorInterface.cpp) so file-scope static linkage is safe. ASCII only.
static void EditorCameraTrace(const char* fmt, ...)
{
	if (!mc2::gate::get<"MC2_EDITOR_TRACE">())
		return;
	FILE* f = fopen("editor-startup.log", "a");
	if (!f)
		return;
	va_list args;
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);
	fputc('\n', f);
	fclose(f);
}


/**************************************************************************************************
CLASS DESCRIPTION
EditorCamera:  draws the terrain and objects and stuff
**************************************************************************************************/
extern bool useShadows;
extern bool s_bSensorMapEnabled;
extern bool drawOldWay;
extern bool useFog;

extern MidLevelRenderer::MLRClipper * theClipper;

extern bool useLOSAngle;	// NS3: def in mclib/camera.cpp (editor never reads it)

// S-CLI globals defined in EditorMFC.cpp; consumed at end of render() below
// to post WM_CLOSE once we've rendered the requested number of frames.
extern long g_cliFramesLimit;
extern long g_cliFrameCounter;
extern bool g_cliClosePosted;

class EditorCamera : public Camera
{
public:
		// Mission zoom settings belong to the document; viewport initialization
		// and navigation must not replace them with renderer-derived values.
		struct MissionView
		{
			float scale = 0.5f;
			float zoomMaximum = 2.0f;
			bool operator==(const MissionView&) const = default;
		};
		std::optional<MissionView> missionView;
		bool readMissionView(FitIniFile& file);
		bool saveMissionView(FitIniFile* file);
		MissionView currentMissionView() const;

		AppearancePtr			compass;
		AppearancePtr			theSky;
		bool					drawCompass;
		float					lastShadowLightPitch;
		long					cameraLineChanged;
		long					oldSkyNumber;

		EditorCamera (void)
		{
			init();
		}

		virtual void init (void)
		{
			Camera::init();
			missionView.reset();
			compass = NULL;
			drawCompass = true;
			cameraLineChanged = 0;
			theSky = NULL;
			// Zoom range is set per-frame in render() (Camera::init resets these).
		}

	virtual void reset (void)
	{
		//Must toss these between map loads to clear out their texture memory!!
		delete compass;
		compass = NULL;
		
		delete theSky;
		theSky = NULL;
	}
	
 	virtual void render (void)
	{
		// Editor zoom range. Re-applied every frame because Camera::init() resets
		// these statics to the game's small values (6000/6400); a 1020 map is
		// ~130k world-units wide, so the ceiling must be well above that to frame
		// it. Editor is a separate process so the game camera is unaffected. Far
		// plane scales with altitude (close stays precise, far reaches the ceiling).
		Camera::AltitudeMaximumLo = 200000.0f;
		Camera::AltitudeMaximumHi = 210000.0f;
		Camera::AltitudeMinimum   = 20.0f;
		Camera::MinFarPlane       = 61500.0f;
		Camera::MaxFarPlane       = 500000.0f;

		// Diagnostic probe: throttled per-frame trace of EditorCamera::render
		// entry. Active/turn gate inside this function may skip the bulk of
		// rendering when (active && turn > 1) is false; logging entry tells
		// us whether render() is even being driven.
		static long s_ecrFrame = 0;
		++s_ecrFrame;
		const bool s_ecrLog = (s_ecrFrame == 1 || s_ecrFrame == 5 || s_ecrFrame == 30 || s_ecrFrame == 120);
		if (s_ecrLog)
			EditorCameraTrace("EditorCamera::render frame=%ld enter active=%d turn=%ld drawOldWay=%d",
				s_ecrFrame, active ? 1 : 0, (long)turn, drawOldWay ? 1 : 0);


		gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
		//--------------------------------------------------------
		// S2.13-surgical: screenResolution + calculateProjectionConstants
		// formerly set here. Hoisted to EditorInterface::update() BEFORE
		// land->geometry() so terrain cull constants are fresh when
		// consumed. Local viewMulX/Y/AddX/Y are still used below for
		// TG_Shape::SetViewport and globalScaleFactor.
	
		TG_Shape::SetViewport(viewMulX,viewMulY,viewAddX,viewAddY);
	

		globalScaleFactor = getScaleFactor();
		globalScaleFactor *= viewMulX / Environment.screenWidth;		//Scale Mechs to ScreenRES
		
		DWORD lightRGB = (ambientRed<<16)+(ambientGreen<<8)+ambientBlue;
			
		eye->setLightColor(1,lightRGB);
		eye->setLightIntensity(1,1.0);
	
		MidLevelRenderer::MLRState default_state;
		default_state.SetBackFaceOn();
		default_state.SetDitherOn();
		default_state.SetTextureCorrectionOn();
		default_state.SetZBufferCompareOn();
		default_state.SetZBufferWriteOn();
	
		default_state.SetFilterMode(MidLevelRenderer::MLRState::BiLinearFilterMode);
		
		float z = 1.0f;
		Stuff::RGBAColor fColor;
		fColor.red = (float)((fogColor >> 16) & 0xff);
		fColor.green = (float)((fogColor >> 8) & 0xff);
		fColor.blue = (float)((fogColor) & 0xff);
	
		MidLevelRenderer::PerspectiveMode = usePerspective;
		theClipper->StartDraw(cameraOrigin, cameraToClip, fColor, &fColor, default_state, &z);
		MidLevelRenderer::GOSVertex::farClipReciprocal = (1.0f-cameraToClip[2][2])/cameraToClip[2][3];

		if (s_ecrLog)
			EditorCameraTrace("EditorCamera::render frame=%ld gate(active && turn>1)=%d",
				s_ecrFrame, (active && turn > 1) ? 1 : 0);
		if (active && turn > 1)
		{
			// Static props must dispatch through mcTextureManager->renderLists() so
			// visibility and batching share the scene render order.

			gos_SetWorldToClipGL(eye->worldToClipGL());

			// Publish the main scene view so static props receive the camera transform;
			// stale or zero view uniforms put the props off-screen.
			{
				RenderCore::ViewUniforms vu{};
				auto stuffToRowMajor = [](const glm::mat4& m, float out[16]) {
					const float* col = glm::value_ptr(m);
					for (int r = 0; r < 4; ++r)
						for (int c = 0; c < 4; ++c)
							out[r * 4 + c] = col[c * 4 + r];
				};
				stuffToRowMajor(eye->worldToClipGL(), vu.worldToClipGL);
				stuffToRowMajor(eye->worldToViewGL(), vu.worldToViewGL);
				const glm::vec3 orig = eye->cameraOriginGL();
				vu.cameraWorldPos[0] = orig.x;
				vu.cameraWorldPos[1] = orig.y;
				vu.cameraWorldPos[2] = orig.z;
				vu.cameraWorldPos[3] = 1.0f;

				{
					RenderCore::EngineView mainView{};
					mainView.id = RenderCore::kMainSceneViewId;
					mainView.viewUniforms = vu;
					mainView.viewport[0] = 0;
					mainView.viewport[1] = 0;
					mainView.viewport[2] = Environment.drawableWidth;
					mainView.viewport[3] = Environment.drawableHeight;
					mainView.renderMask = 0xFFFFFFFF;
					mainView.debugName = "MainScene";
					mainView.mode = RenderCore::ViewMode::Visual;
					mainView.kind = RenderCore::ViewKind::MainScene;
					RenderCore::setCurrentView(mainView);
				}

				static const bool s_vuOn = mc2::gate::get<"MC2_VIEW_UNIFORMS">();
				if (s_vuOn) {
					RenderCore::uploadViewUniforms(vu);
				}
			}

			// The terrain shader needs camera position (distance LOD in TCS)
			// and light direction (PBR lighting in FS). Without these the
			// terrain_camera_pos_ / terrain_light_dir_ uniforms remain at their
			// zero-initialized defaults, breaking distance-based tessellation
			// admission and producing the "GPU path renders zero / falls back to
			// CPU look" symptom. Mirrors code/gamecam.cpp:178-185 exactly.
			glm::vec3 camOrig = getCameraOrigin();
			gos_SetTerrainCameraPos(camOrig.x, camOrig.y, camOrig.z);
			gos_SetTerrainLightDir(lightDirection.x, lightDirection.y, lightDirection.z);

			EditorGpuTimer_Begin();                            // GPU per-pass timing (env-gated)

			if (theSky)
				theSky->render(1);
			EditorGpuTimer_Mark("sky");

			EditorBridge::staticPropFrameBegin();

			// Absorb any mech types registered after finalizeGeometry() (late
			// placement in the editor).
			// No-op when s_pendingLateTypes==false (single branch, ~1ns).
			EditorBridge::mechFinalizePending();

			if (land)
				land->render();
			EditorGpuTimer_Mark("terrain");


			// TODO: Restore the editor actor render walk; the editor has no
			// GameObjectManager, and EditorObjectMgr::render() is a no-op, so actors
			// cannot register per-frame visibility here.
			if (!s_bSensorMapEnabled)
				EditorObjectMgr::instance()->render();
			EditorGpuTimer_Mark("objmgr");

			if (land)
				land->renderWater();
			EditorGpuTimer_Mark("water");


			// TODO: Restore actor shadow visibility; the editor has no renderShadows walk.

			if (!drawOldWay)
			{
				if (compass && (turn > 3) && drawCompass)
					compass->render(-1);		//Force this to zBuffer in front of everything
			}

			// Submit terrain chunk commands before post-processing; the chunk path
			// suppresses legacy per-quad draws, so an unflushed buffer leaves terrain black.
			Terrain::flushDrawCommands();
			EditorGpuTimer_Mark("terrainFlush");

			/* The editor interface needs to be drawn last, as it draws things "on top" of the
			rendered scene. */
			if ( EditorInterface::instance() )
			{
				EditorInterface::instance()->render();
			}
			EditorGpuTimer_Mark("ui");

			if (mcTextureManager)
				mcTextureManager->renderLists();
			EditorGpuTimer_Mark("props");

			if (land)
				land->renderWaterFastPath();
			EditorGpuTimer_Mark("waterFast");

			EditorGpuTimer_End();
		}
	
		if (drawOldWay)
		{
			gos_SetRenderState( gos_State_ZCompare, 0);
			gos_SetRenderState(	gos_State_ZWrite, 0);
			gos_SetRenderState( gos_State_Perspective, 1);

			if (compass && (turn > 3) && drawCompass)
				compass->render();
		}

		// S-CLI: tick the global rendered-frames counter and, if a frames
		// limit was given, post WM_CLOSE once we've finished the Nth frame.
		// Fires exactly once via g_cliClosePosted.
		++g_cliFrameCounter;
		if (!g_cliClosePosted && g_cliFramesLimit > 0 && g_cliFrameCounter >= g_cliFramesLimit)
		{
			g_cliClosePosted = true;
			fprintf(stderr, "[EDITOR_CLI v1] event=frames_done frames=%ld\n", g_cliFrameCounter);
			fflush(stderr);
			CWnd* main = AfxGetMainWnd();
			if (main && main->GetSafeHwnd())
				::PostMessage(main->GetSafeHwnd(), WM_CLOSE, 0, 0);
		}
 	}

	virtual long activate (void)
	{
		if (ready && active)
			return(NO_ERR);
		
		
		lastShadowLightPitch = lightPitch;

		allNormal();

		return NO_ERR;
	}

	virtual long update (void)
	{
		float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);

		float altitudePercent = (cameraAltitude - AltitudeMinimum) / (testMax - AltitudeMinimum);
		Camera::NearPlaneDistance = MinNearPlane + ((MaxNearPlane - MinNearPlane) * altitudePercent);
		Camera::FarPlaneDistance = MinFarPlane + ((MaxFarPlane - MinFarPlane) * altitudePercent);
	
		if (!compass && (turn > 3))	//Create it!
		{
			//Gotta check for the list too because a NEW map has no objects on it and this list
			// Doesn't exist until objects are placed.  Strange but true...
			if ( !appearanceTypeList )
			{
				appearanceTypeList = new AppearanceTypeList;
				gosASSERT(appearanceTypeList != NULL);
			}

			AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "compass" );
			compass = new BldgAppearance;
			compass->init( appearanceType );
		}
		
		if (!theSky && (turn > 3))
		{
			long appearanceType = (GENERIC_APPR_TYPE << 24);
		
			AppearanceTypePtr genericAppearanceType = NULL;
			genericAppearanceType = appearanceTypeList->getAppearance(appearanceType,"skybox");
			if (!genericAppearanceType)
			{
				char msg[1024];
				sprintf(msg,"No Generic Appearance Named %s","skybox");
				Fatal(0,msg);
			}
			  
			theSky = new GenericAppearance;
			gosASSERT(theSky != NULL);
		
			gosASSERT(genericAppearanceType->getAppearanceClass() == GENERIC_APPR_TYPE);
			theSky->init((GenericAppearanceType*)genericAppearanceType, NULL);
			
			theSky->setSkyNumber(EditorData::instance->TheSkyNumber());
			oldSkyNumber = EditorData::instance->TheSkyNumber();
		}

		if (theSky && (oldSkyNumber != EditorData::instance->TheSkyNumber()))
		{
			theSky->setSkyNumber(EditorData::instance->TheSkyNumber());
			oldSkyNumber = EditorData::instance->TheSkyNumber();
		}
		
		long result = Camera::update();

		// Camera::update() re-sets screenResolution from gos_GetViewport (the FULL
		// window width), clobbering the scene-size value set earlier in
		// EditorInterface. With the docked map-resize the GL scene renders into a
		// centralW sub-region (Environment.drawableWidth), so projection +
		// inverseProject picking must use centralW too -- otherwise the placement
		// cursor and the picked point diverge increasingly toward the right. Re-apply
		// the scene size as the LAST word before this frame's render + the next
		// mouse-pick. (Equal to the gos width when undocked, so no-op there.)
		{
			glm::vec3 sceneRes;
			sceneRes.x = (float)Environment.drawableWidth;
			sceneRes.y = (float)Environment.drawableHeight;
			sceneRes.z = 0.0f;
			changeResolution(sceneRes);

			// The object-space projection (Camera::project* -> findObjectByMouse highlight
			// + placement getObjectAtScreenPosition) maps world->screen via the TG/userInput
			// viewport transform, which Camera::update set from gos_GetViewport (FULL window).
			// Re-point it at the SCENE width too -- otherwise objects project into full-window
			// space while the map renders + mouse live in the centralW space, so the highlight
			// and placement diverge from the cursor toward the right. (No-op undocked.)
			TG_Shape::SetViewport(sceneRes.x, sceneRes.y, 0.0f, 0.0f);
			if (userInput)
				userInput->setViewport(sceneRes.x, sceneRes.y, 0.0f, 0.0f);

			static int s_resDiag = 0;
			if ((++s_resDiag % 600) == 1) {
				GLint vp[4] = {0,0,0,0};
				glGetIntegerv(GL_VIEWPORT, vp);
				fprintf(stderr, "[PICKW v1] drawable=%dx%d screenRes=%.0fx%.0f glViewport=%d,%d,%d,%d\n",
					Environment.drawableWidth, Environment.drawableHeight,
					screenResolution.x, screenResolution.y, vp[0], vp[1], vp[2], vp[3]);
				fflush(stderr);
			}
		}

		if ((cameraLineChanged + 10) < turn)
		{
			if (userInput->getKeyDown(KEY_BACKSLASH) && !userInput->ctrl() && !userInput->alt() && !userInput->shift())
			{
				drawCompass ^= true;
				cameraLineChanged = turn;
			}
		}

		#define MAX_SHADOW_PITCH_CHANGE	(5.0f)
		//Always recalc here or shadows never change in editor!!
		forceShadowRecalc = true;

		if (compass && (turn > 3))
   		{
   	   		bool oldFog = useFog;
   			bool oldShadows = useShadows;
   	   		useFog = false;
   			useShadows = false;
   	   		
   	   		glm::vec3 pos = getPosition();
   	   		compass->setObjectParameters(pos,0.0f,false,0,0);
   	   		compass->setMoverParameters(0.0f);
   	   		compass->setGesture(0);
   	   		compass->setObjStatus(OBJECT_STATUS_DESTROYED);
   	   		compass->setInView(true);
   	   		compass->setVisibility(true,true);
   	   		compass->setFilterState(true);
   			compass->setIsHudElement();
   	   		compass->update();		   //Force it to try and draw or stuff will not work!
			
			if (theSky)
			{
				// Position sky sphere at camera eye height, not terrain surface.
				// getPosition().z == cameraShiftZ == terrain elevation; the camera
				// eye is cameraAltitude units above that. On a high-altitude editor
				// camera the gap (~800+ wu) places the sphere center far below the
				// eye, making the sphere appear as a wall at ground level. Adding
				// cameraAltitude centers the sphere at the actual eye height so the
				// camera is always inside the sphere regardless of zoom level.
				glm::vec3 pos = getPosition();
				pos.z = cameraShiftZ + cameraAltitude;

				theSky->setObjectParameters(pos,0.0f,false,0,0);
				theSky->setMoverParameters(0.0f);
				theSky->setGesture(0);
				theSky->setObjStatus(OBJECT_STATUS_NORMAL);
				theSky->setInView(true);
				theSky->setVisibility(true,true);
				theSky->setFilterState(true);
				theSky->setIsHudElement();
				theSky->update();		   //Force it to try and draw or stuff will not work!
			}
					
   	   		useFog = oldFog;
   			useShadows = oldShadows;
   		}

		return result;
	}


};

#endif  // end of file ( EditorCamera.h )
