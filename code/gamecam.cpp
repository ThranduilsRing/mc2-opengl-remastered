//---------------------------------------------------------------------------
//
// GameCam.h -- File contains the Game camera class definitions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef GAMECAM_H
#include"gamecam.h"
#include"tex_resolve_table.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef WEATHER_H
#include"weather.h"
#endif

#include "ground_contact_blob.h"  // GROUND-CONTACT-BLOB-1

#include<mlr/mlr.hpp>
#include <tracy/Tracy.hpp>
#include "cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline (RAII scope)
#include "../GameAdapters/StaticPropRenderAdapter.h"  // M1 CI-gate: firewall bridge for frameBegin()
#include "../GameAdapters/VegetationAdapter.h"         // vegetation card flush (post-renderLists)
#include "particles/batcher.h"  // GPU particle batcher flush (Stage 2' and beyond)
// DRYRUN-OBSERVE-COVERAGE-1: observe-only VFX pass note. render_contract.h pulls
// RenderCore/RenderPassContract.h, which the mc2/code TU's include path can't resolve
// (root not on its -I list). Use the thin extern "C" shim defined in render_contract.cpp
// instead of the full header — same noteRenderPass(ParticleEffect) effect, no draw change.
extern "C" void mc2_note_particle_effect_pass();
// VFX-FBO-ONLY-VALIDATE-1: top-level executor ownership of the VFX/particle flush.
// FBO-only validate (no ambient row). No-op when MC2_FRAMEGRAPH_EXECUTOR unset (byte-identical OFF).
// Same extern "C" shim bridge as the note above (mc2/code TU can't include render_contract.h).
extern "C" void mc2_vfx_pass_begin();
extern "C" void mc2_vfx_pass_end();
#include "../GameOS/gameos/gos_particle_bridge.h"  // B2 P1: camera basis bridge
#include "../GameOS/gameos/debug_renderer.h"
#include "../GuiRuntime/EditorInspector.h"  // IMG-INSPECT-3 flushDebugHighlight
#include "../GameAdapters/SkyRenderAdapter.h"  // HDRI-SKY-1: firewall-clean sky rendering
#include "../GameOS/gameos/view_uniforms_gl.h"  // F1-3A: ViewUniforms UBO upload
#include "../GameOS/gameos/gos_static_prop_killswitch.h"  // F1-3C: gos_GetTerrainMVPMat4 compare probe
#include "tacticaloverview.h"  // Tactical Overview diagnostic HUD + blend state
// WATER-TERRAIN-REFLECTION-1: mirrored terrain reflection pass (engine-side;
// raw GL lives in gos_terrain_indirect.cpp, this is a plain call -> firewall OK).
namespace gos_terrain_indirect { void RenderWaterReflectionPass(); }

#include <cmath>
#include <cstdlib>
#include "../RenderCore/SceneLighting.h"  // SCENE-LIGHTING-STATE-1 canonical mirror
extern float g_iblShStrength;             // static-prop SH ambient slider (gos_static_prop_batcher.cpp)

//---------------------------------------------------------------------------
CameraPtr eye = NULL;

// SCENE-LIGHTING-STATE-1: capture a canonical mirror of today's fragmented
// lighting state and emit one trace line per ~300-frame window. Read-only —
// changes no rendering. Gates: MC2_SCENE_LIGHTING_TRACE=1 (log),
// MC2_SCENE_LIGHTING_ASSERT=1 (additionally WARN, never abort, when the camera
// sun and terrain sun diverge — that divergence is EXPECTED today and is the
// headline fragmentation this slice surfaces). No-op when neither gate is set.
void mc2SceneLightingTrace()
{
    const bool doTrace  = (getenv("MC2_SCENE_LIGHTING_TRACE")  != nullptr);
    const bool doAssert = (getenv("MC2_SCENE_LIGHTING_ASSERT") != nullptr);
    if (!doTrace && !doAssert) return;

    static unsigned s_frame = 0;
    ++s_frame;
    if ((s_frame % 300u) != 1u) return;   // self-throttle: one window per ~300 frames
    if (!eye) return;

    SceneLighting s;
    s.sun_dir[0] = eye->lightDirection.x;
    s.sun_dir[1] = eye->lightDirection.y;
    s.sun_dir[2] = eye->lightDirection.z;
    DWORD sc = eye->getLightColor(0);
    s.sun_color[0] = float((sc >> 16) & 0xFFu) / 255.0f;
    s.sun_color[1] = float((sc >>  8) & 0xFFu) / 255.0f;
    s.sun_color[2] = float( sc        & 0xFFu) / 255.0f;
    // Intensity proxy: the aRGB returned by getLightColor already has the light
    // intensity folded in (tgl.h aRGB = intensity-scaled color), and Camera's raw
    // intensity field is protected. Report the sun color luminance as the proxy.
    s.sun_intensity = 0.299f*s.sun_color[0] + 0.587f*s.sun_color[1] + 0.114f*s.sun_color[2];
    DWORD ac = eye->getLightColor(1);
    s.ambient_color[0] = float((ac >> 16) & 0xFFu) / 255.0f;
    s.ambient_color[1] = float((ac >>  8) & 0xFFu) / 255.0f;
    s.ambient_color[2] = float( ac        & 0xFFu) / 255.0f;

    float tx = 0.0f, ty = 0.0f, tz = 1.0f;
    gos_GetTerrainLightDir(&tx, &ty, &tz);
    s.terrain_light_dir_current[0] = tx;
    s.terrain_light_dir_current[1] = ty;
    s.terrain_light_dir_current[2] = tz;
    s.shadow_sun_dir[0] = -tx;   // derived: gameos_graphics.cpp:8834 passes -terrainLightDir
    s.shadow_sun_dir[1] = -ty;
    s.shadow_sun_dir[2] = -tz;

    s.world_light_count  = -1;                       // slot capacity (Camera::numLights) is protected; ~MAX, not exposed
    s.active_light_count = (int)eye->getNumLights();  // public accessor returns numActiveLights (frustum-filtered)
    s.sky_number   = gos_GetSkyNumber();
    s.ibl_sh_present = (g_iblShStrength > 0.0f);

    // Parity headline: angle between the OBJECT/camera sun and the TERRAIN sun.
    float lc = sqrtf(s.sun_dir[0]*s.sun_dir[0] + s.sun_dir[1]*s.sun_dir[1] + s.sun_dir[2]*s.sun_dir[2]);
    float lt = sqrtf(tx*tx + ty*ty + tz*tz);
    float dtheta = -1.0f;
    if (lc > 1e-4f && lt > 1e-4f) {
        float d = (s.sun_dir[0]*tx + s.sun_dir[1]*ty + s.sun_dir[2]*tz) / (lc * lt);
        d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
        dtheta = acosf(d) * 57.29578f;
    }
    const bool sunDiverged = (dtheta > 5.0f);
    static unsigned s_sunMismatch = 0;
    if (sunDiverged) ++s_sunMismatch;

    if (doTrace) {
        fprintf(stderr,
            "[SCENE_LIGHTING] frame=%u sun_cam=(%.2f,%.2f,%.2f) sun_terrain=(%.2f,%.2f,%.2f) "
            "shadow=(%.2f,%.2f,%.2f) dtheta_cam_terrain=%.1fdeg sun_rgb=(%.2f,%.2f,%.2f) intensity=%.2f "
            "ambient_rgb=(%.2f,%.2f,%.2f) worldLights=%d active=%d sky=%d ibl_sh=%d sun_match=%d cum_sun_mismatch=%u\n",
            s_frame, s.sun_dir[0], s.sun_dir[1], s.sun_dir[2], tx, ty, tz,
            s.shadow_sun_dir[0], s.shadow_sun_dir[1], s.shadow_sun_dir[2], dtheta,
            s.sun_color[0], s.sun_color[1], s.sun_color[2], s.sun_intensity,
            s.ambient_color[0], s.ambient_color[1], s.ambient_color[2],
            s.world_light_count, s.active_light_count, s.sky_number, s.ibl_sh_present ? 1 : 0,
            sunDiverged ? 0 : 1, s_sunMismatch);
        fflush(stderr);
    }
    if (doAssert && sunDiverged) {
        fprintf(stderr,
            "[SCENE_LIGHTING] WARN frame=%u camera_sun vs terrain_sun diverge by %.1fdeg "
            "(EXPECTED today: object path uses the corrected terrain sun because worldLights[0] "
            "was ~90deg off -- see static_prop.vert STATIC-PROP-TERRAIN-SUN-DIFFUSE). Not a regression.\n",
            s_frame, dtheta);
        fflush(stderr);
    }
}

extern bool useShadows;
extern bool useFog;
extern bool DisplayCameraAngle;

extern MidLevelRenderer::MLRClipper * theClipper;

#define MAX_SHADOW_PITCH_CHANGE	(5.0f)

extern bool drawOldWay;

extern bool useNonWeaponEffects;
GenericAppearance *theSky = NULL;

// DEBUG-STATE-ASSETS: globals read by GameOS/gameos/debug_state_dump.cpp via extern.
// LINK-CONFIG-FIX: definitions moved to GameOS/gameos/dbg_asset_globals.cpp so that
// EditRel (gameos_editor) and the data_tools (gameos) — which link a gameos-family
// lib but not gamecam.cpp — resolve them. Declared extern here; set below at
// mission-load / loadscreen-pick.
extern long  g_dbgSkyNumber;
extern char  g_dbgLoadScreen[64];
//---------------------------------------------------------------------------
void GameCamera::destroy (void)
{
	if (theSky)
	{
		delete theSky;
		theSky = NULL;
	}

	if (compass)
	{
		delete compass;
		compass = NULL;
	}

	Camera::destroy();
}

//---------------------------------------------------------------------------
void GameCamera::render (void)
{
	//------------------------------------------------------
	// At present, these actually draw.  Later they will 
	// add elements to the draw list and sort and draw.
	// The later time has arrived.  We begin sorting immediately.
	// NO LONGER NEED TO SORT!
	// ZBuffer time has arrived.  Share and Enjoy!
	// Everything SIMPLY draws at the execution point into the zBuffer
	// at the correct depth.  Miracles occur at that point!
	// Big code change but it removes a WHOLE bunch of code and memory!
	
	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);

	MidLevelRenderer::MLRState default_state;
	default_state.SetBackFaceOn();
	default_state.SetDitherOn();
	default_state.SetTextureCorrectionOn();
	default_state.SetZBufferCompareOn();
	default_state.SetZBufferWriteOn();

	default_state.SetFilterMode(MidLevelRenderer::MLRState::BiLinearFilterMode);

	float z = 1.0f;
	Stuff::RGBAColor fColor;
	fColor.red = ((fogColor >> 16) & 0xff);
	fColor.green = ((fogColor >> 8) & 0xff);
	fColor.blue = ((fogColor) & 0xff);

	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	screenResolution.x = viewMulX;
	screenResolution.y = viewMulY;
	calculateProjectionConstants();

	TG_Shape::SetViewport(viewMulX,viewMulY,viewAddX,viewAddY);

	userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);

	gos_TextSetRegion(viewAddX,viewAddY,viewMulX,viewMulY);
	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	screenResolution.x = viewMulX;
	screenResolution.y = viewMulY;
	calculateProjectionConstants();

	globalScaleFactor = getScaleFactor();
	globalScaleFactor *= viewMulX / 640.0;		//Scale Mechs to ScreenRES
	
	//-----------------------------------------------
	// Set Ambient for this pass of rendering	
	DWORD lightRGB = (ambientRed<<16)+(ambientGreen<<8)+ambientBlue;
		
	eye->setLightColor(1,lightRGB);
	eye->setLightIntensity(1,1.0);

	MidLevelRenderer::PerspectiveMode = usePerspective;
	{
		// F3 CPU projection cost-baseline: mlr_total bucket — wraps StartDraw
		// (per-frame MLR setup). RenderNow gets a second scope further below.
		::mc2_cpu_proj_cost::Scope _f3_mlr_start_scope(
		    ::mc2_cpu_proj_cost::BUCKET_MLR_TOTAL);
		theClipper->StartDraw(cameraOrigin, cameraToClip, fColor, &fColor, default_state, &z);
	}
	MidLevelRenderer::GOSVertex::farClipReciprocal = (1.0f-cameraToClip(2, 2))/cameraToClip(3, 2);

	if (active && turn > 1)
	{
		ZoneScopedN("GameCamera::render activeScene");
		//----------------------------------------------------------
		// Turn stuff on line by line until perspective is working.

		// Compose terrainMVP: MC2 world coords -> GL clip coords
		{
			ZoneScopedN("Camera.BuildMVP");
			// F3 CPU projection cost-baseline: matrix_build site (a).
			::mc2_cpu_proj_cost::Scope _f3_mvp_scope(
			    ::mc2_cpu_proj_cost::BUCKET_MATRIX_BUILD);
			// F1 Stage A unified-projection: single producer call.
			// gos_SetWorldToClipGL composes nothing here — the matrix is
			// built once by Camera::worldToClipGL() (axisSwap *
			// worldToCamera * cameraToClip, with R-clipw polarity folded
			// into kAxisSwapMC2toGL). gos_SetWorldToClipGL handles the
			// column-major -> row-major repackage internally AND writes
			// the terrain_mvp_ cache so all gos_GetTerrainMVPMat4()
			// callers (CullUBO, mech-batcher, static-prop-batcher,
			// particle-bridge, etc.) inherit transparently.
			gos_SetWorldToClipGL(eye->worldToClipGL());

			// F1-4B: Fill vu and register EngineView unconditionally (pure CPU, no GL).
			// Upload UBO only when MC2_VIEW_UNIFORMS gate is on.
			{
				RenderCore::ViewUniforms vu{};
				// Stuff::Matrix4D stores column-major (entries[c*4+r]).
				// ViewUniforms wants row-major (GL_FALSE upload convention).
				// Transpose: row-major[r*4+c] = col-major[c*4+r].
				auto stuffToRowMajor = [](const Stuff::Matrix4D& m, float out[16]) {
					const float* col = m.entries;
					for (int r = 0; r < 4; ++r)
						for (int c = 0; c < 4; ++c)
							out[r * 4 + c] = col[c * 4 + r];
				};
				stuffToRowMajor(eye->worldToClipGL(), vu.worldToClipGL);
				stuffToRowMajor(eye->worldToViewGL(), vu.worldToViewGL);
				const Stuff::Vector3D orig = eye->cameraOriginGL();
				vu.cameraWorldPos[0] = orig.x;
				vu.cameraWorldPos[1] = orig.y;
				vu.cameraWorldPos[2] = orig.z;
				vu.cameraWorldPos[3] = 1.0f;

				// Always register EngineView (GL-free, safe unconditionally).
				// setCurrentView is store-only since F1-4B.
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

				// F1-3D flip: upload UBO by default; kill-switch MC2_VIEW_UNIFORMS=0.
				// Original F1-3B/F1-3C gate required explicit =1 (opt-in). That left
				// the UBO unbound when the env var was absent, so any shader compiled
				// with MC2_USE_VIEW_UNIFORMS read from an unbound/zero UBO and drew
				// props off-screen. Matches the s_viewUniformsShaderEnabled flip in
				// gos_static_prop_batcher.cpp.
				{
					static const char* s_vuEnv = std::getenv("MC2_VIEW_UNIFORMS");
					if (!(s_vuEnv && s_vuEnv[0] == '0')) {
						RenderCore::uploadViewUniforms(vu);

						// F1-3C: compare ViewUniforms.worldToClipGL against legacy terrain MVP upload
						{
							static int s_vuCompareFrame = 0;
							++s_vuCompareFrame;
							const float* legacy = gos_GetTerrainMVPMat4();
							float maxDiff = 0.0f;
							if (legacy) {
								for (int i = 0; i < 16; ++i) {
									float d = vu.worldToClipGL[i] - legacy[i];
									if (d < 0.0f) d = -d;
									if (d > maxDiff) maxDiff = d;
								}
							}
							const int ok = (legacy != nullptr) && (maxDiff <= 1e-5f) ? 1 : 0;
							if (s_vuCompareFrame <= 10 || ok == 0) {
								fprintf(stderr, "[VIEW_UNIFORMS v1] compare frame=%d max_diff=%.6f ok=%d\n",
								        s_vuCompareFrame, maxDiff, ok);
								fflush(stderr);
							}
							// XFORM-CONVENTION-HARNESS-1: promote the F1-3C parity probe to a
							// HARD invariant when MC2_XFORM_PARITY_FATAL is set. Default OFF =
							// byte-identical to the log-only behavior above. When the gate is
							// set AND the ViewUniforms UBO payload diverges from the legacy
							// terrain MVP, abort instead of silently rendering with a mismatched
							// clip-space transform (the exact regression class this slice locks).
							{
								static const char* s_xformFatalEnv = std::getenv("MC2_XFORM_PARITY_FATAL");
								static const bool s_xformFatal = (s_xformFatalEnv && s_xformFatalEnv[0] == '1');
								if (s_xformFatal && ok == 0) {
									fprintf(stderr,
									        "[XFORM_PARITY_FATAL] ViewUniforms.worldToClipGL diverged from legacy "
									        "terrain MVP: frame=%d max_diff=%.6f (>1e-5). MC2_XFORM_PARITY_FATAL=1 -> abort.\n",
									        s_vuCompareFrame, maxDiff);
									fflush(stderr);
									abort();
								}
							}
						}
					}
				}
			}

			// VANISH-PROBE-1 (env MC2_VANISH_PROBE): project real persistent
			// static-prop origins through worldToClipGL at the LIVE camera angle.
			// Rotate to the trees-gone view; if a prop that is clearly in frame
			// reports inFrustum=0 (|x|>w, |y|>w, or z outside [0,w]) -> the prop
			// vanishes because the shared camera projection clips it (depth/
			// reverse-Z/near-plane), NOT cull/store (storeTotal stays high) and
			// NOT matrix divergence (VIEW_UNIFORMS compare stays ok=1). One-shot
			// per ~60 frames; coarse, off by default.
			{
				static const bool s_vanishProbe = (getenv("MC2_VANISH_PROBE") != nullptr);
				if (s_vanishProbe) {
					extern uint32_t gos_ProbeStaticInstanceCount();
					extern bool     gos_ProbeStaticInstanceWorld(uint32_t, float[3]);
					static int s_vpFrame = 0;
					++s_vpFrame;
					if ((s_vpFrame % 60) == 1) {
						const uint32_t n = gos_ProbeStaticInstanceCount();
						const uint32_t step = (n > 0u) ? (n / 6u + 1u) : 1u;
						int inCount = 0, total = 0;
						for (uint32_t i = 0; i < n; i += step) {
							float wp[3];
							if (!gos_ProbeStaticInstanceWorld(i, wp)) break;
							Stuff::Vector4D win, wout;
							win.x = wp[0]; win.y = wp[1]; win.z = wp[2]; win.w = 1.0f;
							wout.Multiply(win, eye->worldToClipGL());  // row-vec * matrix (txmmgr convention)
							const float w = wout.w;
							const bool inFr = (w > 0.0f) &&
								(wout.x >= -w && wout.x <= w) &&
								(wout.y >= -w && wout.y <= w) &&
								(wout.z >= 0.0f && wout.z <= w);
							if (inFr) ++inCount;
							++total;
							fprintf(stderr,
								"[VANISH_PROBE] frame=%d inst=%u/%u world=(%.0f,%.0f,%.0f) "
								"clip=(%.1f,%.1f,%.1f,w=%.1f) ndc=(%.3f,%.3f,%.3f) inFrustum=%d\n",
								s_vpFrame, i, n, wp[0], wp[1], wp[2],
								wout.x, wout.y, wout.z, w,
								(w != 0.0f ? wout.x / w : 0.0f),
								(w != 0.0f ? wout.y / w : 0.0f),
								(w != 0.0f ? wout.z / w : 0.0f), inFr ? 1 : 0);
						}
						fprintf(stderr, "[VANISH_PROBE] frame=%d storeCount=%u sampled=%d inFrustum=%d\n",
							s_vpFrame, n, total, inCount);
						fflush(stderr);
					}
				}
			}

			// Camera position in MC2 world space for TCS distance LOD
			Stuff::Vector3D camOrig = getCameraOrigin();
			gos_SetTerrainCameraPos(camOrig.x, camOrig.y, camOrig.z);

			// Light direction in raw MC2 world space (x, y, elevation)
			// NOT swizzled — fragment shader normals are in tangent space where Z = up,
			// which matches raw MC2 coords (Z = elevation).
			gos_SetTerrainLightDir(lightDirection.x, lightDirection.y, lightDirection.z);

			// SCENE-LIGHTING-STATE-1: mirror + parity trace (gated, self-throttled, no-op default).
			mc2SceneLightingTrace();

			#undef WTC
		}

		if (Environment.Renderer != 3)
		{
			ZoneScopedN("GameCamera::render sky");
			if (GameAdapters::Sky::isHdriReady()) {
				// Both matrices column-major; LinearMatrix4D is 12-float affine
				// and Matrix4D is 16-float full 4x4 — renderHdri only
				// reads indices 0..10 (upper 3x3 of view) and the full 16 of proj.
				const Stuff::LinearMatrix4D& view = eye->worldToCameraGL();
				const Stuff::Matrix4D& proj = eye->cameraToClipGL_const();
				GameAdapters::Sky::renderHdri(
					reinterpret_cast<const float*>(&view.entries[0]),
					reinterpret_cast<const float*>(&proj.entries[0])
				);
			}
			// else: black sky baseline (no fallback to theSky per SPEC).
		}

		{
			ZoneScopedN("GameCamera::render terrain");
			GameAdapters::StaticProp::frameBegin();  // Stage 3.C: reset live-instance list
			land->render();								//render the Terrain
		}

		if (Environment.Renderer != 3)
		{
			// GROUND-CONTACT-BLOB-1: submitted BEFORE craterManager->render() so
			// blobs land first (under) in the shared un-depth-sorted TerrainDecal
			// batch -- footprints/craters composite on top of the contact disc,
			// not the other way around (recon landmine 2). Gate-off: no-op.
			ZoneScopedN("GameCamera::render groundContactBlobs");
			renderGroundContactBlobs();
		}

		if (Environment.Renderer != 3)
		{
			ZoneScopedN("GameCamera::render craters");
			craterManager->render();					//render the craters and footprints
		}

		{
			ZoneScopedN("GameCamera::render objects");
			// F3 CPU projection cost-baseline: mark we are inside the render
			// loop so eventdriven projectZ attribution can distinguish
			// render-time calls from AI/picking/input calls.
			::mc2_cpu_proj_cost::RenderLoopGuard _f3_render_guard;
			ObjectManager->render(true, true, true);	//render all other objects
		}

		{
			ZoneScopedN("GameCamera::render water");
			land->renderWater();						//Draw Water Last!
		}

		if (useShadows && Environment.Renderer != 3)
		{
			ZoneScopedN("GameCamera::render shadows");
			ObjectManager->renderShadows(true, true, true);
		}

		if (mission && mission->missionInterface)
		{
			ZoneScopedN("GameCamera::render drawVTOL");
			mission->missionInterface->drawVTOL();
		}

		if (!drawOldWay && !inMovieMode)
		{
			if (compass && (turn > 3) && drawCompass)
			{
				ZoneScopedN("GameCamera::render compass");
				compass->render(-1);		//Force this to zBuffer in front of everything
			}
		}

		// Terrain LOD chunk Phase 4 flush — submit GPU draw commands built in
		// Terrain::update(). No-op when MC2_TERRAIN_LOD_CHUNK is unset
		// (s_blockMeta is nullptr). Placed before renderLists() so chunk geometry
		// is fully drawn before post-process; after shadow pass so shadows resolve.
		// Mirrors the water fast path pattern (below renderLists) but uses depth
		// written by chunk draws instead of needing an already-populated depth buf.
		Terrain::flushDrawCommands();

		if (!drawOldWay)
		{
			ZoneScopedN("GameCamera::render textureManagerRenderLists");
			mcTextureManager->renderLists();			//This sends triangles down to the card.  All "rendering" to this point has been setting up tri lists
			endFrameTexResolve();              // close the per-frame window — clears frameActive,
			                                   // accumulates resolved-count, emits 600-frame summary
			                                   // when due. No-op when killswitch OFF or already inactive.

			// WATER-TERRAIN-REFLECTION-1 (Phase C1): fill the quarter-res water
			// reflection RT with mirrored terrain. After renderLists (main SOLID
			// draw done, atlases warm) and BEFORE water so C2 can sample it.
			// No-op unless MC2_WATER_REFLECTION_RT=1. Restores terrain MVP itself.
			gos_terrain_indirect::RenderWaterReflectionPass();

			// Stage 2 of renderWater architectural slice (CPU→GPU offload).
			// MUST run after renderLists so terrain has flushed before we
			// alpha-blend water on top. No-op when MC2_RENDER_WATER_FASTPATH
			// is unset; legacy water already drained inside renderLists().
			if (land) {
				ZoneScopedN("GameCamera::render waterFastPath");
				land->renderWaterFastPath();
				// VFX-CACHE-SYNC-1: the water fast path sets blend/cull/depth via
				// raw GL that bypasses the gos render-state cache; re-sync so the
				// next gos_SetRenderState isn't a stale no-op (mirrors mech batcher).
				gos_InvalidateRenderStateCache();
			}

			// Vegetation ground-patch flush — instanced flat quads, default-OFF
			// (MC2_VEGETATION_CARDS=1). After renderLists+water, before particles.
			// Matrix sourced inside flush via gos_GetTerrainMVPMat4() (terrain-chunk space).
			{
				ZoneScopedN("GameCamera::render vegetationFlush");
				float lx = 0.0f, ly = 0.0f, lz = 1.0f;
				gos_GetTerrainLightDir(&lx, &ly, &lz);
				const float terrainLightDir_4f[4] = { lx, ly, lz, 0.0f };
				const float missionTime = static_cast<float>(gos_GetElapsedTime());
				// Camera position in terrain-chunk space for distance fade + wind LOD.
				// Use orbit target (game world: x=east, y=north) — same frame as instance encode.
				// getCameraOrigin() returns Stuff/MLR (west,elev,north) — wrong axes for this.
				Stuff::Vector3D camTgt = getPosition();
				const float vegHalfMap = Terrain::worldUnitsMapSide * 0.5f;
				const float vegCamCX = camTgt.x - Terrain::mapTopLeft3d.x - vegHalfMap;
				const float vegCamCY = camTgt.y - Terrain::mapTopLeft3d.y + vegHalfMap;
				const float vegCamCZ = camTgt.z;
				GameAdapters::Vegetation::flush(terrainLightDir_4f, missionTime,
				                                vegCamCX, vegCamCY, vegCamCZ);
			}

			// GPU particle batcher flush — Stage 2' and beyond.
			// MUST run after renderLists() so the scene depth buffer is
			// populated before alpha-blended billboards composite on top
			// (memory/gpu_direct_renderer_bringup_checklist.md trap #6).
			// No-op when MC2_GPU_PARTICLES=0 (default ON since B3c-2; absent env var → enabled).
			// Stage 1' canary (hardcoded orange billboard) removed now
			// that real gosFX producers call BeginGroup+Emit via the
			// SpawnCard*/SpawnCardCloud paths.
			{
				ZoneScopedN("GameCamera::render particlesFlush");

				// B2 P1: publish current camera basis to particle bridge before flush.
				// cameraOrigin is the camera's world transform (LinearMatrix4D);
				// GetLocalRightInWorld / GetLocalUpInWorld return vectors in MC2/Stuff
				// world space (x=east, y=north, z=elevation).  Apply the same axis swap
				// used in particle_billboard.vert (GL_x=-Stuff_x, GL_y=Stuff_z, GL_z=Stuff_y)
				// so the bridge vectors land in the same space as worldPos in the shader.
				{
					Stuff::UnitVector3D stuffRight, stuffUp;
					cameraOrigin.GetLocalRightInWorld(&stuffRight);
					cameraOrigin.GetLocalUpInWorld(&stuffUp);
					float camRight[3] = { -stuffRight.x,  stuffRight.z,  stuffRight.y };
					float camUp[3]    = { -stuffUp.x,     stuffUp.z,     stuffUp.y    };
					gos_SetActiveCamera(camRight, camUp);
				}

				::mc2::particles::Batcher::Instance().ResolveTextures();  // resolve MLR->GOS after renderLists
				// DRYRUN-OBSERVE-COVERAGE-1: observe-only. ParticleEffect (VFX) has no
				// AmbientContract row and no declared FBO target -> ambient/FBO probe skips
				// it. Fires once per frame even on empty-particle frames (the pass was
				// entered). Gated MC2_GPU_PARTICLES (default ON). No draw change.
				mc2_note_particle_effect_pass();
				// VFX-FBO-ONLY-VALIDATE-1: top-level validate-only ownership (gate
				// MC2_FRAMEGRAPH_EXECUTOR). begin fires right after the note; the RAII
				// guard's dtor fires end on ALL exit paths of this flush scope, covering
				// both Batcher::Flush() and gos_tube_ribbon_flush_deferred() below.
				// FBO-only: MainColor (scene HDR FBO) is bound across the whole window,
				// incl. empty-particle frames. PIN: pure validate counters — no draw/state
				// change, no reorder; the flush body runs UNCHANGED between begin and end.
				mc2_vfx_pass_begin();
				struct TlVfxGuard_ { ~TlVfxGuard_() { mc2_vfx_pass_end(); } } _tlVfxGuard;
				::mc2::particles::Batcher::Instance().Flush();

				// TUBE-DEFERRED-FLUSH-1: drain the ribbon queue enqueued by
				// gosFX::Tube::Draw during the effect-render phase.  MUST run
				// here (post-renderLists) so the depth buffer is fully populated
				// before alpha-blended ribbons composite on top.  Same phase
				// rationale as Batcher::Flush() above.
				// No-op when MC2_VFX_ORACLE_TUBE is unset (queue stays empty).
				// Declared in gos_particle_bridge.h (already included above).
				gos_tube_ribbon_flush_deferred();

				gos_ClearActiveCamera();
			}
		}

		if (drawOldWay)
		{
			//Last thing drawn were shadows which are not Gouraud Shaded!!!
			// MLR to be "efficient" doesn't set this state by default at startup!
			gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		}

		{
			ZoneScopedN("GameCamera::render clipperRenderNow");
			// F3 CPU projection cost-baseline: mlr_total bucket — RenderNow
			// scope (joined with the StartDraw scope above into mlr_total).
			::mc2_cpu_proj_cost::Scope _f3_mlr_rendernow_scope(
			    ::mc2_cpu_proj_cost::BUCKET_MLR_TOTAL);
			// VFX-MESH-BRACKET-1: theClipper->RenderNow() draws gosFX MESH shapes
			// (gosFX::Shape::Draw -> gos_vfx_mesh_flush) — scene geometry that ran
			// OUTSIDE any PassIdentity: it fires AFTER the particle/VFX guard scope
			// above closed (line ~622). Account it under the same ParticleEffect
			// (VFX) pass with a second observe-only bracket around just RenderNow.
			// VFX is a SOFT pass (no AmbientContract row, no FBO contract to enforce
			// per the note above), so this ONLY makes the mesh draw accounted — no
			// GL state change, no draw reorder. note+begin/end mirror the particle
			// bracket at 599/607/608; the RAII guard fires end on all exit paths.
			// executorOwn* (via mc2_vfx_pass_*) are no-ops when
			// MC2_FRAMEGRAPH_EXECUTOR is unset -> byte-identical OFF.
			mc2_note_particle_effect_pass();
			mc2_vfx_pass_begin();
			struct MeshVfxGuard_ { ~MeshVfxGuard_() { mc2_vfx_pass_end(); } } _meshVfxGuard;
			theClipper->RenderNow();		//Draw the FX
		}


		if (useNonWeaponEffects)
		{
			ZoneScopedN("GameCamera::render weather");
			weather->render();				//Draw the weather
		}

		// DebugRenderer world primitives -- depth-tested, before post-process.
		// No-op when MC2_DEBUG_RENDERER is unset.
		{
			ZoneScopedN("GameCamera::render debugRendererFlushWorldPrims");
#ifdef MC2_IMGUI
			EditorInspector::flushDebugHighlight();  // IMG-INSPECT-3: queue highlight prims same-frame
#endif
			// TACTICAL-ARC-OVERLAY-MVP-1: world-space range ring + facing line
			// on each SELECTED mover. Gate MC2_TACTICAL_ARC_OVERLAY (default-OFF,
			// resolved once) -> no draw calls when unset (byte-identical). The
			// debug renderer's own MC2_DEBUG_RENDERER gate must also be ON for
			// these prims to actually flush. Read-only; no gameplay coupling.
			{
				static const bool s_tacArcOverlay = [](){
					const char* v = getenv("MC2_TACTICAL_ARC_OVERLAY");
					return v && v[0] && v[0] != '0';
				}();
				if (s_tacArcOverlay && ObjectManager) {
					const long nMovers = ObjectManager->getNumMovers();
					for (long mi = 0; mi < nMovers; ++mi) {
						MoverPtr mover = ObjectManager->getMover(mi);
						if (!mover || !mover->getSelected()) continue;
						Stuff::Vector3D p = mover->getPosition();
						// Stuff (x=east, y=north, z=elevation) -> debug-renderer
						// world (x=east, y=up, z=north); matches the
						// EditorInspector selection crosshair convention.
						DebugRenderer::Vec3 center{ p.x, p.z, p.y };
						const float maxR = mover->getMaxFireRange();
						// TACTICAL-OVERLAY-SELECTED-MECH-DATA-1: concentric weapon
						// range bands from the selected mover's cached fire ranges
						// (Mover::calcFireRanges()). All public getters, real data,
						// no gameplay coupling. Drawn outer->inner so the inner
						// rings read on top.
						if (maxR > 0.0f)
							DebugRenderer::drawRingWorld(center, maxR, 64, 0x4080FFFFu); // blue  = max range
						const float optR = mover->getOptimalFireRange();
						if (optR > 0.0f && optR < maxR)
							DebugRenderer::drawRingWorld(center, optR, 56, 0x40FFC0FFu); // cyan  = optimal range
						const float minR = mover->getMinFireRange();
						if (minR > 0.0f && minR < maxR)
							DebugRenderer::drawRingWorld(center, minR, 48, 0xFF8040FFu); // amber = min (dead) range
						// Facing heading tick (best-effort: MC2 rotation deg,
						// 0=north=+z). Capped length so it reads as a tick, not a
						// full-range spoke. Verify sign visually; ring is the
						// rotation-independent primary cue.
						const float th  = mover->getRotation() * 0.01745329252f; // deg->rad
						const float len = (maxR > 0.0f) ? fminf(maxR, 40.0f) : 20.0f;
						DebugRenderer::drawLineWorld(center,
							DebugRenderer::Vec3{ center.x + sinf(th) * len, center.y, center.z + cosf(th) * len },
							0x80FF80FFu); // green facing line
						// Firing-arc wedge: two spokes at facing +/- fireArc, out to
						// max range. Mover::getFireArc() returns the per-class HALF
						// angle in degrees (FireArc[]/2), applied relative to facing
						// exactly as Mover::getWeaponsLocked() tests it. Truthful cue
						// for where weapons can bear; no arc primitive in DebugRenderer
						// so the two edge spokes stand in for the wedge.
						const float arcDeg = mover->getFireArc();
						if (arcDeg > 0.0f && arcDeg < 180.0f && maxR > 0.0f) {
							const float arcRad = arcDeg * 0.01745329252f;
							const float thL = th - arcRad;
							const float thR = th + arcRad;
							DebugRenderer::drawLineWorld(center,
								DebugRenderer::Vec3{ center.x + sinf(thL) * maxR, center.y, center.z + cosf(thL) * maxR },
								0xFFFF40A0u); // yellow arc edge (left)
							DebugRenderer::drawLineWorld(center,
								DebugRenderer::Vec3{ center.x + sinf(thR) * maxR, center.y, center.z + cosf(thR) * maxR },
								0xFFFF40A0u); // yellow arc edge (right)
						}
					}
				}
			}
			DebugRenderer::flushWorldPrims();
		}
	}

	if (drawOldWay && !inMovieMode)
	{
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_Perspective, 1);

		if (compass && (turn > 3) && drawCompass)
			compass->render();
	}
	
	//---------------------------------------------------------	
	//Check if we are inMovieMode and should be letterboxed.
	// draw letterboxes here.
	if (inMovieMode && (letterBoxPos != 0.0f))
	{
		//Figure out the two faces we need to draw based on letterBox Pos and Alpha
		float barTopX = screenResolution.y * letterBoxPos;
		float barBotX = screenResolution.y - barTopX;

		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_Texture, 0);
		
 		//------------------------------------
		gos_VERTEX gVertex[4];

		gVertex[0].x		= 0.0f;
		gVertex[0].y		= 0.0f;
		gVertex[0].z		= 0.00001f;
		gVertex[0].rhw		= 0.00001f;
		gVertex[0].u		= 0.0f;
		gVertex[0].v		= 0.0f;
		gVertex[0].argb		= (letterBoxAlpha << 24);
		gVertex[0].frgb		= 0xff000000;

		gVertex[1].x		= 0.0f;
		gVertex[1].y		= barTopX;
		gVertex[1].z		= 0.00001f;               
		gVertex[1].rhw		= 0.00001f;               
		gVertex[1].u		= 0.0f;                  
		gVertex[1].v		= 0.0f;                  
		gVertex[1].argb		= (letterBoxAlpha << 24);
		gVertex[1].frgb		= 0xff000000;            

		gVertex[2].x		= screenResolution.x;
		gVertex[2].y		= barTopX; 
		gVertex[2].z		= 0.00001f;               
		gVertex[2].rhw		= 0.00001f;               
		gVertex[2].u		= 0.0f;                  
		gVertex[2].v		= 0.0f;                  
		gVertex[2].argb		= (letterBoxAlpha << 24);
		gVertex[2].frgb		= 0xff000000;            

		gVertex[3].x		= screenResolution.x;
		gVertex[3].y		= 0.0f;
		gVertex[3].z		= 0.00001f;               
		gVertex[3].rhw		= 0.00001f;               
		gVertex[3].u		= 0.0f;                  
		gVertex[3].v		= 0.0f;                  
		gVertex[3].argb		= (letterBoxAlpha << 24);
		gVertex[3].frgb		= 0xff000000;            
		
		gos_DrawQuads(gVertex, 4);
		
		gVertex[0].x		= 0.0f;
		gVertex[0].y		= barBotX;
		gVertex[0].z		= 0.00001f;
		gVertex[0].rhw		= 0.00001f;
		gVertex[0].u		= 0.0f;
		gVertex[0].v		= 0.0f;
		gVertex[0].argb		= (letterBoxAlpha << 24);
		gVertex[0].frgb		= 0xff000000;

		gVertex[1].x		= screenResolution.x;
		gVertex[1].y		= barBotX;
		gVertex[1].z		= 0.00001f;               
		gVertex[1].rhw		= 0.00001f;               
		gVertex[1].u		= 0.0f;                  
		gVertex[1].v		= 0.0f;                  
		gVertex[1].argb		= (letterBoxAlpha << 24);
		gVertex[1].frgb		= 0xff000000;            

		gVertex[2].x		= screenResolution.x;
		gVertex[2].y		= screenResolution.y; 
		gVertex[2].z		= 0.00001f;               
		gVertex[2].rhw		= 0.00001f;               
		gVertex[2].u		= 0.0f;                  
		gVertex[2].v		= 0.0f;                  
		gVertex[2].argb		= (letterBoxAlpha << 24);
		gVertex[2].frgb		= 0xff000000;            

		gVertex[3].x		= 0.0f; 
		gVertex[3].y		= screenResolution.y;
		gVertex[3].z		= 0.00001f;               
		gVertex[3].rhw		= 0.00001f;               
		gVertex[3].u		= 0.0f;                  
		gVertex[3].v		= 0.0f;                  
		gVertex[3].argb		= (letterBoxAlpha << 24);
		gVertex[3].frgb		= 0xff000000;            
		
		gos_DrawQuads(gVertex, 4);
	}

	if (inMovieMode && (fadeAlpha != 0x0))
	{
		//We are fading to something other then clear screen.
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_Texture, 0);
		
 		//------------------------------------
		gos_VERTEX gVertex[4];

		gVertex[0].x		= 0.0f;
		gVertex[0].y		= 0.0f;
		gVertex[0].z		= 0.00001f;
		gVertex[0].rhw		= 0.00001f;
		gVertex[0].u		= 0.0f;
		gVertex[0].v		= 0.0f;
		gVertex[0].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[0].frgb		= 0xff000000;

		gVertex[1].x		= 0.0f;
		gVertex[1].y		= screenResolution.y;
		gVertex[1].z		= 0.00001f;               
		gVertex[1].rhw		= 0.00001f;               
		gVertex[1].u		= 0.0f;                  
		gVertex[1].v		= 0.0f;                  
		gVertex[1].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[1].frgb		= 0xff000000;            

		gVertex[2].x		= screenResolution.x;
		gVertex[2].y		= screenResolution.y; 
		gVertex[2].z		= 0.00001f;               
		gVertex[2].rhw		= 0.00001f;               
		gVertex[2].u		= 0.0f;                  
		gVertex[2].v		= 0.0f;                  
		gVertex[2].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[2].frgb		= 0xff000000;            

		gVertex[3].x		= screenResolution.x;
		gVertex[3].y		= 0.0f;
		gVertex[3].z		= 0.00001f;               
		gVertex[3].rhw		= 0.00001f;               
		gVertex[3].u		= 0.0f;                  
		gVertex[3].v		= 0.0f;                  
		gVertex[3].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[3].frgb		= 0xff000000;            
		
		gos_DrawQuads(gVertex, 4);
	}
	
	//-----------------------------------------------------
}	

//---------------------------------------------------------------------------
long GameCamera::activate (void)
{
	//------------------------------------------
	// If camera is already active, just return
	if (ready && active)
		return(NO_ERR);
	
	//---------------------------------------------------------
	// Camera always starts pointing at first mover in lists
	// CANNOT be infinite because we don't allow missions without at least 1 player mech!!
	MoverPtr firstMover = NULL;
	if (ObjectManager->getNumMovers() > 0) {
		long i = 0;
		firstMover = ObjectManager->getMover(i);
		while (firstMover && ((firstMover->getCommander()->getId() != Commander::home->getId()) || !firstMover->isOnGUI()))
		{
			i++;
			if (i == ObjectManager->getNumMovers())
				break;
			firstMover = ObjectManager->getMover(i); 
		}
	}
	
	if (firstMover)
	{
		Stuff::Vector3D newPosition(firstMover->getPosition());
		setPosition(newPosition);
	}

	if (land)
	{
		land->update();
	}
		
	allNormal();
	
	//updateDaylight(true);
	
	lastShadowLightPitch = lightPitch;
	
	//Startup the SKYBox
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

	//--------------------------------------------------------------
	gosASSERT(genericAppearanceType->getAppearanceClass() == GENERIC_APPR_TYPE);
	theSky->init((GenericAppearanceType*)genericAppearanceType, NULL);
	
	theSky->setSkyNumber(mission->theSkyNumber);

	// HDRI-SKY-NUMBER-1: swap the HDRI equirect sky to the mood-appropriate
	// asset for this mission's sky number (IblHdriRegistry mapping).
	GameAdapters::Sky::setSkyNumber(static_cast<int>(mission->theSkyNumber));

	// DEBUG-STATE-ASSETS: record sky number for per-mission asset block in JSON dump.
	g_dbgSkyNumber = mission->theSkyNumber;

 	return NO_ERR;
}

inline GameObjectPtr getCamObject (long partId, bool existsOnly) 
{
	GameObjectPtr obj = NULL;
	if (partId == -1)
		obj = NULL;
	else
		obj = ObjectManager->findByPartId(partId);

	if (existsOnly) 
	{
        STOP((""));
        //sebi: will crash if obj==NULL
		if (obj && 
			obj->getExists() && 
			(obj->getCommanderId() == Commander::home->getId()) || 
			(Team::home->teamLineOfSight(obj->getLOSPosition(),0.0f)))
			return(obj);
		return(NULL);
	}

	return(obj);
}

long cameraLineChanged = 0;
bool useLOSAngle = true;
//---------------------------------------------------------------------------
long GameCamera::update (void)
{
	if (lookTargetObject != -1)
		targetObject = getCamObject(lookTargetObject,true);
		
	if (targetObject && 
		targetObject->getExists() && 
		((targetObject->getCommanderId() == Commander::home->getId()) || 
		!targetObject->isMover() ||
		(targetObject->isMover() && ((Mover *)targetObject)->conStat >= CONTACT_SENSOR_QUALITY_1) ))
	{
		setPosition(targetObject->getPosition(),false);
	}
	else
	{
		targetObject = NULL;
	}

	//Force CameraAltitude to be less than max based on angle.  This keeps poly load relatively even	
	float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
	float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);
	
	if (cameraAltitude > testMax)
		cameraAltitude = testMax;

	if ((cameraAltitude < testMax) && (cameraAltitudeDesired > testMax))
		cameraAltitude = testMax;
												  
	// HZB validation knob (debug only): force a low across-map HORIZON view so
	// static props occlude each other. MC2's default RTS camera is near top-down
	// (projectionAngle up to 88deg) where buildings rarely occlude — that hides
	// any HZB-cull benefit. Shallow angle (~12deg) + low altitude looks across
	// the map. Tunable: MC2_HZB_FORCE_HORIZON_ANGLE (deg), MC2_HZB_FORCE_HORIZON_ALT.
	// MC2_HZB_VIEW_FILE: path to a saved_view.txt written by Ctrl+Alt+V.
	// When set, read the file once and pin the camera to those exact coords.
	static const char* s_hzbViewFile = getenv("MC2_HZB_VIEW_FILE");
	struct HzbSavedView { float x, y, z, rot, angle, alt; bool valid; };
	static HzbSavedView s_hzbSavedView = [&]() -> HzbSavedView {
		HzbSavedView v{}; v.valid = false;
		if (!s_hzbViewFile || !s_hzbViewFile[0]) return v;
		FILE* f = fopen(s_hzbViewFile, "r");
		if (!f) { fprintf(stderr, "[HZB_VIEW_FILE] cannot open '%s'\n", s_hzbViewFile); fflush(stderr); return v; }
		int n = fscanf(f, "X=%f Y=%f Z=%f ROT=%f ANGLE=%f ALT=%f",
		               &v.x, &v.y, &v.z, &v.rot, &v.angle, &v.alt);
		fclose(f);
		if (n == 6) {
			v.valid = true;
			fprintf(stderr, "[HZB_VIEW_FILE] loaded X=%.4f Y=%.4f Z=%.4f ROT=%.4f ANGLE=%.4f ALT=%.4f\n",
			        v.x, v.y, v.z, v.rot, v.angle, v.alt);
			fflush(stderr);
		} else {
			fprintf(stderr, "[HZB_VIEW_FILE] parse failed (n=%d) in '%s'\n", n, s_hzbViewFile);
			fflush(stderr);
		}
		return v;
	}();

	// Shared pin lambda: zero all jitter sources so viewProj is byte-stable.
	auto hzbPinCamera = [&]() {
		velocity.Zero();
		goalVelocity.Zero();
		goalVelTime  = 0.0f;
		goalPosition = position;
		goalPosTime  = 0.0f;
		goalRotation.x = projectionAngle;
		goalRotation.y = cameraRotation;
		goalRotation.z = cameraAltitude;
		goalRotTime  = 0.0f;
		if (land) {
			float elev = land->getTerrainElevation(position);
			if (elev < Terrain::waterElevation) elev = Terrain::waterElevation;
			position.z = elev;
			goalPositionZ = elev;
			cameraShiftZ  = elev;
		}
		cameraShift.x = 0.0f;
		cameraShift.y = 0.0f;
	};

	static const bool s_hzbForceHorizon = (getenv("MC2_HZB_FORCE_HORIZON") != nullptr);
	if (s_hzbSavedView.valid)
	{
		// MC2_HZB_VIEW_FILE wins: absolute coord replay from saved_view.txt
		position.x     = s_hzbSavedView.x;
		position.y     = s_hzbSavedView.y;
		// position.z set by pin lambda via terrain elevation
		cameraRotation        = s_hzbSavedView.rot;
		projectionAngle       = s_hzbSavedView.angle;
		cameraAltitude        = s_hzbSavedView.alt;
		cameraAltitudeDesired = s_hzbSavedView.alt;
		anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);
		hzbPinCamera();
	}
	else if (s_hzbForceHorizon)
	{
		auto envf = [](const char* k, float def) {
			const char* v = getenv(k);
			return (v && v[0]) ? (float)atof(v) : def;
		};
		// Corner position as a fraction of the half-map extent (map is centered
		// at origin: vegHalfMap = worldUnitsMapSide*0.5). +X=east, +Y=north.
		// mc2_01 SE-looking-NW: X=+0.9 Y=-0.9 ROT~?  mc2_24 NW-looking-SE: X=-0.9 Y=+0.9.
		static const float s_xf    = envf("MC2_HZB_FORCE_HORIZON_X",   0.9f);
		static const float s_yf    = envf("MC2_HZB_FORCE_HORIZON_Y",  -0.9f);
		static const float s_rot   = envf("MC2_HZB_FORCE_HORIZON_ROT", 135.0f);
		static const float s_angle = envf("MC2_HZB_FORCE_HORIZON_ANGLE", 12.0f);
		static const float s_alt   = envf("MC2_HZB_FORCE_HORIZON_ALT",  150.0f);
		// Position override is opt-in (MC2_HZB_FORCE_HORIZON_POS=1): place the
		// camera at a map corner. Without it, the flythrough position is kept and
		// only the pitch/altitude/rotation are forced (occlusion-rich over land).
		static const bool s_forcePos = (getenv("MC2_HZB_FORCE_HORIZON_POS") != nullptr);
		if (s_forcePos) {
			const float half = Terrain::worldUnitsMapSide * 0.5f;
			position.x = s_xf * half;
			position.y = s_yf * half;
		}
		cameraRotation        = s_rot;
		projectionAngle       = s_angle;
		cameraAltitude        = s_alt;
		cameraAltitudeDesired = s_alt;
		anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);

		// HZB-PIN: kill all jitter sources so viewProj is byte-stable frame-to-frame.
		hzbPinCamera();
	}

	// calculate new near and far plane distance based on
	// Current altitude above terrain.
	float altitudePercent = (cameraAltitude - AltitudeMinimum) / (testMax - AltitudeMinimum);
	Camera::NearPlaneDistance = MinNearPlane + ((MaxNearPlane - MinNearPlane) * altitudePercent);
	Camera::FarPlaneDistance = MinFarPlane + ((MaxFarPlane - MinFarPlane) * altitudePercent);
	
	if (userInput->getKeyDown(KEY_LBRACKET) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		useLOSAngle ^= true;
	}

	// Ctrl+Alt+V: dump current camera viewpoint to saved_view.txt for deterministic replay.
	// Use MC2_HZB_VIEW_FILE=A:/Games/mc2-hzb-cull/saved_view.txt to replay it.
	if (userInput->getKeyDown(KEY_V) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		const char* dumpPath = "A:/Games/mc2-hzb-cull/saved_view.txt";
		FILE* df = fopen(dumpPath, "w");
		if (df)
		{
			fprintf(df, "X=%.4f Y=%.4f Z=%.4f ROT=%.4f ANGLE=%.4f ALT=%.4f\n",
			        position.x, position.y, position.z,
			        cameraRotation, projectionAngle, cameraAltitude);
			fclose(df);
			fprintf(stderr, "[HZB_VIEW_DUMP v1] saved X=%.4f Y=%.4f Z=%.4f ROT=%.4f ANGLE=%.4f ALT=%.4f\n",
			        position.x, position.y, position.z,
			        cameraRotation, projectionAngle, cameraAltitude);
			fflush(stderr);
		}
		else
		{
			fprintf(stderr, "[HZB_VIEW_DUMP v1] ERROR: cannot open '%s' for write\n", dumpPath);
			fflush(stderr);
		}
	}

#ifdef DEBUG_CAMERA
	if (userInput->getKeyDown(KEY_RBRACKET) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		Camera::NearPlaneDistance += 10.0f;
	}		

	if (userInput->getKeyDown(KEY_APOSTROPHE) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		Camera::FarPlaneDistance -= 1005.00f;
	}		

	if (userInput->getKeyDown(KEY_SEMICOLON) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		Camera::FarPlaneDistance += 1005.0f;
	}		

	char text[1024];
	sprintf(text,"Near Plane: %f     Far Plane: %f",Camera::NearPlaneDistance,Camera::FarPlaneDistance);

	DWORD width, height;
	Stuff::Vector4D moveHere;
	moveHere.x = 10.0f;
	moveHere.y = 10.0f;

	gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, true, false, false);
	gos_TextStringLength(&width,&height,text);

	moveHere.z = width;
	moveHere.w = height;

	globalFloatHelp[currentFloatHelp].setHelpText(text);
	globalFloatHelp[currentFloatHelp].setScreenPos(moveHere);
	globalFloatHelp[currentFloatHelp].setForegroundColor(SD_GREEN);
	globalFloatHelp[currentFloatHelp].setBackgroundColor(SD_BLACK);
	globalFloatHelp[currentFloatHelp].setScale(1.0f);
	globalFloatHelp[currentFloatHelp].setProportional(true);
	globalFloatHelp[currentFloatHelp].setBold(false);
	globalFloatHelp[currentFloatHelp].setItalic(false);
	globalFloatHelp[currentFloatHelp].setWordWrap(false);

	currentFloatHelp++;

	gosASSERT(currentFloatHelp < MAX_FLOAT_HELPS);
#endif

	if (DisplayCameraAngle)
	{
		char text[1024];
		sprintf(text,"Camera Angle: %f degrees    Camera Altitude: %f    CameraPosition: X=%f Y=%f Z=%f   CameraRotation: %f",projectionAngle,cameraAltitude,position.x,position.y,position.z,cameraRotation);
		
		DWORD width, height;
		Stuff::Vector4D moveHere;
		moveHere.x = 10.0f;
		moveHere.y = 10.0f;

		gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, true, false, false);
		gos_TextStringLength(&width,&height,text);

		moveHere.z = width;
		moveHere.w = height;

		globalFloatHelp->setFloatHelp(text,moveHere,SD_GREEN,SD_BLACK,1.0f,true,false,false,false);
	}

	// Tactical Overview diagnostic HUD — only under MC2_TACTICAL_OVERVIEW_DEBUG.
	// Shows blend t / setpoint / altitude for tuning; off in normal play.
	if (TacticalOverview::enabled() && getenv("MC2_TACTICAL_OVERVIEW_DEBUG"))
	{
		char ovtext[256];
		sprintf(ovtext,"[TacOverview] t=%.3f setpt=%.2f fires=%d | tilt=%.1f alt=%.0f maxLo=%.0f  (F6=toggle, wheel=zoom)",
			g_tacticalOverview.blend(), g_tacticalOverview.setpoint(),
			g_tacticalOverview.hotkeyFires(), projectionAngle, cameraAltitude,
			Camera::AltitudeMaximumLo);
		Stuff::Vector4D ovPos; ovPos.x = 10.0f; ovPos.y = 40.0f;
		DWORD ow, oh;
		gos_TextSetAttributes(gosFontHandle, 0, gosFontScale, false, true, false, false);
		gos_TextStringLength(&ow,&oh,ovtext);
		ovPos.z = ow; ovPos.w = oh;
		globalFloatHelp->setFloatHelp(ovtext,ovPos,SD_YELLOW,SD_BLACK,1.0f,true,false,false,false);
	}

	if (!compass)	//Create it!
	{
		AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "compass" );
		compass = new BldgAppearance;
		compass->init( appearanceType );
	}

	long result = Camera::update();
	
//	if ((day2NightTransitionTime > 0.0f) && !getIsNight() && (fabs(lastShadowLightPitch-lightPitch) > MAX_SHADOW_PITCH_CHANGE))
//	{
//		forceShadowRecalc = true;
//		lastShadowLightPitch = lightPitch;
//	}
//	else
//	{
//		forceShadowRecalc = false;
//	}
	
	//Always TRUE for right now.  Debugging....
	//-fs
	//forceShadowRecalc = true;
	
	bool oldFog = useFog;
	bool oldShadows = useShadows;
	useFog = false;
	useShadows = false;
		
  	if (compass && (turn > 3))
	{
  		
   		compass->setObjectParameters(getPosition(),0.0f,false,0,0);
   		compass->setMoverParameters(0.0f);
   		compass->setGesture(0);
   		compass->setObjStatus(OBJECT_STATUS_DESTROYED);
   		compass->setInView(true);
   		compass->setVisibility(true,true);
   		compass->setFilterState(true);
		compass->setIsHudElement();
   		compass->update();		   //Force it to try and draw or stuff will not work!
	}

	if (theSky)
	{
		Stuff::Vector3D pos = getPosition();
		
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
	
	return result;
}

//---------------------------------------------------------------------------
// mc2_hzb_dump_camera_view — C-linkage helper called from GuiRuntime/EditorInspector.cpp
// ("Save Camera View (HZB)" button). Writes saved_view.txt in the same format that
// the MC2_HZB_VIEW_FILE replay reader (fscanf "X=%f Y=%f Z=%f ROT=%f ANGLE=%f ALT=%f")
// expects. Keeping this here avoids pulling camera.h -> terrain.h into GuiRuntime.
extern "C" void mc2_hzb_dump_camera_view(void)
{
	const char* dumpPath = "A:/Games/mc2-hzb-cull/saved_view.txt";
	if (!eye) {
		fprintf(stderr, "[HZB_VIEW_DUMP v1] ERROR: eye is null\n");
		fflush(stderr);
		return;
	}
	Stuff::Vector3D pos = eye->getPosition();
	float rot   = eye->getCameraRotation();
	float angle = eye->getProjectionAngle();
	float alt   = eye->getCameraAltitude();
	FILE* df = fopen(dumpPath, "w");
	if (df) {
		fprintf(df, "X=%.4f Y=%.4f Z=%.4f ROT=%.4f ANGLE=%.4f ALT=%.4f\n",
		        pos.x, pos.y, pos.z, rot, angle, alt);
		fclose(df);
		fprintf(stderr, "[HZB_VIEW_DUMP v1] saved X=%.4f Y=%.4f Z=%.4f ROT=%.4f ANGLE=%.4f ALT=%.4f\n",
		        pos.x, pos.y, pos.z, rot, angle, alt);
		fflush(stderr);
	} else {
		fprintf(stderr, "[HZB_VIEW_DUMP v1] ERROR: cannot open '%s' for write\n", dumpPath);
		fflush(stderr);
	}
}

//---------------------------------------------------------------------------
//
// Edit log
//
//---------------------------------------------------------------------------
