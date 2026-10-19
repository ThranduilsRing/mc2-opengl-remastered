//---------------------------------------------------------------------------
//
// Camera.h -- File contains the camera class definitions
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef CAMERA_H
#define CAMERA_H
//---------------------------------------------------------------------------
// Include Files

#ifndef DCAMERA_H
#include"dcamera.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef MATHFUNC_H
#include"mathfunc.h"
#endif

// F3 CPU projection cost-baseline: forward declarations to keep camera.h
// header weight low. projectZ() bumps n_calls_eventdriven when called
// outside the render loop (TLS flag tracked by RenderLoopGuard).
#include <cstdint>  // int64_t for cull_admission_*_ns
#include "Gates/gate.h"
namespace mc2_cpu_proj_cost {
    extern bool g_cpuProjEnabled;
    extern thread_local bool tls_inRenderLoop;
    void add_workload_eventdriven_projectZ();
    // R2 cull-admission timing pair (see cpu_proj_cost_split.h comment).
    int64_t cull_admission_begin_ns();
    void    cull_admission_end_ns(int64_t startNs);
    // R3 narrow-subset wrapper timing pairs (one per remaining
    // policy-split wrapper). Same contract as cull_admission_*_ns:
    // env-OFF returns 0 / no-op.
    int64_t screenxy_begin_ns();
    void    screenxy_end_ns(int64_t startNs);
    int64_t effect_admission_begin_ns();
    void    effect_admission_end_ns(int64_t startNs);
    int64_t terrain_admission_begin_ns();
    void    terrain_admission_end_ns(int64_t startNs);
    int64_t lighting_shadow_begin_ns();
    void    lighting_shadow_end_ns(int64_t startNs);
    int64_t selection_picking_begin_ns();
    void    selection_picking_end_ns(int64_t startNs);
    int64_t debug_overlay_begin_ns();
    void    debug_overlay_end_ns(int64_t startNs);
}

#ifndef TGL_H
#include"tgl.h"
#endif

#ifndef INIFILE_H
#include"inifile.h"
#endif

#include<stuff/stuff.hpp>
#include "math/mc2_math.h"
#include<float.h>   // FLT_MAX for trueSignedRhw sentinel
#include<cmath>     // isfinite for MC2_PROJECTZ_FINITE_CHECK invariant
#include<cstdlib>

inline signed short int float2short(float _in)
{
	#if 1
	return short(floor(_in));
	#else
	_in-=0.5f;
	_in+=12582912.0f;
	return(*(signed short int*)&_in);
	#endif
}

extern float zero;
extern float one;
extern float point1;

//---------------------------------------------------------------------------
// Also defined in Stuff but not getting into here somehow.
enum Axes {
	X_Axis,
	Y_Axis,
	Z_Axis,
	W_Axis
};

#define MAX_LODS				3

inline constexpr int MAX_VIEWS = 4;
inline constexpr int F2_VIEW = 0;
inline constexpr int F3_VIEW = 1;
inline constexpr int F4_VIEW = 2;
inline constexpr int F5_VIEW = 3;

//---------------------------------------------------------------------------
// LegacyProjectionResult
//
// Optional sidecar populated by Camera::projectZ() when a non-null
// pointer is passed. Exposes the inputs the legacy admission test
// destroys (rawClip, signedW) so future diagnostic and replacement-
// candidate predicates can be evaluated alongside the legacy
// screen-rect bool without changing what projectZ() returns or writes
// to its `screen` out-parameter.
//
// Spec: docs/superpowers/specs/2026-04-25-projectz-containment-design.md
// Field semantics match the current projectZ() body byte-for-byte:
//   legacyRhw = 1.0f when signedW == 0, else 1.0f / signedW
// trueSignedRhw is diagnostic-only (commit 3): 1.0f/signedW without the
// zero-guard, so it is ±FLT_MAX (approx ±Inf) when signedW == 0. It
// never matches legacyRhw when signedW is zero; useful for detecting
// the behind-camera fabs(rhw) hazard in the capture report.
//---------------------------------------------------------------------------
struct LegacyProjectionResult {
	bool             acceptedByLegacyScreenRect;
	Stuff::Vector4D  screen;
	Stuff::Vector4D  rawClip;
	float            signedW;
	float            legacyRhw;
	bool             usePerspective;
	float            trueSignedRhw;  // diagnostic-only (commit 3); ±FLT_MAX when signedW==0
};

// Diagnostic trace system include. Declares g_pzTrace, g_projectz_site_id,
// g_projectz_site_cat, projectz_trace_dispatch(), and the PROJECTZ_SITE macro.
// Must appear after LegacyProjectionResult (trace.h forward-declares it).
#include "projectz_trace.h"
#include "object_admission_predicate.h"
#include "../RenderCore/axis_convert.h"

// gos_GetViewport is declared in gameos.hpp which is transitively included via tgl.h.
// No forward declaration needed here.

//---------------------------------------------------------------------------
class Camera
{
    //sebi
    void updateLights();

	//Data Members
	//-------------

	protected:
		float						projectionAngle;				//Angle of orthogonal projection

		glm::vec3				screenResolution;				//Resolution of screen in pixels
		glm::vec3				screenCenter;					//Center coordinate of screen
		glm::vec3				lookVector;						//Direction camera is looking.
		glm::vec3				physicalPos;					//Actual Physical Position of camera in world.
		
		CameraClass					cameraClass;					//Type of camera.w

		// data for Camera Movement
		glm::vec3				position;						//Position of camera in 3Space.
		float						cameraRotation;					//Current Direction camera is looking.
		float						worldCameraRotation;			//Current Direction camera is facing in World Frame of reference.

		float						zoomLevelLODScale[MAX_LODS];	//Data to help calc which LOD at which zoom.
		
		glm::mat4		cameraOrigin;					//Translation and rotation of Camera
		glm::mat4		worldToCameraMatrix;			//Inverse of the above.

		mc2::math::YawPitchRange	cameraDirection;				//Direction camera is looking.
		
		Stuff::Vector2DOf<float>	cameraShift;					//Position camera is looking At.
		
		glm::mat4				cameraToClip;					//Camera Clip Matrix--Used for projection and zoom.
		// F2 unified-projection: parallel GL-native projection product.
		// = cameraToClip * kPixelHomogToGLNDC (precomputed at camera-update
		// time). Consumed by Camera::worldToClipGL() for the GPU path.
		// The legacy `cameraToClip` stays in D3D-pixel-homogeneous form for
		// CPU projectZ + 8 wrappers + MLR. Mclib/camera.cpp keeps both in
		// sync via cameraToClipGL.Multiply(cameraToClip, kPixelHomogToGLNDC)
		// called after every cameraToClip write in calculateProjectionConstants.
		glm::mat4				cameraToClipGL;
		glm::mat4				worldToClip;					//Matrix used to bring a point from world space to camera/clip space
		glm::mat4				clipToWorld;					//Matrix used to bring a point from camera/clip space to world space
		float						cachedFrustumPlanes_[6][4];		// F6 T2 cache; valid after cacheFrustumPlanes() per frame.

		// Pick screen-rect cache: monotonically increments when camera inputs change
		// visually. Keyed on a coarse integer hash of position/rotation/fov/viewport
		// rather than worldToClip bits — stable under per-frame altitude/zoom lerp
		// micro-drift that would otherwise fire every frame.
		// Never zero: 0 is the uninitialized sentinel in PickScreenRectCache.
		uint32_t					viewProjectionRevision_;
		uint32_t					lastCameraInputHash_;			// previous coarse hash for change detection
		
		TG_LightPtr					*worldLights;					//Lighting for the entire world.
		long						numLights;						//Number of lights in the above list.  Always MAX_LIGHTS!
		
		TG_LightPtr					*activeLights;					//This is the light list to process every frame.
		long						numActiveLights;				//Number of lights active.  Actually Correct.
		
		TG_LightPtr					*terrainLights;					//This is the light list to process every frame for TERRAIN ONLY
		long						numTerrainLights;				//Number of lights active for terrain.  Actually Correct.

		gosBuffer*					lights_shader_data_;
		
 		//Camera Scripting stuff
		glm::vec3				goalPosition;
		glm::vec3				lookPosition;
		float						goalPosTime;
		
		glm::vec3				goalRotation;
		float						goalRotTime;
		
		float						goalFOV;
		float						goalFOVTime;
		
		glm::vec3				velocity;
		glm::vec3				goalVelocity;
		float						goalVelTime;
		
		long						lookTargetObject;
		
		float						letterBoxPos;
		float						letterBoxTime;
		BYTE						letterBoxAlpha;
		
		bool						startEnding;
		
		bool						inFadeMode;
		DWORD						fadeColor;
		DWORD						fadeAlpha;
		DWORD						fadeStart;
		float						timeLeftToFade;
		float						startFadeTime;
		
		
	public:

		bool					active;					//Is camera active (ie. drawing itself)
		bool					ready;					//Is camera ready to be activated?
		bool					usePerspective;			//Switch camera from Parallel to perspective view
												
		glm::vec3			lightDirection;			//Direction of Spot Light

		float					lightYaw;				//Direction of Light in Azimuth, elevation
		float					lightPitch;

		float					lightFinalYaw;
		float					lightFinalPitch;
		float					lightTimeToFinal;

		float					newScaleFactor;			//Smooth Zooming
		float					camera_fov;				//Smooth Perspective zooming.
		float					cosHalfFOV;				//Cosine of half the FOV for view cone.
		
		float					viewMulX, viewMulY,		//GOS Viewport data variables
			 					viewAddX, viewAddY;

		unsigned char			lightRed;				//Red component of World Light
		unsigned char			lightGreen;				//Green component of World Light
		unsigned char			lightBlue;				//Blue component of World Light

		unsigned char			ambientRed;				//Red component of World Light
		unsigned char			ambientGreen;			//Green component of World Light
		unsigned char			ambientBlue; 			//Blue component of World Light

		bool			terrainShadowColorEnabled;
		unsigned char			terrainShadowRed;			//Red component of World Light
		unsigned char			terrainShadowGreen;		//Green component of World Light
		unsigned char			terrainShadowBlue; 		//Blue component of World Light
		
		//Day/Night transition data.
		float					dayLightPitch;
		
		unsigned char			dayLightRed;			//Red component of World Light
		unsigned char			dayLightGreen;			//Green component of World Light
		unsigned char			dayLightBlue;			//Blue component of World Light

		unsigned char			dayAmbientRed;			//Red component of World Light
		unsigned char			dayAmbientGreen;		//Green component of World Light
		unsigned char			dayAmbientBlue; 		//Blue component of World Light
		
		unsigned char			sunsetLightRed;			//Red component of World Light
		unsigned char			sunsetLightGreen;			//Green component of World Light
		unsigned char			sunsetLightBlue;			//Blue component of World Light
 		
		unsigned char			nightLightRed;			//Red component of World Light
		unsigned char			nightLightGreen;		//Green component of World Light
		unsigned char			nightLightBlue;			//Blue component of World Light

		unsigned char			nightAmbientRed;		//Red component of World Light
		unsigned char			nightAmbientGreen;		//Green component of World Light
		unsigned char			nightAmbientBlue; 		//Blue component of World Light
		
		bool					terrainLightNight;		//What state are the terrain lights in?
		bool					terrainLightCalc;		//Should we activate all terrain lights this frame and burn their lights in?
		
		bool					isNight;				//Flag indicating if we are in NightMode
														//Based solely on light angle.
																	
		float					nightFactor;			//How "night" is it?  1.0f being full night, 0.0f being full day!


		float					day2NightTransitionTime;//Time in Seconds that light goes from day to night.
		float					dayLightTime;			//Current dayToNight Transition time.
		bool					forceShadowRecalc;		//Has the sun/moon moved enough for shadows to have changed?
		
 		unsigned char			seenRed;				//Red component of World Light
		unsigned char			seenGreen;				//Green component of World Light
		unsigned char			seenBlue;	 			//Blue component of World Light

		unsigned char			baseRed;				//Red component of World Light
		unsigned char			baseGreen;				//Green component of World Light
		unsigned char			baseBlue; 				//Blue component of World Light

		float 					cameraShiftZ;
		float 					goalPositionZ;

		float					fogStart;				//Altitude at which fog starts
		float					fogFull;				//Altitude at which fog is Full;
		DWORD					dayFogColor;				//Color of FOG.
		float					fogTransparency;				//How much of the sky color will show through
		DWORD					fogColor;				//Color of FOG.
		float            		cameraAltitude;
		float					cameraAltitudeDesired;	//What would I like the camera to be at!  Maybe smaller due to low angle!

		// [MOUSE-ANCHORED-ZOOM-1 v2] eased zoom-anchor state (see beginZoomAnchor / update()).
		bool					zoomAnchorActive = false;
		float					zoomAnchorH0 = 1.0f;
		glm::vec3			zoomAnchorWorld;
		glm::vec3			zoomAnchorPivot;

		// [CAMERA-SMOOTHING-1] gameplay camera smoothing (MC2_CAMERA_SMOOTH ms,
		// default ~100, 0 = legacy verbatim). Edge/keyboard scroll and rotation
		// input accumulate a GOAL pose (clamped like setPosition); update() moves
		// the actual pose toward it with a frame-rate-independent exponential
		// step. Hard-set paths (bookmark apply, scripted swoop, capture
		// fixtures) never arm this state, and any externally-caused pose change
		// drops the goal, so captures stay deterministic.
		bool					scrollSmoothActive = false;
		float					scrollSmoothGoalX = 0.0f;
		float					scrollSmoothGoalY = 0.0f;
		float					scrollSmoothLastX = 0.0f;
		float					scrollSmoothLastY = 0.0f;
		bool					rotSmoothActive = false;
		float					rotSmoothGoalCam = 0.0f;
		float					rotSmoothGoalWorld = 0.0f;
		float					rotSmoothLastCam = 0.0f;

		static float			globalScaleFactor;		//Global Rescale Factor.

		static float			MaxClipDistance;
		static float    		MinHazeDistance;
		static float			DistanceFactor;

		static float			MinNearPlane;
		static float			MaxNearPlane;

		static float			MinFarPlane;
		static float			MaxFarPlane;
				
		static float			HazeFactor;
		
		static float			verticalSphereClipConstant;
		static float 			horizontalSphereClipConstant;
		static float 			NearPlaneDistance;
		static float			FarPlaneDistance;

		static float            AltitudeMinimum;
		static float            AltitudeMaximumLo;
		static float			AltitudeMaximumHi;
		static float            AltitudeTight;
		static float            AltitudeDefault;
		static float			MaxLetterBoxTime;

		static float			MIN_PERSPECTIVE;
		static float			MAX_PERSPECTIVE;
		static float			MIN_ORTHO;
		static float			MAX_ORTHO;
		
		static bool				inMovieMode;
		static bool 			forceMovieEnd;
		static float			MaxLetterBoxPos;
		
		static float			cameraTilt[MAX_VIEWS];
		static float			cameraZoom[MAX_VIEWS];
		
		static frameOfRef		cameraFrame;

	//Member Functions
	//-----------------

	public:
	
		virtual void init (void)
		{
			projectionAngle = 30;

			screenResolution.x = 400.0;
			screenResolution.y = 400.0;

			position = glm::vec3(0.0f);

			// The matrix members have empty default constructors, so until the first calculateProjectionConstants() they held whatever
			// the allocation held. Terrain LOD runs before the camera's first update and read them through cacheFrustumPlanes(): NaN
			// planes in one run, zeros in the next (the frame-2 [TerrainLOD v1] DIAG plane lines). Start from identity and zero planes.
			cameraOrigin = glm::mat4(1.0f);
			worldToCameraMatrix = glm::mat4(1.0f);
			cameraToClip = glm::mat4(1.0f);
			cameraToClipGL = glm::mat4(1.0f);
			worldToClip = glm::mat4(1.0f);
			clipToWorld = glm::mat4(1.0f);
			for (auto& plane : cachedFrustumPlanes_)
				plane[0] = plane[1] = plane[2] = plane[3] = 0.0f;

			ready = active = FALSE;

			cameraClass = INVALID_CAMERA;

			setCameraRotation(0.0,0.0);

			cameraShiftZ = goalPositionZ = 0.0f;
	
			lightDirection = glm::vec3(0.0f);
			
			newScaleFactor = 0.5;

			cameraShift.x = cameraShift.y = 0.0;
			
			worldLights = nullptr;
			activeLights = nullptr;
			terrainLights = nullptr;

			// Class invariant: the light COUNTS must be 0 whenever the light
			// arrays are NULL. The ctor zeroed the array pointers but left the
			// counts uninitialized — they are only set to 0 inside
			// Camera::init(FitIniFilePtr) (camera.cpp:453), which allocates the
			// arrays. The editor generate-map path (EditorData::initTerrainFromTGA)
			// never calls eye->init() (only the load path initTerrainFromPCV does),
			// so a freshly generated map reached gos_terrain_lighting::PackAndDispatch
			// with terrainLights==NULL and numTerrainLights==garbage(>2) ->
			// getTerrainLight read terrainLights[2] = NULL+0x10 (0xC0000005). Zero the
			// counts here so the invariant holds from construction in every init path.
			numLights = 0;
			numActiveLights = 0;
			numTerrainLights = 0;

			fogStart = fogFull = 0.0;
			dayFogColor = 0xffffffff;
			fogTransparency = 1.0;
			fogColor = 0xffffffff;

			usePerspective = false;

			viewMulX = viewMulY = viewAddX = viewAddY = 0.0f;
			
			isNight = false;
			forceShadowRecalc = false;
			
			goalPosition = glm::vec3(0.0f);
			lookPosition = glm::vec3(0.0f);
			goalPosTime = 0.0;
			
			goalRotation.x = goalRotation.y = goalRotation.z = 0.0;
			goalRotTime = 0.0;
			
			goalFOV = 0.0;
			goalFOVTime = 0.0;
			
			velocity = glm::vec3(0.0f);
			goalVelocity = glm::vec3(0.0f);
			goalVelTime = 0.0;
			
			lookTargetObject = -1;
			
			terrainLightNight = false;
			terrainLightCalc = true;
			
			letterBoxPos = 0.0f;
			letterBoxTime = 0.0f;
			letterBoxAlpha = 0x0;
			
			startEnding = false;
			
			inFadeMode = false;
			fadeColor = 0x0;
			fadeAlpha = 0x0;
			timeLeftToFade = 0.0f;
			startFadeTime = 0.0f;

			cameraAltitude = 1200.0f;
			cameraAltitudeDesired = 1200.0f;

			nightFactor = 0.0f;

			// TERRAIN-AMBIENT-FLOOR-2: dayLightPitch was the ONLY member of the
			// day/night set left indeterminate here (isNight is zeroed at :440,
			// nightFactor directly above). It is assigned only by
			// init(FitIniFile*) at camera.cpp:371/375, so a default-constructed
			// Camera read back garbage. Nothing used to read it before that load
			// (this line used to add "-- updateDaylight() is the sole consumer
			// and always runs after --"; corrected 2026-07-30
			// DAYLIGHT-GATE-RECHECK-1: updateDaylight() never runs in a mission
			// at all, and it never reads dayLightPitch either),
			// but getDayLightPitch() now feeds the terrain ambient floor's
			// daylight gate, and an indeterminate pitch could resolve to a
			// fully-ON brightening gate. 0 == sun exactly at the horizon ==
			// gate 0, so an unloaded camera fails DARK.
			dayLightPitch = 0.0f;
		}

		Camera (void)
		{
			init();
		}

		long init (FitIniFile *cameraFile);

		// Virtual so a teardown through a Camera pointer reaches GameCamera::destroy, which frees the sky and compass appearances
		// (they used to be swept up with the appearance heap). Idempotent: every free nulls its pointer, so ~Camera after an explicit destroy is safe.
		virtual void destroy (void);

		~Camera (void)
		{
			destroy();
		}

		float getProjectionAngle (void)
		{
			return(projectionAngle);
		}

		float getCameraAltitude (void)
		{
			return(cameraAltitude);
		}

		// [MOUSE-ANCHORED-ZOOM-1 / FIX-4] read post-step desired altitude (h1).
		float getCameraAltitudeDesired (void)
		{
			return(cameraAltitudeDesired);
		}

		// GAMEPLAY-DRIVER-1: drive the perspective-zoom goal-seek directly (the
		// `camera` command-channel verb's optional zoom arg). update() lerps
		// cameraAltitude toward this each frame, so the same smoothing the wheel
		// zoom uses applies. Only meaningful in perspective mode.
		void setCameraAltitudeDesired (float alt)
		{
			cameraAltitudeDesired = alt;
		}

		// GAMEPLAY-DRIVER-2 (MC2_UI_COMMAND_FILE `time_of_day` / `sun_pitch`):
		// move the sun at runtime so a capture agent can render the SAME pose
		// under day and night light without hunting for a mission that ships
		// as night.
		//
		// setDaylightFraction(f) does the FULL recompute: it seeds dayLightTime
		// from f (0..1 == one whole 0..360 degree day/night sweep) and calls
		// updateDaylight(), which rewrites lightPitch AND the ambient / sun /
		// sky / fog colors. Missions with DayToNightTime <= 0 (camera.cpp:341
		// parks it at -1) skip updateDaylight's whole body, so this seeds a
		// 3600s transition first -- nothing else in the GAME reads
		// day2NightTransitionTime (the two other readers are an editor save and
		// a commented-out gamecam block), so that seed is inert by itself.
		//
		// setSunPitch(deg) is the cheap half: it only writes lightPitch/lightYaw.
		// Camera::update() rebuilds lightDirection and re-derives isNight /
		// nightFactor from lightPitch EVERY frame (camera.cpp:2185-2223), so the
		// sun really moves -- but the ambient/fog colors keep their mission
		// values. Use setDaylightFraction for a true day/night A/B and
		// setSunPitch to isolate light DIRECTION at constant color.
		//
		// Returns the resulting lightPitch in degrees.
		float setDaylightFraction (float f);
		float setSunPitch (float pitchDeg, bool setYaw = false, float yawDeg = 0.0f);

		// [MOUSE-ANCHORED-ZOOM-1 / FIX-4] perspective vs parallel projection.
		bool getUsePerspective (void)
		{
			return(usePerspective);
		}

		// [MOUSE-ANCHORED-ZOOM-1 v2] Arm the eased zoom anchor. anchoredZoom captures
		// the cursor world point A, the ACTUAL altitude h0, and the pivot T0 at the
		// wheel event; Camera::update() then shifts position toward A locked to the
		// eased altitude until it settles. See camera.cpp update().
		void beginZoomAnchor (const glm::vec3& A, float h0, const glm::vec3& T0)
		{
			if (h0 < 1.0f) h0 = 1.0f;
			zoomAnchorWorld = A;
			zoomAnchorPivot = T0;
			zoomAnchorH0    = h0;
			zoomAnchorActive = true;
		}

		void setCameraRotation (float angle, float angleWorld);
		float getCameraRotation (void);

		//---------------------------------------------------------------------------
		// projectZ: legacy screen-rect admission test + screen-XY oracle.
		//
		// `optionalResult` is a sidecar for diagnostics and future
		// replacement-candidate predicates. When nullptr (the default,
		// which every existing caller relies on), the function body is
		// byte-for-byte the legacy implementation -- the gated writes
		// constant-fold away. When non-null, the same arithmetic also
		// populates the result struct without altering operation order
		// in either branch.
		//
		// Spec: docs/superpowers/specs/2026-04-25-projectz-containment-design.md
		//---------------------------------------------------------------------------
		[[deprecated("Use an intent-specific projectFor*Admission/projectForScreenXY/etc. wrapper. "
		             "See docs/superpowers/specs/2026-04-26-projectz-policy-split-design.md.")]]
		bool projectZ (glm::vec3 &point, Stuff::Vector4D &screen,
		               LegacyProjectionResult* optionalResult = nullptr)
		{
			// F3 CPU projection cost-baseline: eventdriven projectZ
			// attribution. Count-only (no chrono); single TLS read + branch
			// + non-atomic counter bump. Per audit (c): no outer event
			// boundary exists, so count is the only signal.
			if (mc2_cpu_proj_cost::g_cpuProjEnabled &&
			    !mc2_cpu_proj_cost::tls_inRenderLoop) {
				mc2_cpu_proj_cost::add_workload_eventdriven_projectZ();
			}
			//--------------------------------------------------------------------
			// Now run the NEW project code
			Stuff::Vector4D xformCoords;
			glm::vec3 coords;
			mc2ToRender(coords, point);

			xformCoords.MultiplyPoint(coords, worldToClip);

			if (usePerspective)
			{
				//---------------------------------------
				// Perspective Transform
				float rhw = 1.0f;
				if (xformCoords.w != 0.0f)
					rhw = 1.0f / xformCoords.w;

				screen.x = (xformCoords.x * rhw) * viewMulX + viewAddX;
				screen.y = (xformCoords.y * rhw) * viewMulY + viewAddY;
				screen.z = (xformCoords.z * rhw);
				screen.w = fabs(rhw);
			}
			else
			{
				//---------------------------------------
				// Parallel Transform
				screen.x = (1.0f - xformCoords.x) * viewMulX + viewAddX;
				screen.y = (1.0f - xformCoords.y) * viewMulY + viewAddY;
				screen.z = xformCoords.z;
				screen.w = 0.000001f;
			}

			if ((screen.x < 0) || (screen.y < 0) || (screen.x > screenResolution.x) || (screen.y > screenResolution.y))
			{
				if (optionalResult)
				{
					optionalResult->acceptedByLegacyScreenRect = false;
					optionalResult->screen          = screen;
					optionalResult->rawClip         = xformCoords;
					optionalResult->signedW         = xformCoords.w;
					optionalResult->legacyRhw       = (xformCoords.w != 0.0f) ? (1.0f / xformCoords.w) : 1.0f;
					optionalResult->usePerspective  = usePerspective;
					optionalResult->trueSignedRhw   = (xformCoords.w != 0.0f) ? (1.0f / xformCoords.w) : FLT_MAX;
				}
				if (g_pzTrace)
				{
					// Read-and-clear the callsite ID globals before dispatch so a
					// missed PROJECTZ_SITE() at the next call produces <unknown>.
					const char* sid  = g_projectz_site_id;   g_projectz_site_id  = nullptr;
					const char* scat = g_projectz_site_cat;  g_projectz_site_cat = nullptr;
					projectz_trace_dispatch(sid, scat, point, xformCoords, screen,
					                        usePerspective, false,
					                        screenResolution.x, screenResolution.y);
				}
				return FALSE;
			}

			if (optionalResult)
			{
				optionalResult->acceptedByLegacyScreenRect = true;
				optionalResult->screen          = screen;
				optionalResult->rawClip         = xformCoords;
				optionalResult->signedW         = xformCoords.w;
				optionalResult->legacyRhw       = (xformCoords.w != 0.0f) ? (1.0f / xformCoords.w) : 1.0f;
				optionalResult->usePerspective  = usePerspective;
				optionalResult->trueSignedRhw   = (xformCoords.w != 0.0f) ? (1.0f / xformCoords.w) : FLT_MAX;
			}
			if (g_pzTrace)
			{
				const char* sid  = g_projectz_site_id;   g_projectz_site_id  = nullptr;
				const char* scat = g_projectz_site_cat;  g_projectz_site_cat = nullptr;
				projectz_trace_dispatch(sid, scat, point, xformCoords, screen,
				                        usePerspective, true,
				                        screenResolution.x, screenResolution.y);
			}
			return TRUE;
		}

		//---------------------------------------------------------------------------
		// projectZ Policy Split — intent-specific wrappers.
		// Spec: docs/superpowers/specs/2026-04-26-projectz-policy-split-design.md
		// All seven delegate to projectZ() unchanged. Behavior must be byte-identical.
		// Categories from projectz-callsite-inventory.md map 1:1 to wrappers.
		//---------------------------------------------------------------------------

		// Terrain vertex admission — bool gates submission; per-vertex wedge-risk concentration.
		inline bool projectForTerrainAdmission (glm::vec3& point,
		                                        Stuff::Vector4D& screen) {
			// R3 narrow-subset sidecar (see cpu_proj_cost_split.h).
			const int64_t _f3_terrain_t0 = ::mc2_cpu_proj_cost::terrain_admission_begin_ns();
#pragma warning(push)
#pragma warning(disable: 4996)
			bool accepted = projectZ(point, screen);
#pragma warning(pop)
#if defined(MC2_PROJECTZ_FINITE_CHECK)
			if (accepted) {
				gosASSERT(isfinite(screen.x) && isfinite(screen.y) &&
				          isfinite(screen.z) && isfinite(screen.w));
			}
#endif
			::mc2_cpu_proj_cost::terrain_admission_end_ns(_f3_terrain_t0);
			return accepted;
		}

		// Object lifecycle admission — bool feeds windowsVisible → canBeSeen() cull chain.
		inline bool projectForObjectAdmission (glm::vec3& point,
		                                       Stuff::Vector4D& screen) {
			// R2 cull-admission instrumentation. begin_ns returns 0 when env
			// OFF; end_ns short-circuits the same way. Inline pair is safe in
			// header (no class definition needed).
			const int64_t _f3_cull_t0 = ::mc2_cpu_proj_cost::cull_admission_begin_ns();

			ProjectZBypassMode bypassMode = projectZBypassMode();
			const bool isModern = (objectAdmissionPredicateMode() == ObjectAdmissionPredicateMode::Modern);
			bool ret;

			if (isModern && bypassMode == ProjectZBypassMode::Bypass) {
				// Pure bypass: skip projectZ entirely. screen stays uninitialized;
				// object_admission callers discard screen (verified Track A1/A2).
				ModernClipResult r = projectModernClipGL(point);
				ret = r.admit;
			} else {
				LegacyProjectionResult result;
#pragma warning(push)
#pragma warning(disable: 4996)
				// projectZ writes screen byte-identically to legacy; we capture rawClip
				// via the optionalResult sidecar so the modern predicate can see it.
				bool legacyAccepted = projectZ(point, screen, &result);
#pragma warning(pop)
#if defined(MC2_PROJECTZ_FINITE_CHECK)
				// Invariant gates on legacy-rect-acceptance (the original semantics),
				// not on the bool we ultimately return — preserves the policy-split
				// contract from commit cc83857.
				if (result.acceptedByLegacyScreenRect) {
					gosASSERT(isfinite(screen.x) && isfinite(screen.y) &&
					          isfinite(screen.z) && isfinite(screen.w));
				}
#endif
				if (isModern) {
					ret = clipSpaceFrustumAdmit(result.rawClip);
					if (bypassMode == ProjectZBypassMode::Compare) {
						ModernClipResult b = projectModernClipGL(point);
						if (b.admit != ret) {
							logProjectZBypassDisagreement("object", point, result.rawClip, ret, b.clip, b.admit);
						}
					}
				} else {
					ret = legacyAccepted;
				}
			}

			::mc2_cpu_proj_cost::cull_admission_end_ns(_f3_cull_t0);
			return ret;
		}

		// [LOW-CAMERA-OBJECT-CULL-1 / FIX-3] Sphere-aware object admission.
		// Identical to projectForObjectAdmission EXCEPT the Modern point-frustum
		// admit is replaced by clipSpaceFrustumAdmitSphere(rawClip, worldRadius,
		// projScale) so a whole object whose CENTER has crossed the near plane is
		// still admitted while its bounding sphere remains in front of the eye.
		// Gated by MC2_LOWCAM_OBJ_NEARPAD (default OFF). When OFF, or when the
		// predicate is in Legacy mode / radius<=0, behaviour is byte-identical to
		// projectForObjectAdmission. projScale = max column-xyz-norm (cols 0/1) of
		// worldToClip — the SAME matrix that produced rawClip (see projectZ:503),
		// matching the documented clipSpaceFrustumAdmitSphere contract / gpu_cull.comp.
		inline bool projectForObjectAdmissionSphere (glm::vec3& point,
		                                             Stuff::Vector4D& screen,
		                                             float worldRadius) {
			// Default ON for this low-camera build; set MC2_LOWCAM_OBJ_NEARPAD=0 to disable.
			static const bool s_lowcamObjNearPad = mc2::gate::get<"MC2_LOWCAM_OBJ_NEARPAD">();
			if (!s_lowcamObjNearPad || worldRadius <= 0.0f)
				return projectForObjectAdmission(point, screen);
			// [LOW-CAMERA-OBJECT-CULL-1 v2] getExtentRadius() is a tight extent; near
			// objects still pop at low pitch. Scale the near-plane pad radius. Default
			// 2.5x; MC2_LOWCAM_OBJ_NEARPAD_SCALE=k overrides (=1 = tight stock pad).
			static const float s_objNearPadScale = mc2::gate::get<"MC2_LOWCAM_OBJ_NEARPAD_SCALE">();
			worldRadius *= s_objNearPadScale;

			const int64_t _f3_cull_t0 = ::mc2_cpu_proj_cost::cull_admission_begin_ns();

			ProjectZBypassMode bypassMode = projectZBypassMode();
			const bool isModern = (objectAdmissionPredicateMode() == ObjectAdmissionPredicateMode::Modern);
			bool ret;

			if (!isModern) {
				// Legacy screen-rect path is unchanged by the near-pad slice.
				::mc2_cpu_proj_cost::cull_admission_end_ns(_f3_cull_t0);
				return projectForObjectAdmission(point, screen);
			}

			if (bypassMode == ProjectZBypassMode::Bypass) {
				// Pure bypass: GL-NDC clip. projScale from worldToClipGL (the GL
				// matrix that produced this clip). Note: clipSpaceFrustumAdmitSphere
				// uses the D3D [0,w] depth convention; the bypass GL clip is fed
				// through the same predicate as the strict bypass admit would be,
				// padded only on the near plane via the shared tol.
				ModernClipResult r = projectModernClipGL(point);
				const glm::mat4 M = worldToClipGL();
				const float projScale = objectNearPadProjScale(M);
				ret = clipSpaceFrustumAdmitSphere(r.clip, worldRadius, projScale);
			} else {
				LegacyProjectionResult result;
#pragma warning(push)
#pragma warning(disable: 4996)
				projectZ(point, screen, &result);
#pragma warning(pop)
				const float projScale = objectNearPadProjScale(worldToClip);
				ret = clipSpaceFrustumAdmitSphere(result.rawClip, worldRadius, projScale);
			}

			::mc2_cpu_proj_cost::cull_admission_end_ns(_f3_cull_t0);
			return ret;
		}

		// [LOW-CAMERA-OBJECT-CULL-1 / FIX-3] projScale helper: max over the x/y
		// clip-component columns of the xyz coefficient magnitude. Mirrors the
		// gpu_cull.comp derivation (there: row norms of the column-major viewProj;
		// here: column norms of the row-vector worldToClip). Depth-independent.
		static inline float objectNearPadProjScale (const glm::mat4& M) {
			auto colNorm = [&](int c) {
				const float a = M[c][0], b = M[c][1], d = M[c][2];
				return sqrtf(a * a + b * b + d * d);
			};
			const float nx = colNorm(0);
			const float ny = colNorm(1);
			return (nx > ny) ? nx : ny;
		}

		// Effect billboard admission — bool gates submission; same wedge-class hazard as terrain.
		inline bool projectForEffectAdmission (glm::vec3& point,
		                                       Stuff::Vector4D& screen) {
			// R3 narrow-subset sidecar (see cpu_proj_cost_split.h).
			const int64_t _f3_effect_t0 = ::mc2_cpu_proj_cost::effect_admission_begin_ns();

			ProjectZBypassMode bypassMode = projectZBypassMode();
			const bool isModern = (effectAdmissionPredicateMode() == EffectAdmissionPredicateMode::Modern);
			bool ret;

			if (isModern && bypassMode == ProjectZBypassMode::Bypass) {
				// Pure bypass: skip projectZ entirely. screen stays uninitialized;
				// effect_admission callers discard screen (verified F3 design).
				ModernClipResult r = projectModernClipGL(point);
				ret = r.admit;
			} else {
				LegacyProjectionResult result;
#pragma warning(push)
#pragma warning(disable: 4996)
				// projectZ writes screen byte-identically to legacy; we capture rawClip
				// via the optionalResult sidecar so the modern predicate can see it.
				bool legacyAccepted = projectZ(point, screen, &result);
#pragma warning(pop)
#if defined(MC2_PROJECTZ_FINITE_CHECK)
				// Invariant gates on legacy-rect-acceptance (the original semantics),
				// not on the bool we ultimately return. Same as Track A1's object wrapper.
				if (result.acceptedByLegacyScreenRect) {
					gosASSERT(isfinite(screen.x) && isfinite(screen.y) &&
					          isfinite(screen.z) && isfinite(screen.w));
				}
#endif
				if (isModern) {
					ret = clipSpaceFrustumAdmit(result.rawClip);
					if (bypassMode == ProjectZBypassMode::Compare) {
						ModernClipResult b = projectModernClipGL(point);
						if (b.admit != ret) {
							logProjectZBypassDisagreement("effect", point, result.rawClip, ret, b.clip, b.admit);
						}
					}
				} else {
					ret = legacyAccepted;
				}
			}

			::mc2_cpu_proj_cost::effect_admission_end_ns(_f3_effect_t0);
			return ret;
		}

		// Lighting / shadow activation — bool gates light->active; screen discarded.
		// F3 modernized via clipSpaceFrustumAdmit when MC2_LIGHTING_SHADOW_PREDICATE_MODE=Modern.
		inline bool projectForLightingShadow (glm::vec3& point,
		                                      Stuff::Vector4D& screen) {
			const int64_t _f3_lshadow_t0 = ::mc2_cpu_proj_cost::lighting_shadow_begin_ns();

			ProjectZBypassMode bypassMode = projectZBypassMode();
			const bool isModern = (lightingShadowPredicateMode() == LightingShadowPredicateMode::Modern);
			bool ret;

			if (isModern && bypassMode == ProjectZBypassMode::Bypass) {
				// Pure bypass: skip projectZ entirely. screen stays uninitialized;
				// lighting_shadow callers discard screen (light->active gating only).
				ModernClipResult r = projectModernClipGL(point);
				ret = r.admit;
			} else {
				LegacyProjectionResult result;
#pragma warning(push)
#pragma warning(disable: 4996)
				bool legacyAccepted = projectZ(point, screen, &result);
#pragma warning(pop)
#if defined(MC2_PROJECTZ_FINITE_CHECK)
				if (result.acceptedByLegacyScreenRect) {
					gosASSERT(isfinite(screen.x) && isfinite(screen.y) &&
					          isfinite(screen.z) && isfinite(screen.w));
				}
#endif
				if (isModern) {
					ret = clipSpaceFrustumAdmit(result.rawClip);
					if (bypassMode == ProjectZBypassMode::Compare) {
						ModernClipResult b = projectModernClipGL(point);
						if (b.admit != ret) {
							logProjectZBypassDisagreement("lighting_shadow", point, result.rawClip, ret, b.clip, b.admit);
						}
					}
				} else {
					ret = legacyAccepted;
				}
			}

			::mc2_cpu_proj_cost::lighting_shadow_end_ns(_f3_lshadow_t0);
			return ret;
		}

		// Picking — bool discarded by most callers; screen.xy consumed for distance / rect tests.
		// F3 modernized via clipSpaceFrustumAdmit when MC2_SELECTION_PICKING_PREDICATE_MODE=Modern.
		// screen output is byte-identical between Legacy and Modern (sidecar ptr doesn't affect
		// projectZ's screen-write path), so callers consuming screen.xy are unaffected.
		//
		// F5 T1: selection_picking now honors Bypass mode and produces screen.xy via
		// GL-NDC -> pixel remap (Y-down, Y-flip baked). Callers at camera.cpp:892,960,1026
		// use screen.x/y as pixel coords (Y-down from top-left, compared against
		// screenResolution and screenPos which are also pixel Y-down).
		inline bool projectForSelectionPicking (glm::vec3& point,
		                                        Stuff::Vector4D& screen) {
			const int64_t _f3_pick_t0 = ::mc2_cpu_proj_cost::selection_picking_begin_ns();

			ProjectZBypassMode bypassMode = projectZBypassMode();
			const bool isModern = (selectionPickingPredicateMode() == SelectionPickingPredicateMode::Modern);
			bool ret;

			if (isModern && bypassMode == ProjectZBypassMode::Bypass) {
				// F5 T1: Bypass-with-screen.xy. Compute GL-NDC clip directly, then
				// remap to MC2 pixel coords (Y-down) via viewport so the 3 callers
				// (camera.cpp:892,960,1026) see byte-(near-)identical screen.x/y.
				// gos_GetViewport: fullscreen -> vmx=width, vmy=height, vax=0, vay=0.
				// Y-flip: screen.y = vay + (1 - (ndc.y*0.5+0.5)) * vmy
				ModernClipResult r = projectModernClipGL(point);
				ret = r.admit;
				if (r.clip.w > 1e-4f) {
					float vmx, vmy, vax, vay;
					gos_GetViewport(&vmx, &vmy, &vax, &vay);
					float ndcX = r.clip.x / r.clip.w;
					float ndcY = r.clip.y / r.clip.w;
					float ndcZ = r.clip.z / r.clip.w;
					screen.x = vax + (ndcX * 0.5f + 0.5f) * vmx;
					screen.y = vay + (1.0f - (ndcY * 0.5f + 0.5f)) * vmy;
					screen.z = ndcZ;
					screen.w = r.clip.w;
				} else {
					// behind camera / degenerate
					screen.x = 0.0f;
					screen.y = 0.0f;
					screen.z = 0.0f;
					screen.w = r.clip.w;
				}
			} else {
				LegacyProjectionResult result;
#pragma warning(push)
#pragma warning(disable: 4996)
				bool legacyAccepted = projectZ(point, screen, &result);
#pragma warning(pop)
#if defined(MC2_PROJECTZ_FINITE_CHECK)
				if (result.acceptedByLegacyScreenRect) {
					gosASSERT(isfinite(screen.x) && isfinite(screen.y) &&
					          isfinite(screen.z) && isfinite(screen.w));
				}
#endif
				if (isModern) {
					ret = clipSpaceFrustumAdmit(result.rawClip);
					if (bypassMode == ProjectZBypassMode::Compare) {
						// F5 T1: compare both admit parity AND screen.xy parity for picking.
						ModernClipResult b = projectModernClipGL(point);
						bool bypassAdmit = b.admit;
						if (bypassAdmit != ret) {
							logProjectZBypassDisagreement("selection_picking", point, result.rawClip, ret, b.clip, bypassAdmit);
						}
						if (b.clip.w > 1e-4f) {
							float vmx, vmy, vax, vay;
							gos_GetViewport(&vmx, &vmy, &vax, &vay);
							float ndcX = b.clip.x / b.clip.w;
							float ndcY = b.clip.y / b.clip.w;
							float bypassScreenX = vax + (ndcX * 0.5f + 0.5f) * vmx;
							float bypassScreenY = vay + (1.0f - (ndcY * 0.5f + 0.5f)) * vmy;
							float dxPx = fabsf(bypassScreenX - screen.x);
							float dyPx = fabsf(bypassScreenY - screen.y);
							if (dxPx > 1.0f || dyPx > 1.0f) {
								logSelectionPickingScreenDelta(point, screen, bypassScreenX, bypassScreenY, dxPx, dyPx);
							}
						}
					}
				} else {
					ret = legacyAccepted;
				}
			}

			::mc2_cpu_proj_cost::selection_picking_end_ns(_f3_pick_t0);
			return ret;
		}

		// Cosmetic screen-XY oracle -- bool discarded by all 16 callers; only
		// screen.xy consumed for HUD/UI positioning. F5 T2: optional Bypass
		// honors projectModernClipGL + viewport remap (same convention as
		// selection_picking from F5 T1). Pixel parity is mandatory; Compare
		// mode validates pre-flip. Default: Legacy (conservative).
		// Env MC2_SCREENXY_PREDICATE_MODE=Modern + MC2_PROJECTZ_BYPASS_MODE=Compare
		// logs pixel-delta events when bypass screen.xy disagrees >1px.
		inline bool projectForScreenXY (glm::vec3& point,
		                                Stuff::Vector4D& screen) {
			const int64_t _f3_sxy_t0 = ::mc2_cpu_proj_cost::screenxy_begin_ns();

			ProjectZBypassMode bypassMode = projectZBypassMode();
			const bool isModern = (screenXYPredicateMode() == ScreenXYPredicateMode::Modern);
			bool ret;

			if (isModern && bypassMode == ProjectZBypassMode::Bypass) {
				// Bypass with screen.xy via viewport remap.
				// gos_GetViewport: fullscreen -> vmx=width, vmy=height, vax=0, vay=0.
				// Y-flip: screen.y = vay + (1 - (ndc.y*0.5+0.5)) * vmy
				ModernClipResult r = projectModernClipGL(point);
				ret = r.admit;
				if (r.clip.w > 1e-4f) {
					float vmx, vmy, vax, vay;
					gos_GetViewport(&vmx, &vmy, &vax, &vay);
					float ndcX = r.clip.x / r.clip.w;
					float ndcY = r.clip.y / r.clip.w;
					float ndcZ = r.clip.z / r.clip.w;
					screen.x = vax + (ndcX * 0.5f + 0.5f) * vmx;
					screen.y = vay + (1.0f - (ndcY * 0.5f + 0.5f)) * vmy;
					screen.z = ndcZ;
					screen.w = r.clip.w;
				} else {
					screen.x = 0.0f; screen.y = 0.0f; screen.z = 0.0f; screen.w = r.clip.w;
				}
			} else {
#pragma warning(push)
#pragma warning(disable: 4996)
				LegacyProjectionResult result;
				bool legacyAccepted = projectZ(point, screen, &result);
#pragma warning(pop)
				if (isModern) {
					ret = clipSpaceFrustumAdmit(result.rawClip);
					if (bypassMode == ProjectZBypassMode::Compare) {
						ModernClipResult b = projectModernClipGL(point);
						if (b.clip.w > 1e-4f) {
							float vmx, vmy, vax, vay;
							gos_GetViewport(&vmx, &vmy, &vax, &vay);
							float ndcX = b.clip.x / b.clip.w;
							float ndcY = b.clip.y / b.clip.w;
							float bypassScreenX = vax + (ndcX * 0.5f + 0.5f) * vmx;
							float bypassScreenY = vay + (1.0f - (ndcY * 0.5f + 0.5f)) * vmy;
							float dxPx = fabsf(bypassScreenX - screen.x);
							float dyPx = fabsf(bypassScreenY - screen.y);
							if (dxPx > 1.0f || dyPx > 1.0f) {
								logScreenXYScreenDelta(point, screen, bypassScreenX, bypassScreenY, dxPx, dyPx);
							}
						}
					}
				} else {
					ret = legacyAccepted;
				}
			}

			::mc2_cpu_proj_cost::screenxy_end_ns(_f3_sxy_t0);
			return ret;
		}

		// Debug overlays — LAB_ONLY / drawTerrainGrid-gated draw paths.
		// F3 modernized via clipSpaceFrustumAdmit when MC2_DEBUG_OVERLAY_PREDICATE_MODE=Modern.
		inline bool projectForDebugOverlay (glm::vec3& point,
		                                    Stuff::Vector4D& screen) {
			const int64_t _f3_dbg_t0 = ::mc2_cpu_proj_cost::debug_overlay_begin_ns();

			ProjectZBypassMode bypassMode = projectZBypassMode();
			const bool isModern = (debugOverlayPredicateMode() == DebugOverlayPredicateMode::Modern);
			bool ret;

			if (isModern && bypassMode == ProjectZBypassMode::Bypass) {
				// Pure bypass: skip projectZ entirely. screen stays uninitialized;
				// debug_overlay callers discard screen (LAB_ONLY draw paths).
				ModernClipResult r = projectModernClipGL(point);
				ret = r.admit;
			} else {
				LegacyProjectionResult result;
#pragma warning(push)
#pragma warning(disable: 4996)
				bool legacyAccepted = projectZ(point, screen, &result);
#pragma warning(pop)
#if defined(MC2_PROJECTZ_FINITE_CHECK)
				if (result.acceptedByLegacyScreenRect) {
					gosASSERT(isfinite(screen.x) && isfinite(screen.y) &&
					          isfinite(screen.z) && isfinite(screen.w));
				}
#endif
				if (isModern) {
					ret = clipSpaceFrustumAdmit(result.rawClip);
					if (bypassMode == ProjectZBypassMode::Compare) {
						ModernClipResult b = projectModernClipGL(point);
						if (b.admit != ret) {
							logProjectZBypassDisagreement("debug_overlay", point, result.rawClip, ret, b.clip, b.admit);
						}
					}
				} else {
					ret = legacyAccepted;
				}
			}

			::mc2_cpu_proj_cost::debug_overlay_end_ns(_f3_dbg_t0);
			return ret;
		}

		void projectCamera (glm::vec3 &point);

		// Read-only view of the world->clip matrix. Used by the per-frame
		// inverseProject delta-cache (VPL-retirement Step 3 3b) to detect a
		// camera-matrix change via memcmp without re-projecting every frame.
		const glm::mat4& getWorldToClip (void) const { return worldToClip; }

		// F1 unified-projection: single composition source for runtime GPU
		// uniform path. axisSwap * worldToCameraMatrix * cameraToClip,
		// post-axisSwap GL convention. Distinct from `Camera::worldToClip`
		// (pre-axisSwap; feeds projectZ body and 8 wrappers). See spec
		// 2026-05-22 §0.1 invariant.
		glm::mat4 worldToClipGL() const;

		// HDRI-SKY-1: expose view matrix component (worldToCameraMatrix) separately
		// for sky rendering. Used by GameAdapters::Sky::renderHdri alongside
		// cameraToClipGL_const(). See docs/superpowers/explorations/2026-05-25-hdri-sky-matrix-source.md.
		const glm::mat4& worldToCameraGL() const { return worldToCameraMatrix; }

		// HDRI-SKY-1: expose projection matrix component separately for sky rendering.
		// Used by GameAdapters::Sky::renderHdri alongside worldToCameraGL(). Both
		// matrices are column-major (GL convention) and safe to cast to float* for GPU.
		const glm::mat4& cameraToClipGL_const() const { return cameraToClipGL; }

		// F1-3A ViewUniforms: view matrix without projection component.
		//   = kAxisSwapMC2toGL * worldToCameraMatrix
		// Parallel to worldToClipGL() but stops before multiplying cameraToClipGL.
		glm::mat4 worldToViewGL() const;

		// F1-3A ViewUniforms: camera world position in GL coordinate space.
		// Applies the MC2->GL axis swap (x'=-x, y'=z, z'=y) to physicalPos.
		glm::vec3 cameraOriginGL() const;

		// Pick screen-rect cache invalidation key. Increments when worldToClip
		// changes (position/rotation/FOV/viewport). 0 = uninitialized.
		uint32_t getViewProjectionRevision() const { return viewProjectionRevision_; }

		// F4 projectZ-bypass helper. Computes clip directly via worldToClipGL()
		// for a single world point. Used by the 5 Modern-default wrappers when
		// MC2_PROJECTZ_BYPASS_MODE = Compare or Bypass. Does NOT touch
		// cameraToClip / projectZ / worldToClip (legacy paths preserved).
		ModernClipResult projectModernClipGL(const glm::vec3& world) const;

		// Shared CPU camera-frustum x quad-AABB primitive (VPL-retirement Step 3 3a
		// OWNS the definition; Step 5B references it). Pure CPU, no GL, no readback.
		// extractFrustumPlanes: Gribb-Hartmann 6-plane extraction from worldToClip,
		// with the projection swizzle s=(-wx,wz,wy) folded into each plane so the
		// returned planes test RAW world AABBs (built from vertices[].vx/.vy/.elevation).
		void extractFrustumPlanes (float planes[6][4]) const;
		// quadAabbInFrustum: conservative p-vertex AABB-vs-frustum test. Never
		// false-negative (may false-positive slightly outside - acceptable).
		bool quadAabbInFrustum (const float planes[6][4],
		                        const glm::vec3& mn,
		                        const glm::vec3& mx) const;

		// [LOW-CAMERA-TERRAIN-CULL-1 / FIX-2] As quadAabbInFrustum, but omits the
		// near plane (index 4 in the {left,right,bottom,top,near,far} order) so the
		// terrain LOD-chunk cull does not drop terrain near the eye at a low /
		// grazing pitch. The other 5 planes (left/right/top/bottom/far) are tested
		// identically — this is NOT a global cull-widen. Terrain-only; gated.
		bool quadAabbInFrustumSkipNear (const float planes[6][4],
		                                const glm::vec3& mn,
		                                const glm::vec3& mx) const;

		// F6 T2: per-frame frustum-planes cache, populated by Terrain::geometry
		// once per frame before the setupTextures loop. Shared with
		// TerrainQuad::setupTextures' water-corner admission path.
		// Lifecycle: written by cacheFrustumPlanes(); read via
		// getCachedFrustumPlanes(). NOT thread-safe; assumes single-threaded
		// terrain admission (which it is today).
		void cacheFrustumPlanes();
		const float (*getCachedFrustumPlanes() const)[4];

		// WARNING: inverseProject is a LEGACY TERRAIN PICKER. It may scan
		// thousands of terrain quads (getNumQuads) with a per-quad projection.
		// NEVER call it from paint/input/UI hot paths (mouse-move, wheel, paint,
		// scrollbar sync, minimap repaint) — doing so froze the editor for tens
		// of seconds on large maps. Those affordances use the O(1)
		// screenToGroundPlaneApprox() below. Reserve inverseProject for discrete
		// events that genuinely need the exact terrain hit (e.g. a click).
		//
		// LESSON: never call terrain picking from paint/input UI affordances.
		unsigned long inverseProject (Stuff::Vector2DOf<long> &screenPos, glm::vec3 &point);

		// screenToGroundPlaneApprox: O(1) screen-pixel -> world point on the
		// z=0 ground plane via the inverse clip matrix + ray/plane intersect.
		// NOT a terrain pick: does no quad scan and ignores elevation. Use for
		// cheap, approximate overlays (e.g. the tacmap view rectangle) where
		// exact terrain intersection is unnecessary. Do NOT substitute for
		// inverseProject() when a real terrain hit is required.
		bool screenToGroundPlaneApprox (long screenX, long screenY, glm::vec3 &outWorld, float z_plane = 0.0f);

		// screenToTerrainApprox: screenToGroundPlaneApprox refined onto the
		// terrain surface by iterating the plane height (fixed 3 steps, O(1),
		// no quad scan).
		// WARNING: approx unprojectors compute a ray-plane intersection (z-plane
		// or iterated terrain step) and diverge from the actual terrain surface on
		// elevated or sloped terrain. The old Matrix4D::Invert path was numerically
		// broken for reverse-Z (replaced by lowCamInvert4x4 in 4942e7d2), so the
		// math is now correct, but the z-plane approximation remains incorrect for
		// terrain sculpting. For any paint/pick that requires the exact terrain
		// surface hit use inverseProject (forward-projection quad scanner).
		// These approx functions remain for non-sculpt editor paths and cursor
		// overlay (brush centre display), where O(1) performance matters.
		bool screenToTerrainApprox (long screenX, long screenY, glm::vec3 &outWorld);

		// getClosestVertex: screen-click -> terrain vertex (row,col).
		// Reinstated 2026-05-24 for the EditRel Mission Editor (sole caller
		// editor/TerrainBrush.h). Thin adapter over Camera::inverseProject +
		// Terrain::worldToTile. Does NOT restore the stale-px/py scan that
		// VPL Step 8b deleted. Definition in camera.cpp.
		void getClosestVertex (Stuff::Vector2DOf<long>& screenPos,
		                       long& row, long& col);

		void setOrthogonal(void);
		virtual void setCameraOrigin (void);
		void calculateProjectionConstants (void);
		void calculateTopViewConstants (void);

		void changeResolution (glm::vec3 newRes)
		{
			screenResolution = newRes;
			// The pixel-space scale (viewMulX/Y) is what projectZ() and the legacy
			// inverseProject tile-scan actually multiply NDC by — it is NOT updated
			// by setting screenResolution alone. Camera::render() sources it from
			// gos_GetViewport (the FULL window), so in the editor's shrunk-viewport
			// path that leaves viewMulX pinned at the full-window width while
			// screenResolution + the clip matrix are shrunk -> picks divide by the
			// shrunk width but project by the full width => a fullW/shrunkW (~2x)
			// divergence (placement cursor drifts toward center at the map edge).
			// Re-apply the scale from newRes so all three (screenResolution, clip
			// matrix, pixel scale) agree. changeResolution is editor-only (no game
			// caller), so this cannot affect the game's Camera::render() path.
			viewMulX = newRes.x;
			viewMulY = newRes.y;
			viewAddX = 0.0f;
			viewAddY = 0.0f;
			calculateProjectionConstants();
		}

		void prepareBackground (void);

		CameraClass getClass (void)
		{
			return cameraClass;
		}

		void setClass(CameraClass newClass) {cameraClass = newClass;}
		
		glm::vec3 getPosition (void)
		{
			return position;
		}

		float getCameraShiftZ (void) { return cameraShiftZ; }

		void updateDaylight (bool bInitialize = false);
		
		virtual long update (void);
		virtual void render (void);

		virtual long activate (void);
		virtual void reset (void)
		{
		
		}
		
		void deactivate (void);
		
		void setLightColor (long index, DWORD argb)
		{
			if ((index >= 0) && (index < numLights) && worldLights[index])
				worldLights[index]->SetaRGB(argb);
		}
		
		DWORD getLightColor (long index)
		{
			if ((index >= 0) && (index < numLights) && worldLights[index])
				return worldLights[index]->GetaRGB();

			return 0x00ffffff;
		}

		void setLightIntensity (long index, float intensity)
		{
			if ((index >= 0) && (index < numLights) && worldLights[index])
				worldLights[index]->intensity = intensity;
		}
		
		//Return the ACTIVE lights.  NOT the total light list!!
		TG_LightPtr getWorldLight (long index)
		{
			if ((index >= 0) && (index < numActiveLights) && activeLights[index])
				return activeLights[index];
				
			return nullptr;
		}

		TG_LightPtr *getWorldLights (void)
		{
			return activeLights;
		}

		long getNumLights (void)
		{
			return numActiveLights;
		}

		// STATIC-REG-PREWARM-QUEUE-1: populate activeLights before any render frame.
		// updateLights() is private; this wrapper allows mission-load callers to prime
		// the active light list so SetLightList receives non-zero lights at prewarm time.
		void primeActiveLightsForPrewarm()
		{
			updateLights();
		}

		TG_LightPtr getTerrainLight (long index)
		{
			// Defense in depth: guard the null array base before indexing it —
			// numTerrainLights and terrainLights are set together in init(), but a
			// not-yet-init'd camera can have a stale/garbage count with a NULL array
			// (see ctor note). Without the `terrainLights &&` check this indexes NULL.
			if ((index >= 0) && (index < numTerrainLights) && terrainLights && terrainLights[index])
				return terrainLights[index];
				
			return nullptr;
		}

		TG_LightPtr *getTerrainLights (void)
		{
			return terrainLights;
		}
		
		long getNumTerrainLights (void)
		{
			return numTerrainLights;
		}

		long addWorldLight (TG_LightPtr light)
		{
			numLights = MAX_LIGHTS_IN_WORLD;
			bool lightAdded = false;
			for (long i=0;i<MAX_LIGHTS_IN_WORLD;i++)
			{
				if (!worldLights[i])
				{
					worldLights[i] = light;
					lightAdded = true;
					return i;
				}
			}
			
			return -1;
		}

		void removeWorldLight (DWORD lightNum, TG_LightPtr light)
		{
			if ((lightNum < MAX_LIGHTS_IN_WORLD) && (worldLights[lightNum] == light))
			{
				worldLights[lightNum] = nullptr;			//Up to class that created light to free it!!!!!!

                // sebi: ORG BUG FIX because worldLights are copied to activeLights and terrainLights in eye->update() and then later this function can be called, this means that activeLights and terrainLights may point to freed memory, so call this to fox it
                updateLights();

				return;
			}

			//If we get here, the light we passed in either had an invalid index OR did not match the pointer
			// at the location we passed in.  Scan all lights for a match and toss any and all that do match
			for (long i=0;i<MAX_LIGHTS_IN_WORLD;i++)
			{
				if (worldLights[i] == light)
					worldLights[i] = nullptr;
			}

            // sebi: ORG BUG FIX because worldLights are copied to activeLights and terrainLights in eye->update() and then later this function can be called, this means that activeLights and terrainLights may point to freed memory, so call this to fox it
            updateLights();
		}


		glm::vec3 getScreenRes (void)
		{
			return screenResolution;
		}

		bool getIsNight (void)
		{
			return isNight;
		}
		
		float getNightFactor (void)
		{
			return nightFactor;
		}

		// TERRAIN-AMBIENT-FLOOR-2: the AUTHORED sun pitch, in degrees, as it
		// came out of the mission camera file (LightDirPitch, or DayLightPitch
		// on the DayToNightTime missions).
		//
		// CORRECTED 2026-07-30 (DAYLIGHT-GATE-RECHECK-1). This comment used to
		// say lightPitch "is overwritten at camera.cpp:2033 with the mirrored
		// MOON pitch whenever the authored sun is below the horizon, after which
		// isNight, nightFactor and lightDirection.z all describe the moon and
		// report 'day'." That does not happen. The fold lives at
		// camera.cpp:2079-2080 inside updateDaylight(), and updateDaylight()
		// never runs in a mission: GameCamera::activate (code/gamecam.cpp:1024)
		// overrides the virtual base (camera.h:1285) without chaining and has its
		// own call commented out at gamecam.cpp:1060, and the per-frame site is
		// commented out at camera.cpp:2238. lightPitch therefore keeps its
		// authored value, and Camera::update()'s per-frame re-derivation
		// (camera.cpp:2254-2281) yields the CORRECT night verdict -- e.g. mc2_05
		// authors 270, altitude 180-270 = -90, isNight = true.
		//
		// dayLightPitch is still the right thing to read here: it is written only
		// at load, never folded and never animated, so it is a stable per-mission
		// record that stays correct even if the fold is ever revived. Read-only
		// accessor; no behaviour change. See GameOS/gameos/utils/terrain_daylight.h.
		float getDayLightPitch (void)
		{
			return dayLightPitch;
		}
		
 		long getScreenResX (void)
		{
			return float2long(screenResolution.x);
		}
		
		long getScreenResY (void)
		{
			return float2long(screenResolution.y);
		}

		float fgetScreenResX (void)
		{
			return screenResolution.x;
		}
		
		float fgetScreenResY (void)
		{
			return screenResolution.y;
		}
		
		float getScaleFactor (void)
		{
			return newScaleFactor;
		}

		long getScaleLOD (void)
		{
			for (long i=0;i<MAX_LODS;i++)
			{
				if (newScaleFactor >= zoomLevelLODScale[i])
					return i;
			}
			
			return (MAX_LODS-1);		//ALWAYS return worst case if necessary
		}
		
		//-----------------------------------------
		// Returns Vector in Direction of Camera.
		// For BackFacing Object Checks
		glm::vec3 getLookVector (void)
		{
			return lookVector;
		}

		glm::vec3 getCameraOrigin (void)
		{
			return physicalPos;
		}
		
		float getTiltFactor (void)
		{
			float result = 1.0;
			if (usePerspective)
			{
				result = 0.5f + ((projectionAngle - 28.0f) * 0.016666667f * 0.5);	//1/60th
			}
			
			return result;
		}
		
		//----------------------------------------------
		// Camera Positioning Functions
		void setCameraView (long viewNum);
		
		void allNormal (void)
		{
			tiltNormal();
			ZoomNormal();
			rotateNormal();
		}
		
		void allDefault (void)
		{
			ZoomDefault();
		}
		
		void allMaxIn (void)
		{
			ZoomMax();
		}

		virtual void allTight (void)
		{
			ZoomTight();
		}
				
		void zoomValue (float value);
  		void ZoomIn (float amount);
		void ZoomOut (float amount);
		void ZoomNormal (void);
		void ZoomDefault (void);
		void ZoomTight (void);
		void ZoomMax (void);
		void ZoomMin (void);

		void tiltValue (float value);
		void tiltUp (float amount);
		void tiltDown (float amount);
		void tiltNormal (void);
		
		void movePosLeft(float amount, glm::vec3 &pos);
		void movePosRight(float amount, glm::vec3 &pos);
		void movePosUp(float amount, glm::vec3 &pos);
		void movePosDown(float amount, glm::vec3 &pos);
		
		virtual void moveLeft(float amount);
		virtual void moveRight(float amount);
		virtual void moveUp(float amount);
		virtual void moveDown(float amount);
		
		void rotateLeft(float amount);
		void rotateRight(float amount);

		// [CAMERA-SMOOTHING-1] input accumulators (see camera.cpp).
		void applyScrollDelta(float dx, float dy);
		void queueRotationDelta(float amount);
		void updateInputSmoothing(glm::vec3 &newPosition);
		static bool inputSmoothingEnabled(void);	// MC2_CAMERA_SMOOTH > 0

		void rotateNormal (void);

		void rotateLightLeft(float amount);
		void rotateLightRight(float amount);
		void rotateLightUp(float amount);
		void rotateLightDown(float amount);

		unsigned char getLightRed (float intensity);
		unsigned char getLightGreen (float intensity);
		unsigned char getLightBlue (float intensity);

		void setPosition(glm::vec3 newPosition, bool swoopy = true);

		bool save( FitIniFile* fileName );

		float getFarClipDistance();
		void setFarClipDistance(float farClipDistance);
		float getNearClipDistance();
		void setNearClipDistance(float nearClipDistance);
		float getMaximumCameraAltitude();
		void setMaximumCameraAltitude(float maxAltitude);

		//--------------------------------------------
		// Camera Scripting API
		void setMovieMode (void)
		{
			if (!inMovieMode)
			{
				inMovieMode = true;
				startEnding = false;
				forceMovieEnd = false;
				letterBoxPos = 0.0f;		//Start letterboxing;
				letterBoxTime = 0.0f;
			}
		}
		
		void endMovieMode (void)
		{
			if (inMovieMode)
				startEnding = true;			//Start the letterbox shrinking.  When fully shrunk, inMovieMode goes false!
		}

		void fadeToColor (DWORD color, float timeToFade)
		{
			if (!inFadeMode && (timeToFade > Stuff::SMALL))
			{
				inFadeMode = true;
				fadeStart = fadeColor;
				fadeColor = color;
				timeLeftToFade = 0.0f;
				startFadeTime = timeToFade;
			}
		}
		
		void forceMovieToEnd (void)	//ABL Script must handle this going true!!!!
		{
			forceMovieEnd = true;
		}
		
		void setFieldOfView (float fov)
		{
			camera_fov = fov;
		}
		
		float getFieldOfView (void)
		{
			return camera_fov;
		}
		
		void setFieldOfViewGoal (float fov, float time)
		{
			goalFOV = fov;
			goalFOVTime = time;
		}
		
		float getFieldOfViewGoal (void)
		{
			return goalFOV;
		}
				
		void setGoalPosition (glm::vec3 goalPos)
		{
			goalPosition = goalPos;
		}

		void setLookPosition (glm::vec3 goalPos)
		{
			lookPosition = goalPos;
		}

		glm::vec3 getGoalPosition (void)
		{
			return goalPosition;
		}
		
		glm::vec3 getLookPosition (void)
		{
			return lookPosition;
		}
		
 		void setGoalPosTime (float time)
		{
			goalPosTime = time;
		}

		glm::vec3 getRotation (void)
		{
			glm::vec3 rot(0.0,0.0,0.0);
			rot.x = projectionAngle;
			rot.y = cameraRotation;
			
			return rot;
		}
		
		void setRotation (glm::vec3 rot)
		{
			projectionAngle = rot.x;
			setCameraRotation(rot.y,rot.y);
		
			cameraAltitude = rot.z;
			if (cameraAltitude < AltitudeMinimum)
				cameraAltitude = AltitudeMinimum;
					
			float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
			float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);
		
			newScaleFactor = 1.0f - ((cameraAltitude - AltitudeMinimum) / testMax);
		}
		
		// VK-FIRST-BOOKMARK-POSE-1 (ported for CAMERA-PARK-1): neutralize every
		// scripted camera-override source so a hard-set pose survives the NEXT
		// Camera::update() byte-unchanged. Camera::update() re-asserts the
		// intro-swoop goal (updateGoalPosition/updateGoalRotation) and the
		// object-tracking snap every frame — which would drag the parked pose
		// off its applied value while the mission-intro swoop goal drains.
		// Called right after setPosition/setRotation. Virtual so GameCamera can
		// also drop its object-tracking target.
		virtual void freezeScriptedCameraForCapture (void)
		{
			lookTargetObject = -1;
			goalPosition   = position;
			goalPosTime    = 0.0f;
			goalPositionZ  = position.z;
			goalRotation.x = projectionAngle;
			goalRotation.y = cameraRotation;
			goalRotation.z = cameraAltitude;
			goalRotTime    = 0.0f;
			goalFOVTime    = 0.0f;
			velocity = glm::vec3(0.0f);
			goalVelocity = glm::vec3(0.0f);
			goalVelTime    = 0.0f;
			// Pin the altitude EASE target too: update() lerps cameraAltitude
			// toward cameraAltitudeDesired each frame, so leaving Desired stale
			// would drift altitude off the applied pose. (The engine still
			// legitimately clamps cameraAltitude to the per-projection-angle max,
			// which is expected and deterministic.)
			cameraAltitudeDesired = cameraAltitude;
			// CAMERA-SMOOTHING-1 interop (2026-07-22): kill any in-flight
			// smoothing glide too — the rebase hook composes external deltas
			// into these goals, which would otherwise keep sliding the camera
			// off the parked pose after the hard set.
			scrollSmoothActive = false;
			rotSmoothActive = false;
		}

		void setGoalRotation (glm::vec3 goalRot)
		{
			goalRotation = goalRot;
		}
		
		glm::vec3 getGoalRotation (void)
		{
			return goalRotation;
		}
		
 		void setGoalRotTime (float time)
		{
			goalRotTime = time;
		}

		glm::vec3 getVelocity (void)
		{
			return velocity;
		}
		
 		void setVelocity (glm::vec3 vel)
		{
			velocity = vel;
		}
		
		glm::vec3 getGoalVelocity (void)
		{
			return goalVelocity;
		}
		
  		void setGoalVelocity (glm::vec3 goalVel)
		{
			goalVelocity = goalVel;
		}
		
		void setGoalVelTime (float time)
		{
			goalVelTime = time;
		}
		
		void updateGoalVelocity(void);
		void updateGoalPosition (glm::vec3 &pos);
		void updateGoalRotation (void);
		void updateGoalFOV (void);
		
		void updateLetterboxAndFade (void);
		
		void setCameraTargetId (long targId)
		{
			lookTargetObject = targId;
		}
		
		long getCameraTargetId (void)
		{
			return lookTargetObject;
		}
		
};		

//---------------------------------------------------------------------------
extern CameraPtr eye;

//---------------------------------------------------------------------------
#endif

//---------------------------------------------------------------------------
//
// Edit log
//
//---------------------------------------------------------------------------
