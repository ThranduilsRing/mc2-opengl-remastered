#include "mc2_czstring.h"
#include "vk_scene_renderer.h"
#include <tracy/Tracy.hpp>
#include "utils/mc2log.h"
#include "Gates/preferences_legacy.h"
#include "../GameOS/gameos/ui_texture.h"
#include "../GameOS/gameos/vulkan_legacy_shape_island.h"
#include "../GameOS/gameos/vulkan_mech_island.h"
#include "../GameOS/gameos/gos_mech_batcher.h"
#include "../GameOS/gameos/gos_postprocess.h"
/*************************************************************************************************\
SimpleCamera.cpp	: Implementation of the SimpleCamera component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/
#include"simplecamera.h"
#include"tex_resolve_table.h"
#include"appear.h"
#include"mclib.h"
#include"mech3d.h"
#include"mission.h"
#include "../GameOS/gameos/gos_mech_killswitch.h"  // MechPreviewRenderScope (preview-fix)
#include"bdactor.h"
#include"gvactor.h"
#ifdef MC2_IMGUI
#include "../GuiRuntime/GuiRuntime.h"   // drawPreviewToPanel (PREVIEW-FBO-FIXED-800x600-1)
#endif

// sebi: !NB remove when assert(0 && "test") is removed below
#include <cassert>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <string>
#include "Gates/gate.h"
#include "mc2fmt.h"

// Screen-space extents of the last PreviewDraw (mclib/tgl.cpp, filled in preview_draw.cpp).
extern bool g_previewCaptureBounds;
extern bool g_previewBoundsOnly;
extern float g_previewBoundsMinX, g_previewBoundsMaxX, g_previewBoundsMinY, g_previewBoundsMaxY;
extern std::atomic<uint32_t> g_mc2FrameCounter;
void ProbeGatherLightsForList(TG_LightPtr* lights, unsigned long nLights, TG_HWLightsData* out);

// Mech lab: the in-game GPU mech material path into the panel (vulkan_mech_island.cpp, preview pass). One GPU
// panel mech per frame (the batcher snapshot holds one flush); a second camera that frame keeps its CPU draw.
// Returns true when a request was posted; the CPU draw may still be needed until the island reports a draw.
static bool postMechPreviewGpu( Camera& cam, Mech3DAppearance* mech, float vpX, float vpY, float vpW, float vpH,
                                float groundX, float groundY, float groundH )
{
	static uint32_t s_lastFrame = 0xffffffffu;
	const uint32_t frame = g_mc2FrameCounter.load( std::memory_order_relaxed );
	if ( frame == s_lastFrame || !mc2_vk_mech_preview_usable() )
		return false;
	// A gap in panel frames is a new lab visit: the batcher still holds the last mission's (freed) types.
	const bool freshSession = s_lastFrame == 0xffffffffu || frame != s_lastFrame + 1;
	s_lastFrame = frame;
	if ( !mech->submitPreviewGpu( freshSession ) )
		return false;	// batcher not finalized or no shape: the CPU mech draw stays on
	GpuMechBatcher::instance().flush();

	static TG_HWLightsData s_row;	// the lab actor's single light row (lightDataIndex 0)
	ProbeGatherLightsForList( cam.getWorldLights(), static_cast<unsigned long>( cam.getNumLights() ), &s_row );
	Mc2MechPreviewRequest q{};
	const glm::mat4 m = cam.worldToClipGL();	// column-major; the island takes row-major (GL_FALSE), as gamecam.cpp packs it
	const float* col = &m[0][0];
	for ( int r = 0; r < 4; ++r )
		for ( int c = 0; c < 4; ++c )
			q.worldToClipGL[r * 4 + c] = col[c * 4 + r];
	const glm::vec3 eye = cam.cameraOriginGL();
	q.cameraWorldPos[0] = eye.x; q.cameraWorldPos[1] = eye.y; q.cameraWorldPos[2] = eye.z; q.cameraWorldPos[3] = 1.0f;
	q.lights = &s_row;
	q.lightBytes = sizeof( s_row );
	q.vpX = static_cast<int>( lroundf( vpX ) ); q.vpY = static_cast<int>( lroundf( vpY ) );
	q.vpW = static_cast<int>( lroundf( vpW ) ); q.vpH = static_cast<int>( lroundf( vpH ) );
	q.exposure = 1.0f;
	q.groundX = groundX; q.groundY = groundY; q.groundH = groundH;
	if ( gosPostProcess* pp = getGosPostProcess() )
	{
		q.sceneW = pp->getWidth();
		q.sceneH = pp->getHeight();
	}
	if ( q.sceneW <= 0 || q.sceneH <= 0 )
	{
		q.sceneW = Environment.screenWidth;
		q.sceneH = Environment.screenHeight;
	}
	mc2_vk_mech_preview_request( &q );
	return true;
}

extern bool drawOldWay;
extern bool useFog;

extern MidLevelRenderer::MLRClipper * theClipper;	// NS3: def in mclib/bdactor.cpp

////////////////////////////////////////////////
SimpleCamera::SimpleCamera()
{ 
	pObject = nullptr;
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

// The appearance destructor reads appearanceTypeList: with no list (outside a mission) the object is left unfreed, as before B7o.5.
void SimpleCamera::releaseObject()
{
	if (appearanceTypeList)
	{
		// Appearance-owned lamps belong to this preview camera, even when it is not rendering.
		CameraPtr savedCamera = eye;
		eye = this;
		pObject.reset();
		eye = savedCamera;
	}
	else
		(void)pObject.release();
}

SimpleCamera::~SimpleCamera()
{

	// sebi: added this condition ecause  appearanceTypeList used inside destructor
	releaseObject();

	pObject = nullptr;

	//We have to do this here because we always load the damned sensor shape.
	// ONLY if we are running it in logistics.  DO NOT DELETE THESE IN THE MIDDLE OF A MISSION!!!
	if (!bIsInMission)
	{
		AppearanceTypeList::destroySensorShapes();
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

	if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
	{
		bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info);
		if (f) { MC2_LOG(Ui, Info, "[PREVIEW] SimpleCamera::render pObject={:016X} bIsComponent={:d}", mc2::fmt::as_ptr(static_cast<void*>(pObject.get())), mc2::fmt::as_i32(static_cast<int>(bIsComponent)));   }
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
		if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
		{
			bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info);
			if (f) { MC2_LOG(Ui, Info, "[PREVIEW] gos_GetViewport viewMul={:.1f},{:.1f} viewAdd={:.1f},{:.1f} EnvScreen={:d},{:d} bounds=[{:.1f},{:.1f},{:.1f},{:.1f}]", viewMulX, viewMulY, viewAddX, viewAddY, mc2::fmt::as_i32(Environment.screenWidth), mc2::fmt::as_i32(Environment.screenHeight), bounds[0], bounds[1], bounds[2], bounds[3]);   }
		}

		//--------------------------------------------------------
		// Get new viewport values to scale stuff.  No longer uses
		// VFX stuff for this.  ALL GOS NOW!
			screenResolution.x = viewMulX;
			screenResolution.y = viewMulY;
			calculateProjectionConstants();
		
			tglView.SetViewport(viewMulX,viewMulY,viewAddX,viewAddY);

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
			// SimpleCamera overrides the base render: capture its own projection
			// before drawing. A preview never changes the mission camera's values.
			tglView.SetCameraMatrices(cameraOrigin, cameraToClip);
			if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
			{
				bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info);
				if (f) {
					MC2_LOG(Ui, Info, "[PREVIEW] cameraToClip=[{:.3f} {:.3f} {:.3f} {:.3f} / {:.3f} {:.3f} {:.3f} {:.3f} / {:.3f} {:.3f} {:.3f} {:.3f} / {:.3f} {:.3f} {:.3f} {:.3f}]", cameraToClip[0][0], cameraToClip[1][0], cameraToClip[2][0], cameraToClip[3][0], cameraToClip[0][1], cameraToClip[1][1], cameraToClip[2][1], cameraToClip[3][1], cameraToClip[0][2], cameraToClip[1][2], cameraToClip[2][2], cameraToClip[3][2], cameraToClip[0][3], cameraToClip[1][3], cameraToClip[2][3], cameraToClip[3][3]);

				}
			}
			theClipper->StartDraw(cameraOrigin, cameraToClip, fColor, &fColor, default_state, &z);
			MidLevelRenderer::GOSVertex::farClipReciprocal = (1.0f-cameraToClip[2][2])/cameraToClip[2][3];

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
			// PREVIEW-DRAWOLDWAY-1 (THE blank-panel root cause): with drawOldWay
			// false, TG_Shape::Render does NOT draw — it defers every triangle
			// into mc2::renderSubmissions().addVertices() buckets (tgl.cpp:3252+), which
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
			// Mechs, vehicles and components draw through TG_MultiShape::PreviewDraw (preview_draw.cpp).
			TG_MultiShape* previewShape = nullptr;
			Mech3DAppearance* labMech = nullptr;	// worldless panel mech: candidate for the GPU material path
			if ( auto* mech = dynamic_cast<Mech3DAppearance*>(pObject.get()) )
			{
				previewShape = mech->previewShape();
				if ( isPanelPreview && ( mission == nullptr || !mission->isActive() ) )
					labMech = mech;
			}
			else if ( auto* bldg = dynamic_cast<BldgAppearance*>(pObject.get()) )
				previewShape = bldg->previewShape();
			else if ( auto* gv = dynamic_cast<GVAppearance*>(pObject.get()) )
				previewShape = gv->previewShape();
			if ( previewShape )
			{
				// Project with the same panel offset update() centres the shape with.
				const float offsetX = (bounds[2] + bounds[0] - viewMulX) / 2.f + fudgeX;
				const float offsetY = (bounds[1] + bounds[3] - viewMulY) / 2.f + fudgeY;
				tglView.SetViewport(viewMulX,viewMulY,offsetX,offsetY);
				auto view = getViewUniforms();
				if ( framed_ )
				{
					// Fill (cool, low, from the opposite side of the key) and rim (from behind, above).
					auto addLight = [&view]( float pitchDeg, float yawDeg, DWORD argb ) {
						glm::mat4 m;
						mc2::math::buildTranslation( m, glm::vec3( 0.0f, 0.0f, 0.0f ) );
						mc2::math::buildRotation( m, mc2::math::Euler( pitchDeg * DEGREES_TO_RADS, yawDeg * DEGREES_TO_RADS, 0.0f ) );
						view.lights.push_back( { TG_LIGHT_INFINITE, argb, m } );
					};
					addLight( 20.f, 0.f, 0xff5a6a80 );
					addLight( 20.f, 180.f, 0xffa0a8b8 );
					g_previewCaptureBounds = true;
					g_previewBoundsMinX = g_previewBoundsMinY = 1e9f;
					g_previewBoundsMaxX = g_previewBoundsMaxY = -1e9f;
				}
				bool gpuDraws = false;
				// Contact-shadow anchor: the mech's smoothed feet centre and height from the last frame's projected bounds.
				if ( framed_ && drawn_[3] - drawn_[1] > 8.f )
				{
					const float cx = ( drawn_[0] + drawn_[2] ) * 0.5f, cy = drawn_[3], hh = drawn_[3] - drawn_[1];
					const float k = haveGround_ ? 0.08f : 1.f;
					groundC_[0] += ( cx - groundC_[0] ) * k; groundC_[1] += ( cy - groundC_[1] ) * k; groundH_ += ( hh - groundH_ ) * k;
					haveGround_ = true;
				}
				if ( labMech && postMechPreviewGpu( *this, labMech, offsetX, offsetY, viewMulX, viewMulY,
				                                   haveGround_ ? groundC_[0] : 0.f, groundC_[1], haveGround_ ? groundH_ : 0.f ) )
					gpuDraws = mc2_vk_mech_preview_drew_last() != 0;
				// With the GPU path drawing, PreviewDraw only projects (the framing bounds below).
				g_previewBoundsOnly = gpuDraws;
				previewShape->PreviewDraw(view);
				g_previewBoundsOnly = false;
				if ( framed_ )
				{
					g_previewCaptureBounds = false;
					drawn_[0] = g_previewBoundsMinX; drawn_[1] = g_previewBoundsMinY;
					drawn_[2] = g_previewBoundsMaxX; drawn_[3] = g_previewBoundsMaxY;
					haveDrawn_ = true;
				}
			}
			else
				pObject->render();
			if ( isPanelPreview )
				drawOldWay = savedDrawOldWay;

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
			bool worldScenePresent = (mission != nullptr);
			{
				static int s_forceDrain = -1;
				if (s_forceDrain < 0) {
					s_forceDrain = false;
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
				mc2::VkSceneRenderer::renderLists(mc2::renderSubmissions());
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
    const uint64_t tex=mc2vk::uiTextureServiceReady() ? mc2_vk_preview_texture() : 0;
	const float u1 = bounds[0] / 800.f;
	const float u2 = bounds[2] / 800.f;
	const float v1 = 1.f - bounds[1] / 600.f;   // top    -> flipped
	const float v2 = 1.f - bounds[3] / 600.f;   // bottom -> flipped
	if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
	{
		bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info);
		if (f) {
			MC2_LOG(Ui, Info, "[PREVIEW] drawPreviewToPanel tex={:d} panel=[{:.1f},{:.1f},{:.1f},{:.1f}] uv=[{:.3f},{:.3f},{:.3f},{:.3f}] readyForUi={:d}", mc2::fmt::as_u64(tex), panelX, panelY, panelW, panelH, u1, v1, u2, v2, mc2::fmt::as_i32(static_cast<int>(GuiRuntime::IsReadyForUiText())));

		}
	}
	if ( !tex )
		return;

	bool ok = GuiRuntime::DrawUiImage( tex, panelX, panelY, panelW, panelH, u1, v1, u2, v2, 0xffffffff );
	if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
	{
		bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info);
		if (f) { MC2_LOG(Ui, Info, "[PREVIEW] DrawUiImage returned {:d}", mc2::fmt::as_i32(static_cast<int>(ok)));   }
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
		// The scope keeps calculateProjectionConstants off the native-aspect path (camera_matrix.cpp).
		MechPreviewRenderScope _previewScope;

		turn++;			//Must increment this now or matrices NEVER change!!

		mc2::VkSceneRenderer::beginFrame();

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
		

			tglView.SetViewport(viewMulX,viewMulY, offsetX, offsetY);

		useShadows = 0;
		oldCam = eye;
		eye = this;
		Camera::update();

		if ( framed_ )
		{
			projectionAngle = framedPitch_;
			if ( haveDrawn_ && fitIters_ > 0 )
			{
				haveDrawn_ = false;
				--fitIters_;
				const float rw = bounds[2] - bounds[0], rh = bounds[3] - bounds[1];
				const float w = drawn_[2] - drawn_[0], h = drawn_[3] - drawn_[1];
				if ( w > 1.f && h > 1.f )
				{
					// Size ~ 1/tan(fov/2) at a fixed distance (a long lens: moving the camera in distorts the
					// model and pushes it into the near plane); then centre across, feet just above the bottom edge.
					const float s = std::clamp( std::max( h / ( fillFrac_ * rh ), w / ( 0.94f * rw ) ), 0.5f, 2.f );
					framedFov_ = 2.f * std::atan( std::tan( framedFov_ * 0.5f * DEGREES_TO_RADS ) * s ) / DEGREES_TO_RADS;
					fudgeX += ( bounds[0] + bounds[2] ) * 0.5f - ( drawn_[0] + drawn_[2] ) * 0.5f;
					fudgeY += bounds[3] - rh * ( 1.f - fillFrac_ ) * 0.35f - drawn_[3];
				}
			}
			const float sharedAlt = AltitudeTight;
			AltitudeTight = framedAlt_;
			ZoomTight();
			AltitudeTight = sharedAlt;
			camera_fov = framedFov_;
			cosHalfFOV = std::cos( camera_fov * DEGREES_TO_RADS );
		}
		else
			ZoomTight();

		pObject->recalcBounds();
		pObject->scale(shapeScale);
		if ( framed_ )
		{
			// Idle turntable once the fit has settled; the clock keeps running while paused so a drag does not bank a jump.
			const double now = std::chrono::duration<double>( std::chrono::steady_clock::now().time_since_epoch() ).count();
			if ( fitIters_ == 0 && !turntablePaused_ && turntableLastT_ > 0.0 )
				rotation = std::fmod( rotation + 10.f * static_cast<float>( std::min( now - turntableLastT_, 0.1 ) ), 360.f );
			turntableLastT_ = now;
			pObject->rotation = this->rotation;
		}

		// we don't want to center around the feet
		glm::vec3 mechPos = pObject->getRootNodeCenter();
		mechPos.x = -mechPos.x/2.f;
		float tmp = -mechPos.y/2.f;
		mechPos.y = -mechPos.z/2.f;
		mechPos.z = tmp;

		float rotation = frameLength * rotationIncrement + pObject->rotation;

		pObject->setObjectParameters(mechPos, rotation, 0, 0, 0);
	
		pObject->update();
		pObject->setVisibility(true,true);

		if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
		{
			bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info);
			if (f) { MC2_LOG(Ui, Info, "[PREVIEW] update bounds=[{:.0f},{:.0f},{:.0f},{:.0f}] viewMul={:.0f},{:.0f} off={:.1f},{:.1f} scale={:.3f} Alt={:.0f} mechPos={:.1f},{:.1f},{:.1f}", bounds[0], bounds[1], bounds[2], bounds[3], viewMulX, viewMulY, offsetX, offsetY, shapeScale, AltitudeTight, mechPos.x, mechPos.y, mechPos.z);   }
		}

		eye = oldCam;

	}

	return 0;
}

// File name without directory or extension (the _splitpath fname part).
static std::string stemOfPath(mc2::czstring fileName)
{
	std::string nativeName(fileName);
#ifndef _WIN32
	std::replace(nativeName.begin(), nativeName.end(), '\\', '/');
#endif
	std::string stem = std::filesystem::path(nativeName).filename().string();
	if (const auto dot = stem.rfind('.'); dot != std::string::npos)
		stem.resize(dot);
	return stem;
}

void SimpleCamera::setMech(mc2::czstring fileName, long baseColor, long highlight1, long highlight2 )
{
	ZoneScopedN("SimpleCamera::setMech");
	this->pushContext();

	shapeScale = 0.0f;

	bIsComponent = 0;

	fudgeX = 5;
	fudgeY = 10;

	AltitudeTight = 650;

	// sebi: added this conition ecause  appearanceTypeList used inside destructor
	releaseObject();

	if ( !appearanceTypeList )
		Mission::initBareMinimum();

	rotationIncrement = 0;
	

	pObject = nullptr;

	if ( !fileName )
	{
//		allNormal();
		this->popContext();
		return;
	}

	std::string NoPathFileName = stemOfPath(fileName);

	char testName[256];
	strcpy( testName, NoPathFileName.c_str() );
	strcat( testName, "enc" );

	FullPathFileName path;
	path.init( tglPath, testName, ".ini" );

	//MUST ALWAYS CALL GET, EVEN IF WE HAVE AN APPEARANCE TYPE OR REFERENCE COUNT DOES NOT INCREASE!
	Mech3DAppearanceType* appearanceType = nullptr;
	
	if ( fileExists( path ) )
		appearanceType = static_cast<Mech3DAppearanceType*>(appearanceTypeList->getAppearance( MECH_TYPE << 24, static_cast<char*>(testName) ));
	else
		appearanceType = static_cast<Mech3DAppearanceType*>(appearanceTypeList->getAppearance( MECH_TYPE << 24, NoPathFileName.data() ));

	// A mod chassis with no tgl ini (or an empty base shape) leaves no object, so
	// hasObject() stays false and the mech lab keeps its silhouette statics.
	if ( !appearanceType || appearanceType->baseShapeLoadFailed )
	{
		if ( appearanceType )
			appearanceTypeList->removeAppearance( appearanceType );
		this->popContext();
		return;
	}

	pObject = std::make_unique<Mech3DAppearance>();
	pObject->init( appearanceType );
	pObject->setGestureGoal(2);
	pObject->resetPaintScheme( highlight1, highlight2, baseColor );
	pObject->rotation = rotation;

	activate();
		
	setPosition(position, 0);
	ZoomTight();

	this->popContext();
}

void SimpleCamera::setVehicle(mc2::czstring fileName,long base, long highlight, long h2)
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
	
	// sebi: added this conition ecause  appearanceTypeList used inside destructor
	releaseObject();

	pObject = nullptr;

	if ( !fileName )
	{
		this->popContext();
		return;
	}

	std::string NoPathFileName = stemOfPath(fileName);

	char testName[256];
	strcpy( testName, fileName );
	strcat( testName, "enc" );

	FullPathFileName path;
	path.init( tglPath, testName, ".ini" );

	//MUST ALWAYS CALL GET, EVEN IF WE HAVE AN APPEARANCE TYPE OR REFERENCE COUNT DOES NOT INCREASE!
	GVAppearanceType* appearanceType = nullptr;
	
	if ( fileExists( path ) )
		appearanceType = static_cast<GVAppearanceType*>(appearanceTypeList->getAppearance( GV_TYPE << 24, static_cast<char*>(testName) ));
	else
		appearanceType= static_cast<GVAppearanceType*>(appearanceTypeList->getAppearance( GV_TYPE << 24, NoPathFileName.data() ));

	pObject = std::make_unique<GVAppearance>();	
	pObject->init( appearanceType );
	pObject->setGestureGoal(2);
	pObject->resetPaintScheme(base, highlight, h2);
	pObject->rotation = rotation;

	activate();
		
	setPosition(position);
	ZoomTight();

	this->popContext();
}


void SimpleCamera::setComponent(mc2::czstring fileName )
{
	this->pushContext();

	shapeScale = 0.0f;

	bIsComponent = 1;

	AltitudeTight = 580;

	fudgeX = 0;
	fudgeY = 0;

	
	if ( !appearanceTypeList )
		Mission::initBareMinimum();

	
	// sebi: added this conition ecause  appearanceTypeList used inside destructor
	releaseObject();

	pObject = nullptr;

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
	BldgAppearanceType* appearanceType = nullptr;
	if ( fileExists( path ) )
	{
		appearanceType = static_cast<BldgAppearanceType*>(appearanceTypeList->getAppearance( BLDG_TYPE << 24, static_cast<char*>(testName) ));
	}
	else
		appearanceType = static_cast<BldgAppearanceType*>(appearanceTypeList->getAppearance( BLDG_TYPE << 24, const_cast<char*>(fileName) ));

	//MUST ALWAYS CALL GET, EVEN IF WE HAVE AN APPEARANCE TYPE OR REFERENCE COUNT DOES NOT INCREASE!
	 

	pObject = std::make_unique<BldgAppearance>();	
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

void SimpleCamera::setBuilding( mc2::czstring pBuilding )
{
	shapeScale = 0.0f;

	setComponent( pBuilding );
	AltitudeTight = 800;
	bIsComponent = 0;
}

void SimpleCamera::setObject( mc2::czstring pFileName, long type, long base, long highlight, long h2 )
{
	if ( !pFileName || !strlen( pFileName ) )
	{
		// Empty/NULL filename = no model. Free existing object and return cleanly.
		releaseObject();
		pObject = nullptr;
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

