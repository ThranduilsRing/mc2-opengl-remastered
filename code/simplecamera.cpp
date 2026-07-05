/*************************************************************************************************\
SimpleCamera.cpp	: Implementation of the SimpleCamera component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/
#include"simplecamera.h"
#include <GL/glew.h>   // MC2_LOG_PREVIEW diagnostics: glIsEnabled/glGetIntegerv/glGetBooleanv
#include"tex_resolve_table.h"
#include"appear.h"
#include"mclib.h"
#include"mech3d.h"
#include"mission.h"
#include "../GameOS/gameos/gos_mech_killswitch.h"  // MechPreviewRenderScope (preview-fix)
#include"bdactor.h"
#ifdef MC2_IMGUI
#include "../GuiRuntime/GuiRuntime.h"   // drawPreviewToPanel (PREVIEW-FBO-FIXED-800x600-1)
#endif

// sebi: !NB remove when assert(0 && "test") is removed below
#include <cassert>

extern bool useShadows;
extern bool drawOldWay;
extern bool useFog;

extern MidLevelRenderer::MLRClipper * theClipper;	// NS3: def in mclib/bdactor.cpp

////////////////////////////////////////////////
SimpleCamera::SimpleCamera()
{ 
	pObject = NULL;
	Camera::init();

	char path[256];
	strcpy( path, cameraPath );
	strcat( path, "cameras.fit" );
	FitIniFile camFile;
	if ( NO_ERR != camFile.open( path ) )
	{
		STOP(( "Need Camera File " ));
	}

	Camera::init( &camFile );
	AltitudeTight = 650;
	rotation = -45.f;
	bIsComponent = 0;
	rotateLightRight(90.0f);

	bIsInMission = false;

    bContextNotSet = true;
}


SimpleCamera::~SimpleCamera()
{

    // sebi, do not see how this object cannot be on a heap..
    // so delete unconditionally
    /*
	//Why did we not delete here??
	// It was commented out.
	// -fs
	if ( appearanceTypeList && appearanceTypeList->pointerCanBeDeleted(pObject) )
		delete pObject;
    */

	// sebi: added this condition ecause  appearanceTypeList used inside destructor
	if (appearanceTypeList)
    	delete pObject;

	pObject = NULL;

	//We have to do this here because we always load the damned sensor shape.
	// ONLY if we are running it in logistics.  DO NOT DELETE THESE IN THE MIDDLE OF A MISSION!!!
	if (!bIsInMission)
	{
		if (GVAppearanceType::SensorTriangleShape)
		{
			delete GVAppearanceType::SensorTriangleShape;
			GVAppearanceType::SensorTriangleShape = NULL;
		}
		
		if (GVAppearanceType::SensorCircleShape)
		{
			delete GVAppearanceType::SensorCircleShape;
			GVAppearanceType::SensorCircleShape = NULL;
		}

		if (Mech3DAppearanceType::SensorSquareShape)
		{
			delete Mech3DAppearanceType::SensorSquareShape;
			Mech3DAppearanceType::SensorSquareShape = NULL;
		}
	}
}

void SimpleCamera::init( float left, float top, float right, float bottom )
{
	bounds[0] = left;
	bounds[1] = top;
	bounds[2] = right;
	bounds[3] = bottom;

}

////////////////////////////////////////////////
void SimpleCamera::render()
{
	render( 0, 0 );
}

void SimpleCamera::render(long xOffset, long yOffset)
{
	// PREVIEW-FBO-FIXED-800x600-1: resolved the ours/theirs conflict that used to be
	// documented here. Root cause of the "Mechlopedia/Mech Bay preview shows nothing"
	// bug (2026-07-04 session): the real GL window/ImGui run at the real configured
	// resolution, but this legacy CPU MLR draw's screen-space math (gos_GetViewport,
	// TG_Shape::SetViewport, globalScaleFactor) is written entirely in terms of
	// Environment.screenWidth/Height, which the legacy 2D UI intentionally keeps at a
	// fixed 800x600 (see gos_SetHudResClampEnabled in gameos_graphics.cpp) -- so the
	// mech drew at the right LOGICAL position but the wrong REAL screen position
	// relative to where the new ImGui panel actually is on a real-resolution display.
	// Tried making Environment.screenWidth track the real resolution instead
	// (rejected after adversarial+meta review: breaks other legacy tuned-resolution
	// code, gets re-clamped elsewhere, inconsistent partial-migration state).
	// Fix instead: for the WORLDLESS panel-preview case (Mech Bay / Mechlopedia /
	// Options->Gameplay paint preview -- no mission loaded), render into a FIXED
	// 800x600 offscreen FBO (gos_BeginCameraPreviewRender/EndCameraPreviewRender)
	// so every line of this function's existing 800x600-relative math stays exactly
	// as it always was -- the camera's own ZoomTight/AltitudeTight aim already
	// targets the panel's sub-rect WITHIN that virtual 800x600 canvas (see bounds[]
	// usage in update()). The caller then draws gos_GetCameraPreviewTexture(),
	// UV-cropped to bounds[]/800,600, as a normal ImGui image scaled to fit the
	// real-resolution panel -- see GuiRuntime::DrawUiImage / drawPreviewToPanel().
	// Gated on setPreviewOffscreen(true), NOT on mission==NULL: a caller with no
	// defs/ImGui panel to composite into (e.g. Mech Bay, as of this pass -- fully
	// legacy, no hasDefsUiPage()/Gui3DView) must keep drawing straight to the real
	// screen as before, or its preview would render into a texture nobody reads.
	// The in-mission cinematic camera pan (mission != NULL) never sets this flag
	// and is UNCHANGED: it still draws directly, matching its existing (working,
	// untouched by this bug) behavior.
	const bool isPanelPreview = drawOffscreen_;

	if ( xOffset != 0 && yOffset != 0 ) // don't know how to do this
		return;

	if ( getenv("MC2_LOG_PREVIEW") )
	{
		FILE* f = fopen("preview_debug.log","a");
		if (f) { fprintf(f,"[PREVIEW] SimpleCamera::render pObject=%p bIsComponent=%d\n",(void*)pObject,(int)bIsComponent); fflush(f); fclose(f); }
	}

	if ( pObject )
	{
		// PREVIEW-FIX: mark this as a UI preview render so Mech3DAppearance::render
		// bypasses the GPU mech batcher (whose flush uses the world snapshot/terrain
		// MVP) and takes the CPU MLR draw, which honors THIS SimpleCamera. Scoped:
		// world/tactical rendering never sees a nonzero depth.
		MechPreviewRenderScope _previewScope;

		// PREVIEW-FBO-FIXED-800x600-1: worldless panel preview draws into its own
		// fixed 800x600 offscreen texture instead of the real framebuffer -- see the
		// comment above render(long,long). The in-mission cinematic pan is untouched.
		if ( isPanelPreview )
		{
			gos_BeginCameraPreviewRender();
			if ( getenv("MC2_LOG_PREVIEW") )
			{
				GLint fboAfterBegin = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fboAfterBegin);
				FILE* f = fopen("preview_debug.log","a");
				if (f) { fprintf(f,"[PREVIEW] fboAfterBegin=%d\n", fboAfterBegin); fflush(f); fclose(f); }
			}
		}

		if ( bIsComponent )
		{
			lightRed = 196;
			lightGreen = 196;
			lightBlue = 220;

			ambientRed = 196;
			ambientGreen = 196;
			ambientBlue = 196;
		}

		gos_PushRenderStates();
		// PREVIEW-FBO-FIXED-800x600-1 root-cause fix #2: gosRenderer::drawTris
		// (gameos_graphics.cpp) defers EVERY triangle into a hudBatch_ queue
		// instead of drawing immediately whenever gos_State_IsHUD is true --
		// and it IS true for this entire call (mechbayscreen.cpp/mechlopedia.cpp
		// wrap their whole LogisticsScreen::render() in
		// gos_SetRenderState(gos_State_IsHUD,1)). The deferred batch flushes
		// later against the real/main framebuffer, by which point our offscreen
		// preview FBO is already unbound (gos_EndCameraPreviewRender() below) --
		// so the mech's triangles were being queued and then silently drawn to
		// the wrong target. Force IsHUD off for just this draw so drawTris takes
		// its immediate path into our bound FBO; gos_PopRenderStates() below
		// restores the caller's IsHUD=1 automatically.
		if ( isPanelPreview )
			gos_SetRenderState( gos_State_IsHUD, 0 );
		oldCam = eye;
		eye = this;
		useFog = 0;

		gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
		if ( getenv("MC2_LOG_PREVIEW") )
		{
			FILE* f = fopen("preview_debug.log","a");
			if (f) { fprintf(f,"[PREVIEW] gos_GetViewport viewMul=%.1f,%.1f viewAdd=%.1f,%.1f EnvScreen=%d,%d bounds=[%.1f,%.1f,%.1f,%.1f]\n",
				viewMulX, viewMulY, viewAddX, viewAddY, Environment.screenWidth, Environment.screenHeight,
				bounds[0], bounds[1], bounds[2], bounds[3]); fflush(f); fclose(f); }
		}

		//--------------------------------------------------------
		// Get new viewport values to scale stuff.  No longer uses
		// VFX stuff for this.  ALL GOS NOW!
			screenResolution.x = viewMulX;
			screenResolution.y = viewMulY;
			calculateProjectionConstants();
		
			TG_Shape::SetViewport(viewMulX,viewMulY,viewAddX,viewAddY);	

			globalScaleFactor = getScaleFactor();
			globalScaleFactor *= viewMulX / Environment.screenWidth;		//Scale Mechs to ScreenRES
			
			//-----------------------------------------------
				
			setLightColor(1,0xffffffff);
			setLightIntensity(1,1.0);
		
			MidLevelRenderer::MLRState default_state;
			default_state.SetBackFaceOn();
			default_state.SetDitherOn();
			default_state.SetTextureCorrectionOn();
			default_state.SetZBufferCompareOn();
			default_state.SetZBufferWriteOn();
		
			default_state.SetFilterMode(MidLevelRenderer::MLRState::BiLinearFilterMode);
			
			Stuff::RGBAColor fColor;
			fColor.red = 0;
			fColor.green = 0;
			fColor.blue = 0;

			float z = 1.0;
			MidLevelRenderer::PerspectiveMode = usePerspective;
			// PREVIEW-FBO-FIXED-800x600-1 root-cause fix: TG_MultiShape::Render (the
			// CPU draw pObject->render() below dispatches into) transforms every
			// shape via the GLOBAL STATIC TG_Shape::s_worldToClip
			// (msl.cpp:2068 shapeToClip.Multiply(shapeToWorld, TG_Shape::s_worldToClip)),
			// which is only ever populated by Camera::render() -- the base-class
			// method every OTHER camera (GameCamera, the main world camera) calls
			// every frame (camera.cpp:2313 TG_Shape::SetCameraMatrices). SimpleCamera
			// overrides render() completely and never called it, so the preview mech
			// was always being transformed with whatever camera matrix the last WORLD
			// frame left behind (a mission camera, or stale/zero at boot) -- nowhere
			// near this camera's small offscreen frustum, so nothing ever rasterized
			// (confirmed via the preview harness: pObject->render() runs with no GL
			// error, but a full-FBO pixel scan finds zero non-background pixels
			// anywhere). Set it here, same as the base class does, before drawing.
			TG_Shape::SetCameraMatrices(&cameraOrigin, &cameraToClip);
			if ( getenv("MC2_LOG_PREVIEW") )
			{
				FILE* f = fopen("preview_debug.log","a");
				if (f) {
					fprintf(f,"[PREVIEW] cameraToClip=[%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]\n",
						cameraToClip(0,0), cameraToClip(0,1), cameraToClip(0,2), cameraToClip(0,3),
						cameraToClip(1,0), cameraToClip(1,1), cameraToClip(1,2), cameraToClip(1,3),
						cameraToClip(2,0), cameraToClip(2,1), cameraToClip(2,2), cameraToClip(2,3),
						cameraToClip(3,0), cameraToClip(3,1), cameraToClip(3,2), cameraToClip(3,3));
					fflush(f); fclose(f);
				}
			}
			theClipper->StartDraw(cameraOrigin, cameraToClip, fColor, &fColor, default_state, &z);
			MidLevelRenderer::GOSVertex::farClipReciprocal = (1.0f-cameraToClip(2, 2))/cameraToClip(3, 2);

			//--------------------------------
			//Set States for Software Renderer
			if (Environment.Renderer == 3)
			{
				gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);

				gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
				gos_SetRenderState( gos_State_MonoEnable, 1);
				gos_SetRenderState( gos_State_Perspective, 0);
				gos_SetRenderState( gos_State_Clipping, 1);
				gos_SetRenderState( gos_State_Specular, 0);
				gos_SetRenderState( gos_State_Dither, 0);
				gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
				gos_SetRenderState( gos_State_Filter, gos_FilterNone);
				gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
				gos_SetRenderState( gos_State_ZCompare, 1);
				gos_SetRenderState(	gos_State_ZWrite, 1);
			}
			//--------------------------------
			//Set States for Hardware Renderer	
			else
			{
				gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
				gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
				gos_SetRenderState( gos_State_MonoEnable, 0);
				gos_SetRenderState( gos_State_Perspective, 1);
				gos_SetRenderState( gos_State_Clipping, 1);
				gos_SetRenderState( gos_State_Specular, 1);
				gos_SetRenderState( gos_State_Dither, 1);
				gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
				gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
				gos_SetRenderState( gos_State_ZCompare, 1);
				gos_SetRenderState(	gos_State_ZWrite, 1);
	
			}


			// EXPERIMENT (PREVIEW-FBO-FIXED-800x600-1 debug): the mech draws zero
			// pixels anywhere in the FBO despite shouldRender=1. Test whether
			// backface culling is discarding every triangle (e.g. an unexpected
			// winding-order flip specific to FBO targets). MC2_PREVIEW_NOCULL=1
			// force-disables GL_CULL_FACE for just this offscreen draw.
			if ( isPanelPreview && getenv("MC2_PREVIEW_NOCULL") )
				glDisable(GL_CULL_FACE);

			if ( getenv("MC2_LOG_PREVIEW") )
			{
				GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
				GLboolean depthMask = GL_FALSE; glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
				GLboolean cullFace = glIsEnabled(GL_CULL_FACE);
				GLint frontFace = 0; glGetIntegerv(GL_FRONT_FACE, &frontFace);
				GLint cullMode = 0; glGetIntegerv(GL_CULL_FACE_MODE, &cullMode);
				GLint vp[4] = {}; glGetIntegerv(GL_VIEWPORT, vp);
				GLint fbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
				FILE* f = fopen("preview_debug.log","a");
				if (f) {
					fprintf(f,"[PREVIEW] pre-draw fbo=%d depthTest=%d depthMask=%d cullFace=%d frontFace=0x%X cullMode=0x%X vp=[%d,%d,%d,%d] globalScaleFactor=%.4f bounds=[%.0f,%.0f,%.0f,%.0f]\n",
						fbo, (int)depthTest, (int)depthMask, (int)cullFace, (unsigned)frontFace, (unsigned)cullMode, vp[0],vp[1],vp[2],vp[3], globalScaleFactor,
						bounds[0],bounds[1],bounds[2],bounds[3]);
					fflush(f); fclose(f);
				}
			}
			// PREVIEW-DRAWOLDWAY-1 (THE blank-panel root cause): with drawOldWay
			// false, TG_Shape::Render does NOT draw — it defers every triangle
			// into mcTextureManager->addVertices() buckets (tgl.cpp:3252+), which
			// renderLists() rasterizes LATER in the frame, long after
			// gos_EndCameraPreviewRender() has restored FBO 0. That is exactly
			// the observed "fbo=8 pre-draw, curFbo=0 at drawTris, nonBgCount=0"
			// signature. Force the immediate-draw branch (gos_DrawTriangles at
			// tgl.cpp:3230) for the duration of the preview render so the mech
			// rasterizes into the preview FBO while it is bound. Save/restore —
			// the global must stay false for the world renderer.
			const bool savedDrawOldWay = drawOldWay;
			if ( isPanelPreview )
				drawOldWay = true;
			pObject->render();
			if ( isPanelPreview )
				drawOldWay = savedDrawOldWay;
			if ( getenv("MC2_LOG_PREVIEW") )
			{
				GLenum err = glGetError();
				// Sample a handful of pixels across the panel rect (bounds[]) plus
				// the FBO's dead-center, to tell "mesh rasterized nothing" (all
				// samples == clear color 0,0,0,255) apart from "mesh drew but the
				// composite/UV step is wrong" (some samples differ from clear).
				// SCAN THE FULL PHYSICAL VIEWPORT. The preview FBO is supersampled
				// (e.g. 3200x2400 for logical 800x600); the old fixed 800x600 read
				// only covered the top-left sixteenth, so a correctly-drawn mech at
				// logical (~379,206) => physical (~1516,824) scored nonBgCount=0 —
				// a false "rasterized nothing".
				GLint svp[4] = {}; glGetIntegerv(GL_VIEWPORT, svp);
				const int pw = svp[2] > 0 ? svp[2] : 800;
				const int ph = svp[3] > 0 ? svp[3] : 600;
				int cx = (int)((bounds[0] + bounds[2]) * 0.5f * pw / 800.0f);
				int cy = ph - (int)((bounds[1] + bounds[3]) * 0.5f * ph / 600.0f);   // GL bottom-up
				GLubyte centerPx[4] = {}, cornerPx[4] = {};
				glReadPixels(cx, cy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, centerPx);
				glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, cornerPx);
				int nonBgCount = 0, minX=9999, minY=9999, maxX=-1, maxY=-1;
				static GLubyte* s_scanBuf = nullptr;
				static int s_scanCap = 0;
				if (pw*ph*4 > s_scanCap) { delete[] s_scanBuf; s_scanBuf = new GLubyte[pw*ph*4]; s_scanCap = pw*ph*4; }
				glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, s_scanBuf);
				const int step = pw >= 1600 ? 16 : 4;
				for (int y = 0; y < ph; y += step) {
					for (int x = 0; x < pw; x += step) {
						GLubyte* p = s_scanBuf + (y*pw + x)*4;
						if (p[0] != 0 || p[1] != 0 || p[2] != 0) {
							nonBgCount++;
							if (x < minX) minX = x; if (x > maxX) maxX = x;
							if (y < minY) minY = y; if (y > maxY) maxY = y;
						}
					}
				}
				FILE* f = fopen("preview_debug.log","a");
				if (f) {
					fprintf(f,"[PREVIEW] post-draw glErr=0x%X centerPx(%d,%d)=RGBA(%d,%d,%d,%d) cornerPx(10,10)=RGBA(%d,%d,%d,%d) nonBgCount=%d bbox=[%d,%d,%d,%d]\n",
						(unsigned)err, cx, cy, centerPx[0],centerPx[1],centerPx[2],centerPx[3],
						cornerPx[0],cornerPx[1],cornerPx[2],cornerPx[3], nonBgCount, minX,minY,maxX,maxY);
					fflush(f); fclose(f);
				}
			}
			// PREVIEW-FBO-FIXED-800x600-1: end the offscreen preview render and
			// restore the real framebuffer/viewport before anything else in this
			// scope touches the screen.
			if ( isPanelPreview )
				gos_EndCameraPreviewRender();
			// PREVIEW-WORLDLESS-DRAIN-1: the !drawOldWay block below drains the
			// in-mission GPU-driven scene (renderLists() + renderWaterFastPath() +
			// scene-FBO post) so terrain/water appear on the SimpleCamera
			// intro/deployment cinematic pan. On a WORLDLESS menu mech preview
			// (Mech Bay / Options->Gameplay paint preview, where mission==NULL) there
			// is no terrain or water to draw, yet this block still ran every frame --
			// draining a near-empty queue against the scene FBO and inheriting GL
			// state. AMD tolerated it; NVIDIA's stricter FBO/depth behavior surfaced it
			// as a ~1Hz whole-screen flash with the mech visible for one frame. The
			// preview needs only the CPU MLR mech draw above (pObject->render(), forced
			// by MechPreviewRenderScope). Gate the world drain on a live mission so the
			// cinematic (mission!=NULL) is unchanged. Escape hatch: MC2_PREVIEW_SCENE_DRAIN=1
			// forces the old always-drain behavior.
			bool worldScenePresent = (mission != NULL);
			{
				static int s_forceDrain = -1;
				if (s_forceDrain < 0) {
					const char* v = std::getenv("MC2_PREVIEW_SCENE_DRAIN");
					s_forceDrain = (v && v[0] == '1') ? 1 : 0;
				}
				if (s_forceDrain == 1) worldScenePresent = true;
			}
			if ( !drawOldWay && worldScenePresent ) {
				// GPU-CULL-SIMPLECAM-1: update terrain MVP before renderLists() so
				// compute_dispatch() uses THIS camera's world-to-clip, not the stale
				// matrix left by the last GameCamera::render(). Without this, the GPU
				// cull frustum test runs with last frame's GameCamera MVP, mis-culling
				// visible actors on every camera-move frame of the intro pan.
				gos_SetWorldToClipGL(worldToClipGL());
				mcTextureManager->renderLists();
				// CINEMATIC-WATER-1: mirror GameCamera::render (gamecam.cpp:354).
				// Draw the GPU water fast path after renderLists() so water appears
				// on the SimpleCamera intro/deployment pan. The cinematic path only
				// ran renderLists() (terrain + objects); when water moved out of the
				// legacy renderLists drain into the separate renderWaterFastPath()
				// call, the intro lost water (regressed the MISSION-INTRO-ARMED-RENDER-1
				// fix). renderWaterFastPath() self-guards: no-op unless gpu_driven
				// water is enabled AND WaterStream is ready AND terrainTextures2 exists
				// (so it is harmless on the component/mech-bay SimpleCamera).
				if (land)
					land->renderWaterFastPath();
				// VFX-CACHE-SYNC-1: re-sync the gos render-state cache after the
				// raw-GL water pass (mirrors GameCamera + mech batcher).
				gos_InvalidateRenderStateCache();
			}
			endFrameTexResolve();              // defensive — see plan Task 2 Step 3a.
			eye = oldCam;
			gos_PopRenderStates();
	}



}

// PREVIEW-FBO-FIXED-800x600-1: composite this camera's panel-preview render
// (see render(long,long)) into the real-resolution defs/ImGui page. bounds[]
// is this camera's rect within the fixed 800x600 virtual canvas the preview
// FBO was rendered at; converting to [0,1] UV crops just that sub-rect out of
// the full 800x600 texture. v is flipped (GL texture row 0 = bottom) so the
// image isn't upside down in ImGui's top-left-origin image space.
void SimpleCamera::drawPreviewToPanel( float panelX, float panelY, float panelW, float panelH ) const
{
#ifdef MC2_IMGUI
	const unsigned int tex = gos_GetCameraPreviewTexture();
	const float u1 = bounds[0] / 800.f;
	const float u2 = bounds[2] / 800.f;
	const float v1 = 1.f - bounds[1] / 600.f;   // top    -> flipped
	const float v2 = 1.f - bounds[3] / 600.f;   // bottom -> flipped
	if ( getenv("MC2_LOG_PREVIEW") )
	{
		FILE* f = fopen("preview_debug.log","a");
		if (f) {
			fprintf(f,"[PREVIEW] drawPreviewToPanel tex=%u panel=[%.1f,%.1f,%.1f,%.1f] uv=[%.3f,%.3f,%.3f,%.3f] readyForUi=%d\n",
				tex, panelX, panelY, panelW, panelH, u1, v1, u2, v2, (int)GuiRuntime::IsReadyForUiText());
			fflush(f); fclose(f);
		}
	}
	if ( !tex )
		return;

	bool ok = GuiRuntime::DrawUiImage( tex, panelX, panelY, panelW, panelH, u1, v1, u2, v2, 0xffffffff );
	if ( getenv("MC2_LOG_PREVIEW") )
	{
		FILE* f = fopen("preview_debug.log","a");
		if (f) { fprintf(f,"[PREVIEW] DrawUiImage returned %d\n", (int)ok); fflush(f); fclose(f); }
	}
#else
	(void)panelX; (void)panelY; (void)panelW; (void)panelH;
#endif
}

////////////////////////////////////////////////
long SimpleCamera::update()
{
	if ( pObject )
	{
		// MERGE-CONFLICT-UI-PHASE1: see matching note in SimpleCamera::render() above —
		// theirs additionally pulled Camera::NearPlaneDistance to -10.0f here and ran a
		// fit-to-panel AltitudeTight convergence loop (g_previewCaptureBounds/fitFrames_)
		// after pObject->update(). Not reapplied; kept ours (MechPreviewRenderScope full-
		// transform scope) to stay compiling. Needs re-derivation, not blind reapply.
		// PREVIEW-FIX: the geometry consumed by the CPU MLR preview draw is built
		// here (pObject->update() -> updateGeometry -> TransformMultiShape). Mark
		// the preview context for this whole update so updateGeometry runs the
		// FULL transform (populating listOfVertices) instead of the GPU
		// _PositionsOnly fast path, which would leave the preview blank.
		MechPreviewRenderScope _previewScope;

		turn++;			//Must increment this now or matrices NEVER change!!

		//reset the TGL RAM pools.
		colorPool->reset();
		vertexPool->reset();
		facePool->reset();
		shadowPool->reset();
		trianglePool->reset();

        // sebi: why do it two times???
        
		//reset the TGL RAM pools.
		colorPool->reset();
		vertexPool->reset();
		facePool->reset();
		shadowPool->reset();
		trianglePool->reset();

		mcTextureManager->clearArrays();
		mcTextureManager->update();

		Camera::update();
		//--------------------------------------------------------
		// Get new viewport values to scale stuff.  No longer uses
		// VFX stuff for this.  ALL GOS NOW!
			screenResolution.x = viewMulX;
			screenResolution.y = viewMulY;
			calculateProjectionConstants();

			float offsetX = bounds[2] + bounds[0] - viewMulX;
			offsetX /= 2;
			offsetX += fudgeX;
			float offsetY = bounds[1] + bounds[3] - viewMulY;
			offsetY /= 2;
			offsetY += fudgeY; // hack, just to get exactly where Dorje wants it
		

			TG_Shape::SetViewport(viewMulX,viewMulY, offsetX, offsetY); 

		useShadows = 0;
		oldCam = eye;
		eye = this;
		Camera::update();

		ZoomTight();
		
		pObject->recalcBounds();
		pObject->scale(shapeScale);

		// we don't want to center around the feet
		Stuff::Vector3D mechPos = pObject->getRootNodeCenter();
		mechPos.x = -mechPos.x/2.f;
		float tmp = -mechPos.y/2.f;
		mechPos.y = -mechPos.z/2.f;
		mechPos.z = tmp;


		float rotation = frameLength * rotationIncrement + pObject->rotation;

		pObject->setObjectParameters(mechPos, rotation, 0, 0, 0);
	
		pObject->update();
		pObject->setVisibility(true,true);

		if ( getenv("MC2_LOG_PREVIEW") )
		{
			FILE* f = fopen("preview_debug.log","a");
			if (f) { fprintf(f,"[PREVIEW] update bounds=[%.0f,%.0f,%.0f,%.0f] viewMul=%.0f,%.0f off=%.1f,%.1f scale=%.3f Alt=%.0f mechPos=%.1f,%.1f,%.1f\n",
				bounds[0],bounds[1],bounds[2],bounds[3],viewMulX,viewMulY,offsetX,offsetY,shapeScale,(float)AltitudeTight,mechPos.x,mechPos.y,mechPos.z); fflush(f); fclose(f); }
		}

		eye = oldCam;

	}

	return 0;
}

void SimpleCamera::setMech(const char* fileName, long baseColor, long highlight1, long highlight2 )
{
	this->pushContext();

	shapeScale = 0.0f;

	bIsComponent = 0;

	fudgeX = 5;
	fudgeY = 10;

	AltitudeTight = 650;

    // sebi, do not see how this object cannot be on a heap..
    // so delete unconditionally
    /*
	// moving this to above the spot where we create the appearancetypelist
	if ( appearanceTypeList && appearanceTypeList->pointerCanBeDeleted(pObject) )
		delete pObject;
        */
	// sebi: added this conition ecause  appearanceTypeList used inside destructor
	if (appearanceTypeList)
		delete pObject;


	if ( !appearanceTypeList )
		Mission::initBareMinimum();

	rotationIncrement = 0;
	

	pObject = NULL;

	if ( !fileName )
	{
//		allNormal();
		this->popContext();
		return;
	}

	char NoPathFileName[256];
	_splitpath( fileName, NULL, NULL, NoPathFileName, NULL );

	char testName[256];
	strcpy( testName, NoPathFileName );
	strcat( testName, "enc" );

	FullPathFileName path;
	path.init( tglPath, testName, ".ini" );


	//MUST ALWAYS CALL GET, EVEN IF WE HAVE AN APPEARANCE TYPE OR REFERENCE COUNT DOES NOT INCREASE!
	Mech3DAppearanceType* appearanceType = NULL;
	
	if ( fileExists( path ) )
		appearanceType = (Mech3DAppearanceType*)appearanceTypeList->getAppearance( MECH_TYPE << 24, (char*)testName );
	else
		appearanceType = (Mech3DAppearanceType*)appearanceTypeList->getAppearance( MECH_TYPE << 24, (char*)NoPathFileName );

	pObject = new Mech3DAppearance;	
	pObject->init( appearanceType );
	pObject->setGestureGoal(2);
	pObject->resetPaintScheme( highlight1, highlight2, baseColor );
	pObject->rotation = rotation;

	activate();
		
	setPosition(position, 0);
	ZoomTight();

	this->popContext();
}

void SimpleCamera::setVehicle(const char* fileName,long base, long highlight, long h2)
{
    this->pushContext();

	shapeScale = 0.0f;

	bIsComponent = 0;

	fudgeX = 5;
	fudgeY = 10;

	AltitudeTight = 650;

	if ( !appearanceTypeList )
		Mission::initBareMinimum();

	rotationIncrement = 90;
	
    // sebi, do not see how this object cannot be on a heap..
    // so delete unconditionally
    /*
	if ( appearanceTypeList && appearanceTypeList->pointerCanBeDeleted(pObject) )
		delete pObject;
        */
  
	// sebi: added this conition ecause  appearanceTypeList used inside destructor
	if (appearanceTypeList)
    	delete pObject;

	pObject = NULL;

	if ( !fileName )
	{
		this->popContext();
		return;
	}

	char NoPathFileName[256];
	_splitpath( fileName, NULL, NULL, NoPathFileName, NULL );


	char testName[256];
	strcpy( testName, fileName );
	strcat( testName, "enc" );

	FullPathFileName path;
	path.init( tglPath, testName, ".ini" );

	//MUST ALWAYS CALL GET, EVEN IF WE HAVE AN APPEARANCE TYPE OR REFERENCE COUNT DOES NOT INCREASE!
	GVAppearanceType* appearanceType = NULL;
	
	if ( fileExists( path ) )
		appearanceType = (GVAppearanceType*)appearanceTypeList->getAppearance( GV_TYPE << 24, (char*)testName );
	else
		appearanceType= (GVAppearanceType*)appearanceTypeList->getAppearance( GV_TYPE << 24, (char*)NoPathFileName );

	pObject = new GVAppearance;	
	pObject->init( appearanceType );
	pObject->setGestureGoal(2);
	pObject->resetPaintScheme(base, highlight, h2);
	pObject->rotation = rotation;

	activate();
		
	setPosition(position);
	ZoomTight();

	this->popContext();
}



void SimpleCamera::setComponent(const char* fileName )
{
	this->pushContext();

	shapeScale = 0.0f;

	bIsComponent = 1;

	AltitudeTight = 580;

	fudgeX = 0;
	fudgeY = 0;

	
	if ( !appearanceTypeList )
		Mission::initBareMinimum();

	
    // sebi, do not see how this object cannot be on a heap..
    // so delete unconditionally
    /*
	if ( appearanceTypeList && appearanceTypeList->pointerCanBeDeleted(pObject) )
		delete pObject;
        */

	// sebi: added this conition ecause  appearanceTypeList used inside destructor
	if (appearanceTypeList)
    	delete pObject;

	pObject = NULL;


	if ( !fileName )
	{
		this->popContext();
		return;
	}

	char testName[256];
	strcpy( testName, fileName );
	strcat( testName, "enc" );

	FullPathFileName path;
	path.init( tglPath, testName, ".ini" );
	BldgAppearanceType* appearanceType = NULL;
	if ( fileExists( path ) )
	{
		appearanceType = (BldgAppearanceType*)appearanceTypeList->getAppearance( BLDG_TYPE << 24, (char*)testName );
	}
	else
		appearanceType = (BldgAppearanceType*)appearanceTypeList->getAppearance( BLDG_TYPE << 24, (char*)fileName );

	//MUST ALWAYS CALL GET, EVEN IF WE HAVE AN APPEARANCE TYPE OR REFERENCE COUNT DOES NOT INCREASE!
	 

	pObject = new BldgAppearance;	
	pObject->init( appearanceType );
	pObject->resetPaintScheme(0xffff7e00, 0xffff7e00, 0xffbcbcbc);
	pObject->rotation = rotation;

	rotationIncrement = 90;
	


	activate();
		
	setPosition(position);
	ZoomTight();

	this->popContext();
}
void SimpleCamera::setScale( float newAltitude )
{
	shapeScale = newAltitude;
}

void SimpleCamera::setBuilding( const char* pBuilding )
{
	shapeScale = 0.0f;

	setComponent( pBuilding );
	AltitudeTight = 800;
	bIsComponent = 0;
}

void SimpleCamera::setObject( const char* pFileName, long type, long base, long highlight, long h2 )
{
	if ( !pFileName || !strlen( pFileName ) )
	{
		// Empty/NULL filename = no model. Free existing object and return cleanly.
		if ( appearanceTypeList )
			delete pObject;
		pObject = NULL;
		return;
	}
	switch( type )
	{
	case BLDG_TYPE:
		setBuilding( pFileName );
			break;
	case TREED_TYPE:
		setBuilding( pFileName ); // this might not work....
			break;
	case GV_TYPE:
		setVehicle( pFileName, base, highlight, h2 );
			break;
	case MECH_TYPE:
		setMech( pFileName, base, highlight, h2 );
			break;

	default:
		gosASSERT( !"camera just got an unknown type!" );
	}
}

void SimpleCamera::setColors( long base, long highlight, long h2 )
{
	pObject->resetPaintScheme( base, highlight, h2 );
}

void SimpleCamera::zoomIn( float howMuch )
{
	AltitudeTight = 650.f/howMuch;

}// scale for things that can't 

